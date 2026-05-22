// SPDX-License-Identifier: GPL-3.0-or-later
// SixBack — Verwaltungs-REST-API (Port 80)
//
// Vollstaendige Speaker-/Preset-/Gruppen-Verwaltung. Frontend (Web-UI)
// nutzt diese Endpoints und bekommt damit alle Funktionalitaet:
//   - Speaker entdecken + listen
//   - Pro Speaker: Migration / Revert / Reboot
//   - Pro Speaker: 6 Presets verwalten
//   - Gruppen-Sync (Preset-Set von Speaker A auf B,C,...)
//   - TuneIn-Lookup + Suche

#include "api_endpoints.h"
#include "version.h"
#include "config.h"
#include "speaker_telnet.h"
#include "wifi_provisioning.h"
#include "speaker_inventory.h"
#include "preset_store.h"
#include "tunein_resolver.h"
#include "system_health.h"
#include "captive_portal.h"
#include "ota_pull.h"
#include "auto_mode.h"
#include "source_normalizer.h"
#include "bose_endpoints.h"
#include "event_store.h"
#include "speaker_diagnostic.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <LittleFS.h>
#include <Update.h>
#include <Preferences.h>

namespace {

// -----------------------------------------------------------------------------
// JSON-Body-Akkumulator fuer POST/PUT — Helper
// -----------------------------------------------------------------------------
using JsonHandler = std::function<void(AsyncWebServerRequest*, JsonDocument&)>;

void routeJsonBody(AsyncWebServer& s, const String& uri, WebRequestMethodComposite m,
                   JsonHandler cb) {
    s.on(uri.c_str(), m,
        [](AsyncWebServerRequest* req){ /* finalized by body handler */ },
        nullptr,
        [cb](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total){
            if (!req->_tempObject) {
                req->_tempObject = new String();
                ((String*)req->_tempObject)->reserve(total + 1);
                // Framework-Destructor ruft free() auf _tempObject; das kennt
                // den String-Heap-Buffer nicht und leakt ihn bei Abbruch.
                // onDisconnect ueberbruecken: korrekt deleten + nullen, damit
                // free(nullptr) im Destructor harmlos ist.
                req->onDisconnect([req]() {
                    if (req->_tempObject) {
                        delete static_cast<String*>(req->_tempObject);
                        req->_tempObject = nullptr;
                    }
                });
            }
            String* buf = (String*)req->_tempObject;
            buf->concat((const char*)data, len);
            if (idx + len == total) {
                JsonDocument doc;
                auto err = deserializeJson(doc, *buf);
                delete buf;
                req->_tempObject = nullptr;
                if (err) { req->send(400, "application/json",
                    String("{\"error\":\"bad json: ") + err.c_str() + "\"}"); return; }
                cb(req, doc);
            }
        });
}

String myBaseUrl() {
    return "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
}

// RFC3986-percent-encode fuer Query-Parameter. Nur die unreservierten
// ASCII-Zeichen bleiben unverwandelt; alles andere wird %XX. Speziell
// '&' muss escaped werden, sonst splittet ein User-Sucher wie "Bob & Friends"
// die TuneIn-Search-URL in zwei Query-Parameter.
String urlEncodeQuery_(const String& in) {
    String out;
    out.reserve(in.length() * 3);
    for (size_t i = 0; i < in.length(); ++i) {
        unsigned char c = (unsigned char)in.charAt(i);
        bool unreserved = (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out += (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// GET /api/status  — System-Health
// -----------------------------------------------------------------------------
void handleStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["name"]       = FW_NAME;
    doc["version"]    = FW_VERSION_STRING;
    doc["build"]      = FW_BUILD_DATE;
    doc["uptime_s"]   = millis() / 1000;

    JsonObject wifi   = doc["wifi"].to<JsonObject>();
    wifi["connected"] = WiFi.status() == WL_CONNECTED;
    wifi["ssid"]      = WiFi.SSID();
    wifi["ip"]        = WiFi.localIP().toString();
    wifi["rssi"]      = WiFi.RSSI();
    wifi["mac"]       = WiFi.macAddress();
    wifi["hostname"]  = String(MDNS_HOSTNAME) + ".local";
    wifi["improv_active"]   = sixback::improvIsActive();
    wifi["improv_window_s"] = sixback::improvWindowRemainingS();
    wifi["captive_active"]   = sixback::captiveIsActive();
    wifi["captive_window_s"] = sixback::captiveWindowRemainingS();

    JsonObject heap   = doc["heap"].to<JsonObject>();
    heap["free"]      = ESP.getFreeHeap();
    heap["min_free"]  = ESP.getMinFreeHeap();
    heap["total"]     = ESP.getHeapSize();
    JsonObject psram  = doc["psram"].to<JsonObject>();
    psram["free"]     = ESP.getFreePsram();
    psram["total"]    = ESP.getPsramSize();
    esp_chip_info_t info; esp_chip_info(&info);
    JsonObject chip   = doc["chip"].to<JsonObject>();
    chip["model"]     = info.model == CHIP_ESP32S3 ? "ESP32-S3" :
                        info.model == CHIP_ESP32   ? "ESP32"    :
                        info.model == CHIP_ESP32C3 ? "ESP32-C3" :
                        info.model == CHIP_ESP32C6 ? "ESP32-C6" : "?";
    chip["cores"]     = info.cores;
    chip["revision"]  = info.revision;
    JsonObject cloud  = doc["cloud_replacement"].to<JsonObject>();
    cloud["port"]     = BOSE_HTTP_PORT;
    cloud["base_url"] = myBaseUrl();
    doc["speakers_count"]    = sixback::SpeakerInventory::instance().list().size();
    doc["scan_in_progress"]  = sixback::SpeakerInventory::instance().isScanRunning();

    // Health-Snapshot: boot/crash-counter, last reset reason, watchdog state
    JsonObject health = doc["health"].to<JsonObject>();
    sixback::healthToJson(health);

    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// Speaker-Liste + Discovery
// -----------------------------------------------------------------------------
void emitSpeakers(AsyncWebServerRequest* req, JsonDocument& doc) {
    JsonArray arr = doc["speakers"].to<JsonArray>();
    auto& inv = sixback::SpeakerInventory::instance();
    auto& ps  = sixback::PresetStore::instance();
    for (auto& s : inv.list()) {
        JsonObject o = arr.add<JsonObject>();
        o["device_id"] = s.deviceId;
        o["name"]      = s.name;
        o["model"]     = s.model;
        o["firmware"]  = s.firmware;
        o["ip"]        = s.ip;
        o["account_id"]= s.accountId;
        o["status"]      = sixback::migrationStatusToStr(s.status);
        o["cloud_url"]   = s.cloudUrl;
        o["owned_by_us"] = s.ownedByUs;
        o["group_id"]    = s.groupId;
        // Anzahl belegter Preset-Slots
        int n = 0;
        for (auto& p : ps.getForSpeaker(s.deviceId)) {
            if (p.source != sixback::PresetSource::EMPTY) ++n;
        }
        o["preset_count"] = n;
    }
    doc["scan_in_progress"] = inv.isScanRunning();
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

void handleSpeakersList(AsyncWebServerRequest* req) {
    JsonDocument doc;
    emitSpeakers(req, doc);
}

void discoverWorker_(void* /*arg*/) {
    sixback::SpeakerInventory::instance().discover();
    vTaskDelete(nullptr);
}

void handleDiscover(AsyncWebServerRequest* req) {
    // Discover als Background-Task: knownIpProbe + SSDP-listen + refreshMigrationStatus
    // brauchen zusammen ~5 s, danach laeuft der /24-Active-Scan weitere 30s+.
    // Wir returnen sofort den aktuellen Snapshot — scan_in_progress=true signal-
    // isiert dem UI dass es weiter pollen soll. Wenn discover() bereits laeuft,
    // gibt's keinen weiteren Worker (compare_exchange in discover()) — wir
    // returnen einfach den aktuellen Stand.
    auto& inv = sixback::SpeakerInventory::instance();
    if (!inv.isScanRunning()) {
        BaseType_t r = xTaskCreate(discoverWorker_, "bg-discover", 4096,
                                    nullptr, tskIDLE_PRIORITY + 1, nullptr);
        if (r != pdPASS) {
            req->send(503, "application/json", "{\"error\":\"task spawn failed\"}");
            return;
        }
    }
    JsonDocument doc;
    emitSpeakers(req, doc);
}

void handleSpeakerAdd(AsyncWebServerRequest* req, JsonDocument& body) {
    String ip = (const char*)(body["ip"] | "");
    if (ip.length() == 0) { req->send(400, "application/json", "{\"error\":\"ip required\"}"); return; }
    sixback::ProbeFailure fail;
    bool ok = sixback::SpeakerInventory::instance().addByIp(ip, &fail);
    if (ok) {
        req->send(200, "application/json", "{\"ok\":true}");
        return;
    }
    // Strukturierter Fehler — UI kann reason+detail darstellen statt nur "failed"
    String out = "{\"ok\":false,\"error\":\"probe_failed\",\"reason\":\"";
    out += sixback::probeFailReasonStr(fail.reason);
    out += "\",\"detail\":\"";
    String d = fail.detail; d.replace("\"", "'"); d.replace("\n", " ");
    out += d;
    out += "\",\"hint\":\"";
    switch (fail.reason) {
        case sixback::ProbeFailReason::CONNECT_FAILED:
            out += "Speaker antwortet nicht auf Port 8090. Pruefe ob die IP "
                   "korrekt ist und der Speaker eingeschaltet ist. Wenn ein "
                   "SoundTouch Portable im Standby ist, weckt ihn ein "
                   "kurzer Button-Druck.";
            break;
        case sixback::ProbeFailReason::HTTP_NOT_200:
            out += "Port 8090 antwortet, liefert aber nicht HTTP 200. Ist die IP "
                   "wirklich ein SoundTouch, oder etwas anderes?";
            break;
        case sixback::ProbeFailReason::WRONG_BODY:
        case sixback::ProbeFailReason::EMPTY_BODY:
            out += "Antwort kein Bose-/info-XML. Pruefe: curl http://" + ip +
                   ":8090/info — was kommt zurueck?";
            break;
        case sixback::ProbeFailReason::NO_DEVICE_ID:
            out += "<info>-Tag vorhanden, aber kein deviceID-Attribut. Sehr "
                   "ungewoehnlich — bitte XML-Body vom curl-Test mitschicken.";
            break;
        default:
            out += "Unbekannter Fehler — bitte serielles Log mitschicken.";
    }
    out += "\"}";
    req->send(404, "application/json", out);
}

void handleSpeakerDelete(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    bool ok = sixback::SpeakerInventory::instance().remove(id);
    req->send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"not found\"}");
}

// -----------------------------------------------------------------------------
// Speaker-Aktionen: migrate / revert / reboot / refresh-status
// -----------------------------------------------------------------------------
// Worker-Pattern fuer Long-IO-Handler:
// Telnet auf TCP:17000 zum Speaker dauert 5-10 s. ESPAsyncWebServer laeuft
// auf einem einzigen async_tcp-Task — solange er in einem Handler haengt,
// blockieren ALLE anderen Requests (auch port 8000 Cloud-Mock). Deshalb:
// Handler spawnt FreeRTOS-Task und returnt sofort 202+queued; das UI sieht
// einen schnellen Erfolg und re-fetch'ed die speaker-Liste, was den
// finalen Status zeigt sobald der Worker durch ist.
struct MigrateJob_ {
    String deviceId;
    String ip;
    String baseUrl;
    bool   doMigrate;  // true=migrate, false=revert
};

// Forward-decl: Import-Helper liegt weiter unten (vor handleImportFromDevice).
// handleMigrate ruft ihn vor migrate-trigger auf, damit der Speaker NIE eine
// Migration auf einen leeren Preset-Store erlebt — sonst verliert er beim
// ersten Sync seinen Local-Cache.
int importPresetsFromSpeaker_(const String& id, int& countOk, int& countAban,
                              int& httpCodeOut, JsonArray imported,
                              JsonArray abandoned);

void migrateRevertWorker_(void* arg) {
    auto* job = static_cast<MigrateJob_*>(arg);
    auto& inv = sixback::SpeakerInventory::instance();
    if (job->doMigrate) {
        auto r = migrateSpeaker(job->ip, job->baseUrl);
        Serial.printf("[bg:migrate] %s -> %s ok=%d msg=%s\n",
                      job->deviceId.c_str(), job->baseUrl.c_str(),
                      (int)r.ok, r.message.c_str());
        if (r.ok) {
            sixback::SpeakerInventory::LockGuard g(inv);
            if (auto* sp = inv.findById(job->deviceId)) {
                sp->status    = sixback::MigrationStatus::MIGRATED;
                sp->cloudUrl  = job->baseUrl;
                sp->ownedByUs = true;
                inv.saveToNVS();
            }
        }
    } else {
        auto r = revertSpeaker(job->ip);
        Serial.printf("[bg:revert] %s ok=%d msg=%s\n",
                      job->deviceId.c_str(), (int)r.ok, r.message.c_str());
        if (r.ok) {
            sixback::SpeakerInventory::LockGuard g(inv);
            if (auto* sp = inv.findById(job->deviceId)) {
                sp->status    = sixback::MigrationStatus::NOT_MIGRATED;
                sp->cloudUrl  = "https://streaming.bose.com";
                sp->ownedByUs = false;
                inv.saveToNVS();
            }
        }
    }
    delete job;
    vTaskDelete(nullptr);
}

void handleMigrate(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String ip;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        ip = sp->ip;
    }

    // SCHUTZ gegen Preset-Verlust (2026-05-19): wenn unser Store fuer das
    // Device noch leer ist, MUSS vorher import-from-device passieren — sonst
    // antwortet handleDevicePresets dem Speaker mit <presets/> und der
    // ueberschreibt seinen Local-Cache. handleDevicePresets selbst schickt
    // jetzt zwar 404 bei leerem Store, aber wir wollen den Migrate sowieso
    // erst freischalten, wenn die Presets im Store sind.
    if (!sixback::PresetStore::instance().hasAnyFor(id)) {
        int countOk = 0, countAban = 0, httpCode = 0;
        int status = importPresetsFromSpeaker_(id, countOk, countAban, httpCode,
                                                JsonArray(), JsonArray());
        Serial.printf("[migrate-safe] %s pre-import status=%d ok=%d aban=%d "
                      "speakerhttp=%d store-now-has=%d\n",
                      id.c_str(), status, countOk, countAban, httpCode,
                      (int)sixback::PresetStore::instance().hasAnyFor(id));
        if (status != 200) {
            String err = "{\"error\":\"pre-migrate preset-import failed (status ";
            err += status; err += ")\",\"speaker_http\":"; err += httpCode; err += "}";
            req->send(409, "application/json", err);
            return;
        }
        // Auch ohne Error koennten 0 Presets gefunden worden sein (Speaker hat
        // selber keinen). Das ist OK — Mock liefert dann zwar 404 weiter, aber
        // damit ist nichts verloren. Speaker behaelt was er hat (= nichts).
    }

    sixback::persistPreMigrateSnapshot(id, /*force=*/false);

    // DLNA-Server-UUIDs vom Speaker cachen, damit handleAccountFull spaeter
    // pro UUID eine dedizierte STORED_MUSIC-Source deklarieren kann.
    // Ohne diese sieht der Speaker STORED_MUSIC-Presets als
    // UNKNOWN_SOURCE_ERROR (1005) nach Migration zu SixBack.
    sixback::SpeakerInventory::instance().refreshMediaServers(id);

    auto* job = new MigrateJob_{id, ip, myBaseUrl(), /*doMigrate=*/true};
    BaseType_t r = xTaskCreate(migrateRevertWorker_, "bg-migrate", 4096, job,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (r != pdPASS) {
        delete job;
        req->send(503, "application/json", "{\"error\":\"task spawn failed\"}");
        return;
    }
    req->send(202, "application/json",
              "{\"ok\":true,\"queued\":true,"
              "\"message\":\"migration started in background — refresh speaker card in ~90s to see result (Bose-Reboot + Telnet-Settle)\"}");
}

void handleRevert(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String ip;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        ip = sp->ip;
    }
    auto* job = new MigrateJob_{id, ip, myBaseUrl(), /*doMigrate=*/false};
    BaseType_t r = xTaskCreate(migrateRevertWorker_, "bg-revert", 4096, job,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (r != pdPASS) {
        delete job;
        req->send(503, "application/json", "{\"error\":\"task spawn failed\"}");
        return;
    }
    req->send(202, "application/json",
              "{\"ok\":true,\"queued\":true,"
              "\"message\":\"revert started in background — refresh speaker card in ~90s to see result (Bose-Reboot + Telnet-Settle)\"}");
}

void handleReboot(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String ip;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        ip = sp->ip;
    }
    bool ok = rebootSpeaker(ip);
    req->send(ok ? 200 : 500, "application/json",
              ok ? "{\"ok\":true,\"message\":\"speaker reboot triggered\"}"
                 : "{\"error\":\"telnet failed\"}");
}

void handleRefreshStatus(AsyncWebServerRequest* req) {
    sixback::SpeakerInventory::instance().refreshMigrationStatus();
    JsonDocument doc;
    emitSpeakers(req, doc);
}

// -----------------------------------------------------------------------------
// Preset-Verwaltung
// -----------------------------------------------------------------------------
void handleGetPresets(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    JsonDocument doc;
    JsonArray arr = doc["presets"].to<JsonArray>();
    for (auto& p : sixback::PresetStore::instance().getForSpeaker(id)) {
        JsonObject o = arr.add<JsonObject>();
        o["slot"]      = p.slot;
        o["source"]    = sixback::presetSourceToStr(p.source);
        o["name"]      = p.name;
        o["stationId"] = p.stationId;
        o["streamUrl"] = p.streamUrl;
        o["imageUrl"]  = p.imageUrl;
        if (p.source == sixback::PresetSource::OPAQUE) {
            o["opaqueSourceName"] = p.opaqueSourceName;
            // rawContentItem nicht standardmaessig ausliefern (kann gross sein,
            // und ohnehin nur fuer Diagnose interessant — Diagnostic-Snapshot
            // hat das XML vollstaendig).
            o["rawContentItemBytes"] = (uint16_t)p.rawContentItem.length();
        }
    }
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

void handlePutPreset(AsyncWebServerRequest* req, JsonDocument& body) {
    String id  = req->pathArg(0);
    uint8_t slot = req->pathArg(1).toInt();
    if (slot < 1 || slot > 6) { req->send(400, "application/json", "{\"error\":\"slot 1..6\"}"); return; }
    sixback::Preset p;
    p.slot      = slot;
    // source-Validierung an der Grenze: presetSourceFromStr returnt EMPTY
    // fuer unbekannte Strings (silent default), was sonst zu einem nicht
    // abspielbaren Preset-Slot fuehrt der den User irritiert. Lieber 400.
    String srcStr = String((const char*)(body["source"] | "TUNEIN"));
    p.source    = sixback::presetSourceFromStr(srcStr);
    if (p.source == sixback::PresetSource::EMPTY && srcStr != "EMPTY") {
        req->send(400, "application/json",
                  String("{\"error\":\"unknown source\",\"got\":\"") + srcStr +
                  "\",\"expected\":\"TUNEIN | LOCAL_INTERNET_RADIO\"}");
        return;
    }
    p.name      = (const char*)(body["name"]      | "");
    p.stationId = (const char*)(body["stationId"] | "");
    p.streamUrl = (const char*)(body["streamUrl"] | "");
    p.imageUrl  = (const char*)(body["imageUrl"]  | "");
    bool ok = sixback::PresetStore::instance().set(id, p);
    req->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"set failed\"}");
}

void handleDeletePreset(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    uint8_t slot = req->pathArg(1).toInt();
    bool ok = sixback::PresetStore::instance().clear(id, slot);
    req->send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"unknown\"}");
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/presets/import-from-device
//   Liest /presets-XML vom echten Speaker (BMX-API Port 8090) und uebernimmt
//   die bereits dort gespeicherten Presets in den ESP-PresetStore. So muss
//   der User nicht haendisch jede TuneIn-ID raussuchen.
//   Funktioniert AUCH wenn der Speaker noch nicht migriert ist (BMX-API ist
//   immer offen, unabhaengig von der Cloud-URL-Config).
// -----------------------------------------------------------------------------
String xmlExtractAttr(const String& xml, int start, int end, const String& attr) {
    String key = attr + "=\"";
    int a = xml.indexOf(key, start);
    if (a < 0 || a >= end) return "";
    a += key.length();
    int b = xml.indexOf('"', a);
    if (b < 0 || b >= end) return "";
    return xml.substring(a, b);
}

String xmlExtractTag(const String& xml, int start, int end, const String& tag) {
    String open = "<" + tag + ">";
    String close = "</" + tag + ">";
    int a = xml.indexOf(open, start);
    if (a < 0 || a >= end) return "";
    a += open.length();
    int b = xml.indexOf(close, a);
    if (b < 0 || b > end) return "";
    return xml.substring(a, b);
}

// Pure-Helper: holt /presets-XML vom Speaker, normalisiert + persistiert.
// JsonDocument&-Outputs optional (nullptr fuer "kein Detail-Report noetig",
// nur Status reicht). Return-Wert:
//   200 = OK (countOk + countAban gesetzt)
//   404 = unknown deviceId
//   500 = http begin failed
//   502 = speaker http != 200 (httpCodeOut gefuellt)
int importPresetsFromSpeaker_(const String& id, int& countOk, int& countAban,
                              int& httpCodeOut, JsonArray imported,
                              JsonArray abandoned) {
    countOk = countAban = 0;
    httpCodeOut = 0;
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) return 404;
        spIp = sp->ip;
    }
    HTTPClient http;
    http.setReuse(false);
    String url = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/presets";
    http.setConnectTimeout(2000); http.setTimeout(3000);
    if (!http.begin(url)) return 500;
    int code = http.GET();
    httpCodeOut = code;
    if (code != 200) { http.end(); return 502; }
    String xml = http.getString();
    http.end();

    std::vector<sixback::Preset> toSet;
    toSet.reserve(6);
    int pos = 0;
    while (true) {
        int presetOpen = xml.indexOf("<preset id=\"", pos);
        if (presetOpen < 0) break;
        int idStart = presetOpen + 12;
        int idEnd = xml.indexOf('"', idStart);
        if (idEnd < 0) break;
        int presetClose = xml.indexOf("</preset>", idEnd);
        if (presetClose < 0) break;
        uint8_t slot = xml.substring(idStart, idEnd).toInt();
        if (slot < 1 || slot > 6) { pos = presetClose; continue; }

        String src  = xmlExtractAttr(xml, idEnd, presetClose, "source");
        String loc  = xmlExtractAttr(xml, idEnd, presetClose, "location");
        String name = xmlExtractTag (xml, idEnd, presetClose, "itemName");
        String img  = xmlExtractTag (xml, idEnd, presetClose, "containerArt");

        sixback::Preset p;
        p.slot = slot;
        auto nr = sixback::normalizePreset(src, loc, name, img, p);

        if (nr.status == sixback::NormalizeStatus::OK_OPAQUE) {
            // Vollstaendiges <ContentItem>...</ContentItem> extrahieren —
            // wird beim Sync 1:1 ans Speaker zurueckgeschickt. Speaker spricht
            // DLNA/UPnP/Bluetooth selbst an, unsere Cloud ist nicht
            // beteiligt am Playback.
            int ciOpen  = xml.indexOf("<ContentItem", idEnd);
            int ciClose = xml.indexOf("</ContentItem>", ciOpen);
            if (ciOpen >= 0 && ciOpen < presetClose && ciClose > ciOpen) {
                p.rawContentItem = xml.substring(ciOpen, ciClose + 14);
            }
            if (p.name.length() == 0) {
                p.name = String("[") + src + String("] preset");
            }
        }

        if (nr.status == sixback::NormalizeStatus::ABANDONED) {
            if (!abandoned.isNull()) {
                JsonObject o = abandoned.add<JsonObject>();
                o["slot"]   = slot;
                o["source"] = src;
                o["reason"] = nr.reason;
            }
            ++countAban;
        } else {
            toSet.push_back(p);
            if (!imported.isNull()) {
                JsonObject o = imported.add<JsonObject>();
                o["slot"]            = p.slot;
                o["name"]            = p.name;
                o["source"]          = sixback::presetSourceToStr(p.source);
                o["stationId"]       = p.stationId;
                o["streamUrl"]       = p.streamUrl;
                o["normalize"]       = sixback::normalizeStatusToStr(nr.status);
                if (nr.status == sixback::NormalizeStatus::OK_CONVERTED) {
                    o["converted_from"] = nr.originalSource;
                    o["reason"]         = nr.reason;
                }
                if (nr.status == sixback::NormalizeStatus::OK_OPAQUE) {
                    o["opaque_source"]  = p.opaqueSourceName;
                    o["raw_bytes"]      = (uint16_t)p.rawContentItem.length();
                }
            }
            ++countOk;
        }
        pos = presetClose;
    }
    // Batched-Save: alle erfolgreich normalisierten Presets in einem
    // NVS-Write statt einer pro Slot. ABER: wenn der Speaker grad leer ist
    // (z.B. eben durch eine voraus-fehlgeschlagene Migration), setSlots-mit-
    // leerem-vector wuerde unseren Store auch leer machen. Deshalb nur dann
    // schreiben wenn was gefunden wurde — sonst Store unveraendert lassen.
    if (!toSet.empty()) {
        sixback::PresetStore::instance().setSlots(id, toSet);
    }
    return 200;
}

void handleImportFromDevice(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    JsonDocument resp;
    JsonArray imported  = resp["imported"].to<JsonArray>();
    JsonArray abandoned = resp["abandoned"].to<JsonArray>();
    int countOk = 0, countAban = 0, httpCode = 0;
    int status = importPresetsFromSpeaker_(id, countOk, countAban, httpCode,
                                            imported, abandoned);
    if (status == 404) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
    if (status == 500) { req->send(500, "application/json", "{\"error\":\"http begin\"}");      return; }
    if (status == 502) {
        req->send(502, "application/json",
                  String("{\"error\":\"speaker http ") + httpCode + "\"}");
        return;
    }
    resp["ok"]              = true;
    resp["count"]           = countOk;
    resp["abandoned_count"] = countAban;
    String body; serializeJson(resp, body);
    req->send(200, "application/json", body);
}

// POST /api/speaker/{id}/preset/{slot}/push-to-device
//   speichert das aktuelle Preset am echten Speaker (Long-Press-Simulation):
//   /select mit ContentItem → /key press PRESET_n → 2.5s → /key release PRESET_n
//
// 4 s + 2.5 s = 6.5 s sind hartes Bose-Timing, nicht verkuerzbar — daher
// als Background-Task wie Migrate/Revert, damit der async_tcp-Thread frei
// bleibt.
struct PushPresetJob_ {
    String spIp;
    sixback::Preset p;
};

// Persistenter Single-Worker mit FreeRTOS-Queue. Vorgaenger-Designs hatten
// pro Push einen eigenen Task spawned + Mutex serialisiert. Bei Burst >6
// Pushes ging das gelegentlich kaputt — entweder xTaskCreate fail (Heap-
// Druck mit N×4KB-Stacks parallel) oder Race im Mutex-Lazy-Init.
//
// Jetzt: eine Worker-Task wird lazy beim ersten Push-Handle erzeugt und
// laeuft fuer immer. Handler enqueued nur `PushPresetJob_*` in die Queue,
// Worker holt FIFO ab. Queue-Full -> 503 (statt verlorenen Tasks).
static QueueHandle_t g_pushQueue = nullptr;
static TaskHandle_t  g_pushTask  = nullptr;
constexpr UBaseType_t PUSH_QUEUE_DEPTH = 16;  // ≥ 3 Speaker × 6 Slots

static void doPush_(const PushPresetJob_& job) {
    HTTPClient http;
    http.setReuse(false);
    String url = "http://" + job.spIp + ":" + String(BOSE_BMX_PORT) + "/select";
    int selectCode = -1;
    if (http.begin(url)) {
        // sourceAccount muss zum Speaker-/sources-Eintrag passen, sonst HTTP 500:
        //   TUNEIN -> "TuneIn" (Greta+Emma+Kueche haben das so unter sourceItem
        //             source="TUNEIN" sourceAccount="TuneIn" gespeichert).
        //   LOCAL_INTERNET_RADIO -> "" (das ist der ESP-eigene Stream-Proxy,
        //             Speaker akzeptiert leeren Account).
        const char* srcAcct =
            (job.p.source == sixback::PresetSource::TUNEIN) ? "TuneIn" : "";
        String ci = "<ContentItem source=\"";
        ci += sixback::presetSourceToStr(job.p.source);
        ci += "\" type=\"stationurl\" location=\"";
        if (job.p.source == sixback::PresetSource::TUNEIN) {
            ci += "/v1/playback/station/" + job.p.stationId;
        } else {
            ci += job.p.streamUrl;
        }
        ci += "\" sourceAccount=\""; ci += srcAcct;
        ci += "\" isPresetable=\"true\"><itemName>" + job.p.name + "</itemName></ContentItem>";
        http.addHeader("Content-Type", "text/xml");
        selectCode = http.POST(ci);
        http.end();
    }
    delay(4000);  // Stream stabilisieren
    // Long-Press PRESET_n
    auto sendKey = [&](const String& state) {
        HTTPClient h;
        h.setReuse(false);
        String u = "http://" + job.spIp + ":" + String(BOSE_BMX_PORT) + "/key";
        if (!h.begin(u)) return;
        h.addHeader("Content-Type", "text/xml");
        String body = "<key state=\"" + state + "\" sender=\"Gabbo\">PRESET_" + String(job.p.slot) + "</key>";
        h.POST(body);
        h.end();
    };
    sendKey("press");
    delay(2500);  // > 2s = Long-Press
    sendKey("release");
    Serial.printf("[bg:push-preset] %s slot %u select=%d done\n",
                  job.spIp.c_str(), job.p.slot, selectCode);
}

void pushPresetWorker_(void* /*arg*/) {
    PushPresetJob_* job = nullptr;
    while (true) {
        if (xQueueReceive(g_pushQueue, &job, portMAX_DELAY) == pdTRUE && job) {
            doPush_(*job);
            delete job;
            job = nullptr;
        }
    }
}

void handlePushPresetToDevice(AsyncWebServerRequest* req) {
    // Lazy-init: erste Push-Anfrage triggert Worker + Queue. AsyncWebServer
    // ist single-threaded, kein Race um die Init-Flags.
    if (!g_pushQueue) {
        g_pushQueue = xQueueCreate(PUSH_QUEUE_DEPTH, sizeof(PushPresetJob_*));
        if (!g_pushQueue) {
            req->send(503, "application/json", "{\"error\":\"push queue alloc failed\"}");
            return;
        }
        BaseType_t tr = xTaskCreate(pushPresetWorker_, "push-worker", 4096,
                                     nullptr, tskIDLE_PRIORITY + 1, &g_pushTask);
        if (tr != pdPASS) {
            vQueueDelete(g_pushQueue);
            g_pushQueue = nullptr;
            req->send(503, "application/json", "{\"error\":\"push worker spawn failed\"}");
            return;
        }
    }

    String id = req->pathArg(0);
    uint8_t slot = req->pathArg(1).toInt();
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    auto p = sixback::PresetStore::instance().get(id, slot);
    if (p.source == sixback::PresetSource::EMPTY) {
        req->send(400, "application/json", "{\"error\":\"empty slot\"}"); return;
    }
    auto* job = new PushPresetJob_{spIp, p};
    if (xQueueSend(g_pushQueue, &job, 0) != pdTRUE) {
        delete job;
        req->send(503, "application/json",
                  "{\"error\":\"push queue full (>=16 pending) — retry in a few seconds\"}");
        return;
    }
    UBaseType_t depth = uxQueueMessagesWaiting(g_pushQueue);
    String body = "{\"ok\":true,\"queued\":true,\"queue_depth\":";
    body += String((unsigned)depth);
    body += ",\"message\":\"preset push enqueued — sequential push-worker, ~7s per slot\"}";
    req->send(202, "application/json", body);
}

// -----------------------------------------------------------------------------
// Gruppen
// -----------------------------------------------------------------------------
void handlePutGroup(AsyncWebServerRequest* req, JsonDocument& body) {
    String id = req->pathArg(0);
    String groupId = (const char*)(body["group_id"] | "");
    auto& inv = sixback::SpeakerInventory::instance();
    sixback::SpeakerInventory::LockGuard g(inv);
    auto* sp = inv.findById(id);
    if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown\"}"); return; }
    sp->groupId = groupId;
    inv.saveToNVS();
    req->send(200, "application/json", "{\"ok\":true}");
}

void handleSyncGroup(AsyncWebServerRequest* req, JsonDocument& body) {
    String src = (const char*)(body["source_device_id"] | "");
    std::vector<String> targets;
    if (body["target_device_ids"].is<JsonArray>()) {
        for (JsonVariant v : body["target_device_ids"].as<JsonArray>()) {
            targets.push_back(String((const char*)v));
        }
    } else if (body["group_id"].is<const char*>()) {
        String gid = (const char*)body["group_id"];
        for (auto& s : sixback::SpeakerInventory::instance().list()) {
            if (s.groupId == gid && s.deviceId != src) targets.push_back(s.deviceId);
        }
    }
    int n = sixback::PresetStore::instance().syncToGroup(src, targets);
    JsonDocument doc; doc["ok"] = true; doc["synced_to"] = n;
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

// -----------------------------------------------------------------------------
// TuneIn — Resolve fuer UI-Preview
// -----------------------------------------------------------------------------
void handleTuneInResolve(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto r = sixback::resolveTuneInStruct(id);
    JsonDocument doc;
    doc["ok"]        = r.ok;
    doc["stationId"] = r.stationId;
    doc["name"]      = r.name;
    doc["streamUrl"] = r.streamUrl;
    doc["imageUrl"]  = r.imageUrl;
    doc["source"]    = r.source;
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

// GET /api/tunein/search?q=...
//   Proxy zu http://opml.radiotime.com/Search.ashx?query=<q>&render=json
void handleTuneInSearch(AsyncWebServerRequest* req) {
    String q;
    if (req->hasParam("q")) q = req->getParam("q")->value();
    if (q.length() == 0) { req->send(400, "application/json", "{\"error\":\"q required\"}"); return; }
    if (WiFi.status() != WL_CONNECTED) { req->send(503, "application/json", "{\"error\":\"offline\"}"); return; }
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000); http.setTimeout(5000);
    String url = "http://opml.radiotime.com/Search.ashx?query=" + urlEncodeQuery_(q) + "&render=json";
    if (!http.begin(url)) { req->send(500, "application/json", "{\"error\":\"begin\"}"); return; }
    int code = http.GET();
    if (code != 200) { http.end(); req->send(502, "application/json", "{\"error\":\"upstream\"}"); return; }
    String body = http.getString();
    http.end();
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// System
// -----------------------------------------------------------------------------
void handleFactoryResetWifi(AsyncWebServerRequest* req) {
    sixback::factoryResetWifi();
    req->send(200, "application/json", "{\"ok\":true,\"reboot_in_ms\":500}");
    delay(500); ESP.restart();
}

void handleSelfReboot(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"ok\":true,\"reboot_in_ms\":500}");
    delay(500); ESP.restart();
}

// -----------------------------------------------------------------------------
// OTA — App-Image (POST /api/ota) und LittleFS-Image (POST /api/ota/fs)
//   Beide multipart/octet-stream firmware upload, Reboot nach Finalize.
//   Unterschied: Update.begin(..., U_FLASH) vs U_SPIFFS — bei arduino-esp32
//   ist U_SPIFFS auch der Pfad fuer LittleFS (gleiche Partition).
// -----------------------------------------------------------------------------
void handleOtaFinalize(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["ok"]    = !Update.hasError();
    doc["error"] = Update.errorString();
    doc["kind"]  = "app";
    String body; serializeJson(doc, body);
    req->send(Update.hasError() ? 500 : 200, "application/json", body);
    if (!Update.hasError()) { delay(500); ESP.restart(); }
}

void handleOtaUpload(AsyncWebServerRequest* req, String filename,
                     size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        Serial.printf("[ota] APP start %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
        }
    }
    if (Update.write(data, len) != len) {
        Update.printError(Serial);
    }
    if (final) {
        if (Update.end(true)) Serial.printf("[ota] APP done (%u bytes)\n",
                                            (unsigned)(index+len));
        else                  Update.printError(Serial);
    }
}

void handleOtaFsFinalize(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["ok"]    = !Update.hasError();
    doc["error"] = Update.errorString();
    doc["kind"]  = "fs";
    String body; serializeJson(doc, body);
    req->send(Update.hasError() ? 500 : 200, "application/json", body);
    if (!Update.hasError()) { delay(500); ESP.restart(); }
}

void handleOtaFsUpload(AsyncWebServerRequest* req, String filename,
                       size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        Serial.printf("[ota] FS start %s\n", filename.c_str());
        // LittleFS hat keine eigene Update-Konstante - U_SPIFFS funktioniert,
        // weil arduino-esp32 die Daten-Partition unabhaengig vom FS-Type
        // schreibt. WICHTIG: vor dem Schreiben unmounten, sonst frisch
        // geschriebene Daten werden vom alten Mount nicht gesehen.
        LittleFS.end();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
            Update.printError(Serial);
        }
    }
    if (Update.write(data, len) != len) {
        Update.printError(Serial);
    }
    if (final) {
        if (Update.end(true)) Serial.printf("[ota] FS done (%u bytes)\n",
                                            (unsigned)(index+len));
        else                  Update.printError(Serial);
    }
}

// -----------------------------------------------------------------------------
// Online-Update (HTTPS-Pull von install.busware.de) — Inspired by
// tul-knx-gateway. Drei Endpoints:
//   GET  /api/update/check    — Manifest holen + Version vergleichen.
//   POST /api/update/install  — Background-Task startet Firmware-Pull + Flash.
//   GET  /api/update/status   — JSON-Snapshot fuer UI-Polling.
// -----------------------------------------------------------------------------
namespace {
const char* otaStateName_(sixback::ota::State s) {
    using S = sixback::ota::State;
    switch (s) {
        case S::IDLE:       return "idle";
        case S::CHECKING:   return "checking";
        case S::AVAILABLE:  return "available";
        case S::INSTALLING: return "installing";
        case S::DONE:       return "done";
        case S::ERROR_:     return "error";
    }
    return "?";
}

void writeOtaStatus_(AsyncWebServerRequest* req) {
    auto st = sixback::ota::getStatus();
    JsonDocument doc;
    doc["state"]    = otaStateName_(st.state);
    doc["current"]  = st.current;
    doc["latest"]   = st.latest;
    doc["progress"] = st.progress;
    doc["total"]    = st.total;
    doc["phase"]    = st.phase;
    doc["phase_idx"]= st.phaseIdx;
    doc["phase_n"]  = st.phaseN;
    doc["error"]    = st.error;
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}
} // anon

void handleOtaUpdateCheck(AsyncWebServerRequest* req) {
    sixback::ota::checkOnline();
    writeOtaStatus_(req);
}

void handleOtaUpdateInstall(AsyncWebServerRequest* req) {
    // Optional: ?force=1 erlaubt Re-Install des gleichen oder eines aelteren
    // Versions-Standes (z.B. "ich will von install.busware.de denselben Build
    // nochmal pullen", oder Demo des Progress-Bars wenn current >= latest).
    bool force = req->hasParam("force") && req->getParam("force")->value() == "1";
    bool ok = force ? sixback::ota::installOnlineForceAsync()
                    : sixback::ota::installOnlineAsync();
    if (!ok) {
        auto st = sixback::ota::getStatus();
        if (!force && st.state != sixback::ota::State::AVAILABLE) {
            req->send(409, "application/json",
                      String("{\"error\":\"no update available — run /api/update/check first (use ?force=1 to re-install)\",\"state\":\"")
                       + otaStateName_(st.state) + "\"}");
            return;
        }
        req->send(500, "application/json",
                  "{\"error\":\"failed to spawn install task\"}");
        return;
    }
    writeOtaStatus_(req);
}

void handleOtaUpdateStatus(AsyncWebServerRequest* req) {
    writeOtaStatus_(req);
}

void handleRoot(AsyncWebServerRequest* req) {
    // Wenn LittleFS Web-UI hat -> liefere index.html, sonst Mini-Page.
    if (LittleFS.exists("/index.html")) {
        req->send(LittleFS, "/index.html", "text/html");
        return;
    }
    String ip = WiFi.localIP().toString();
    String html =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<title>SixBack " FW_VERSION_STRING "</title>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:-apple-system,Segoe UI,sans-serif;max-width:40em;margin:3em auto;padding:0 1em}"
        "code{background:#fee;padding:.1em .3em;border-radius:3px}</style></head><body>"
        "<h1>SixBack</h1><p>Web UI not flashed. Use the JSON API directly:</p>"
        "<ul>"
          "<li><code>GET /api/status</code></li>"
          "<li><code>GET /api/speakers</code></li>"
          "<li><code>POST /api/speakers/discover</code></li>"
          "<li><code>POST /api/speaker/&lt;id&gt;/migrate</code></li>"
          "<li><code>POST /api/speaker/&lt;id&gt;/revert</code></li>"
          "<li><code>GET/PUT /api/speaker/&lt;id&gt;/preset/&lt;1..6&gt;</code></li>"
          "<li><code>POST /api/group/sync</code></li>"
          "<li><code>GET /api/tunein/resolve/&lt;s24896&gt;</code></li>"
          "<li><code>POST /api/ota</code> (multipart firmware.bin)</li>"
          "<li><code>POST /api/ota/fs</code> (multipart littlefs.bin)</li>"
        "</ul>"
        "<p>Version <code>" FW_VERSION_STRING "</code> &middot; Build " FW_BUILD_DATE
        " &middot; <a href=\"https://github.com/tostmann/SixBack\">github.com/tostmann/SixBack</a></p>"
        "</body></html>";
    req->send(200, "text/html; charset=utf-8", html);
}

} // anon

// -----------------------------------------------------------------------------
// IP-Failsafe Test-Endpoint
//   Setzt NVS `sixback-net.last_ip` auf einen anderen Wert, sodass beim
//   naechsten Boot `ipFailsafeCheck()` einen IP-Wechsel erkennt und alle
//   owned-Speaker per Telnet re-migriert. Reine Test-Hilfe, kein Cleanup
//   noetig — der Failsafe persistiert beim Lauf die aktuelle IP wieder.
// -----------------------------------------------------------------------------
void handleTestForceIpChange(AsyncWebServerRequest* req, JsonDocument& body) {
    String fakeIp = (const char*)(body["fake_ip"] | "10.10.99.99");
    Preferences p;
    if (!p.begin("sixback-net", false)) {
        req->send(500, "application/json", "{\"error\":\"nvs open failed\"}"); return;
    }
    String oldStored = p.getString("last_ip", "");
    p.putString("last_ip", fakeIp);
    p.end();
    JsonDocument doc;
    doc["ok"]                = true;
    doc["nvs_last_ip_was"]   = oldStored;
    doc["nvs_last_ip_now"]   = fakeIp;
    doc["esp_actual_ip"]     = WiFi.localIP().toString();
    doc["next"]              = "POST /api/reboot to trigger ip_failsafe";
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

// -----------------------------------------------------------------------------
// Auto-Mode (Zero-Touch Migration beim Boot)
// -----------------------------------------------------------------------------
void handleGetAutoMode(AsyncWebServerRequest* req) {
    auto cfg = sixback::loadAutoModeConfig();
    auto st  = sixback::getAutoModeStatus();
    JsonDocument doc;
    doc["config"]["enabled"]         = cfg.enabled;
    doc["config"]["dry_run"]         = cfg.dryRun;
    doc["config"]["boot_delay_ms"]   = cfg.bootDelayMs;
    doc["config"]["max_per_boot"]    = cfg.maxPerBoot;
    doc["config"]["cron_interval_s"] = cfg.cronIntervalS;
    doc["status"]["ran"]                  = st.ran;
    doc["status"]["running"]              = st.running;
    doc["status"]["state"]                = st.state;
    doc["status"]["current_device"]       = st.currentDeviceId;
    doc["status"]["speakers_seen"]        = st.speakersSeen;
    doc["status"]["speakers_eligible"]    = st.speakersEligible;
    doc["status"]["speakers_migrated"]    = st.speakersMigrated;
    doc["status"]["slots_normalized"]     = st.slotsNormalized;
    doc["status"]["slots_converted"]      = st.slotsConverted;
    doc["status"]["slots_abandoned"]      = st.slotsAbandoned;
    doc["status"]["last_error"]           = st.lastError;
    doc["status"]["started_ms"]           = st.startedMs;
    doc["status"]["finished_ms"]          = st.finishedMs;
    doc["status"]["tick_count"]           = st.tickCount;
    doc["status"]["last_tick_finished_ms"]= st.lastTickFinishedMs;
    doc["status"]["next_tick_in_s"]       = st.nextTickInS;
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

void handlePutAutoMode(AsyncWebServerRequest* req, JsonDocument& body) {
    auto cfg = sixback::loadAutoModeConfig();
    if (body["enabled"].is<bool>())            cfg.enabled       = body["enabled"].as<bool>();
    if (body["dry_run"].is<bool>())            cfg.dryRun        = body["dry_run"].as<bool>();
    if (body["boot_delay_ms"].is<uint32_t>())  cfg.bootDelayMs   = body["boot_delay_ms"].as<uint32_t>();
    if (body["max_per_boot"].is<uint32_t>())   cfg.maxPerBoot    = body["max_per_boot"].as<uint32_t>();
    if (body["cron_interval_s"].is<uint32_t>())cfg.cronIntervalS = body["cron_interval_s"].as<uint32_t>();
    sixback::saveAutoModeConfig(cfg);
    JsonDocument resp;
    resp["ok"]                         = true;
    resp["config"]["enabled"]          = cfg.enabled;
    resp["config"]["dry_run"]          = cfg.dryRun;
    resp["config"]["boot_delay_ms"]    = cfg.bootDelayMs;
    resp["config"]["max_per_boot"]     = cfg.maxPerBoot;
    resp["config"]["cron_interval_s"]  = cfg.cronIntervalS;
    String b; serializeJson(resp, b);
    req->send(200, "application/json", b);
}

// -----------------------------------------------------------------------------
// P2 — Event-Capture / Now-Playing-UI.
//   GET  /api/events                       — all devices, latest now-playing per
//   GET  /api/speaker/{deviceId}/now-playing  — single device
//   GET  /api/speaker/{deviceId}/events       — last 20 event-types per device
//   DELETE /api/events                     — clear all
// -----------------------------------------------------------------------------
void handleEventsAll(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    sixback::eventStoreAllDevicesJson(arr);
    doc["count"] = arr.size();
    sixback::EventStoreStats st = sixback::eventStoreStats();
    JsonObject stats = doc["stats"].to<JsonObject>();
    stats["chunks_seen"]      = st.chunks_seen;
    stats["bodies_completed"] = st.bodies_completed;
    stats["parse_errors"]     = st.parse_errors;
    stats["events_ingested"]  = st.events_ingested;
    stats["last_chunk_ms"]    = st.last_chunk_ms;
    stats["last_body_ms"]     = st.last_body_ms;
    stats["last_chunk_age_ms"] = st.last_chunk_ms ? (long)(millis() - st.last_chunk_ms) : -1L;
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handleNowPlaying(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    JsonDocument doc;
    JsonObject now = doc["now"].to<JsonObject>();
    sixback::eventStoreNowPlayingJson(id, now);
    doc["device_id"] = id;
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handleSpeakerEvents(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    JsonDocument doc;
    JsonArray arr = doc["events"].to<JsonArray>();
    sixback::eventStoreEventsJson(id, arr);
    doc["device_id"] = id;
    doc["count"]     = arr.size();
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handleEventsClear(AsyncWebServerRequest* req) {
    sixback::eventStoreClearAll();
    req->send(200, "application/json", "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// GET /api/unknown-requests — Catch-All-Logger Ringbuffer-Inhalt (P3).
// DELETE /api/unknown-requests — leert den Ringbuffer.
// -----------------------------------------------------------------------------
void handleUnknownRequestsGet(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["requests"].to<JsonArray>();
    getUnknownRequestsJson(arr);
    doc["count"]    = arr.size();
    doc["capacity"] = 50;
    doc["uptime_ms"] = millis();
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handleUnknownRequestsClear(AsyncWebServerRequest* req) {
    clearUnknownRequests();
    req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/speaker/{id}/diagnostic-snapshot
//   ?source=stored  → liefert den persistierten Pre-Migrate-Snapshot (404 wenn keiner da)
//   ?source=live    → erzeugt einen frischen Live-Snapshot vom Speaker (default)
//   ?save=1         → bei live: zusaetzlich als manueller Snapshot speichern (force)
void handleDiagnosticSnapshot(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    String src = req->hasParam("source") ? req->getParam("source")->value() : String("live");

    if (src == "stored") {
        String stored;
        if (!sixback::loadStoredSnapshot(id, stored)) {
            req->send(404, "application/json",
                      "{\"error\":\"no stored snapshot for this device\"}");
            return;
        }
        AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", stored);
        resp->addHeader("Content-Disposition",
                        "attachment; filename=\"sixback-snapshot-" + id + "-stored.json\"");
        req->send(resp);
        return;
    }

    JsonDocument doc;
    if (!sixback::captureLiveSnapshot(id, doc)) {
        req->send(502, "application/json",
                  "{\"error\":\"speaker unreachable or unknown deviceId\"}");
        return;
    }
    doc["snapshot_kind"] = "live";

    if (req->hasParam("save") && req->getParam("save")->value() == "1") {
        sixback::persistPreMigrateSnapshot(id, /*force=*/true);
    }

    String body;
    serializeJson(doc, body);
    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", body);
    resp->addHeader("Content-Disposition",
                    "attachment; filename=\"sixback-snapshot-" + id + "-live.json\"");
    req->send(resp);
}

void handleDiagnosticSnapshotMeta(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    JsonDocument doc;
    doc["device_id"] = id;
    doc["has_stored"] = sixback::hasStoredSnapshot(id);
    String body;
    serializeJson(doc, body);
    req->send(200, "application/json", body);
}

void registerApiEndpoints(AsyncWebServer& ui) {
    // Statische Assets aus LittleFS (CSS, JS, etc.)
    ui.serveStatic("/assets/", LittleFS, "/assets/").setCacheControl("max-age=600");

    ui.on("/",                        HTTP_GET,    handleRoot);
    ui.on("/api/status",              HTTP_GET,    handleStatus);
    ui.on("/api/speakers",            HTTP_GET,    handleSpeakersList);
    ui.on("/api/speakers/discover",   HTTP_POST,   handleDiscover);
    routeJsonBody(ui, "/api/speakers/add", HTTP_POST, handleSpeakerAdd);
    ui.on("^/api/speakers/([^/]+)$",  HTTP_DELETE, handleSpeakerDelete);
    ui.on("^/api/speaker/([^/]+)/migrate$",        HTTP_POST, handleMigrate);
    ui.on("^/api/speaker/([^/]+)/revert$",         HTTP_POST, handleRevert);
    ui.on("^/api/speaker/([^/]+)/reboot$",         HTTP_POST, handleReboot);
    ui.on("/api/speakers/refresh-status",          HTTP_POST, handleRefreshStatus);

    ui.on("^/api/speaker/([^/]+)/presets$",        HTTP_GET,    handleGetPresets);
    routeJsonBody(ui, "^/api/speaker/([^/]+)/preset/([1-6])$", HTTP_PUT, handlePutPreset);
    ui.on("^/api/speaker/([^/]+)/preset/([1-6])$", HTTP_DELETE, handleDeletePreset);
    ui.on("^/api/speaker/([^/]+)/preset/([1-6])/push-to-device$",
          HTTP_POST, handlePushPresetToDevice);
    ui.on("^/api/speaker/([^/]+)/presets/import-from-device$",
          HTTP_POST, handleImportFromDevice);

    ui.on("^/api/speaker/([^/]+)/diagnostic-snapshot$",
          HTTP_GET, handleDiagnosticSnapshot);
    ui.on("^/api/speaker/([^/]+)/diagnostic-snapshot/meta$",
          HTTP_GET, handleDiagnosticSnapshotMeta);

    routeJsonBody(ui, "^/api/speaker/([^/]+)/group$", HTTP_PUT, handlePutGroup);
    routeJsonBody(ui, "/api/group/sync",              HTTP_POST, handleSyncGroup);

    ui.on("^/api/tunein/resolve/([^/]+)$", HTTP_GET, handleTuneInResolve);
    ui.on("/api/tunein/search",            HTTP_GET, handleTuneInSearch);

    ui.on("/api/auto-mode",          HTTP_GET,  handleGetAutoMode);
    routeJsonBody(ui, "/api/auto-mode", HTTP_PUT, handlePutAutoMode);
    routeJsonBody(ui, "/api/test/force-ip-change", HTTP_POST, handleTestForceIpChange);

    ui.on("/api/factory_reset_wifi", HTTP_POST, handleFactoryResetWifi);
    ui.on("/api/reboot",             HTTP_POST, handleSelfReboot);

    ui.on("/api/unknown-requests",   HTTP_GET,    handleUnknownRequestsGet);
    ui.on("/api/unknown-requests",   HTTP_DELETE, handleUnknownRequestsClear);

    ui.on("/api/events",                                   HTTP_GET,    handleEventsAll);
    ui.on("/api/events",                                   HTTP_DELETE, handleEventsClear);
    ui.on("^/api/speaker/([^/]+)/now-playing$",            HTTP_GET,    handleNowPlaying);
    ui.on("^/api/speaker/([^/]+)/events$",                 HTTP_GET,    handleSpeakerEvents);

    // OTA — multipart upload. Mit ASYNCWEBSERVER_REGEX=1 binden plain
    // Path-Strings ohne `^...$` als Prefix — /api/ota wuerde dann auch
    // /api/ota/fs schlucken. Mit explizitem Anchor matched jede Route
    // exakt ihren Pfad.
    ui.on("^/api/ota$",    HTTP_POST, handleOtaFinalize,   handleOtaUpload);
    ui.on("^/api/ota/fs$", HTTP_POST, handleOtaFsFinalize, handleOtaFsUpload);

    // Online-Update — HTTPS-Pull von install.busware.de
    sixback::ota::init(String(FW_VERSION_STRING));
    ui.on("/api/update/check",   HTTP_GET,  handleOtaUpdateCheck);
    ui.on("/api/update/install", HTTP_POST, handleOtaUpdateInstall);
    ui.on("/api/update/status",  HTTP_GET,  handleOtaUpdateStatus);
}
