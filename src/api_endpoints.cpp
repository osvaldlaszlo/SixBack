// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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
#include "spotify_player.h"
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
    doc["license"]    = "PolyForm-Noncommercial-1.0.0";
    doc["copyright"]  = "Copyright (c) 2026 Dirk Tostmann";
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
        // Spotify-Accounts (READY) — Stufe 0 Diagnose, leerer Array wenn kein Link.
        if (!s.spotifyAccounts.empty()) {
            JsonArray spa = o["spotify_accounts"].to<JsonArray>();
            for (const auto& sa : s.spotifyAccounts) {
                JsonObject sp = spa.add<JsonObject>();
                sp["source_account"] = sa.sourceAccount;
                sp["display_name"]   = sa.displayName;
            }
        }
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
    sixback::SpeakerInventory::instance().refreshSpotifyAccounts(id);

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
    auto& inv = sixback::SpeakerInventory::instance();
    inv.refreshMigrationStatus();
    // Stufe-0 Diagnose: bei jedem manuellen Status-Refresh auch die
    // Spotify-Account-Verknuepfung pro Speaker neu pullen (leichter
    // BMX /sources-GET, ~3s pro Speaker max). So bekommt der User
    // im UI per "🔄 Status"-Click eine aktuelle Spotify-Anzeige.
    for (const auto& s : inv.list()) {
        inv.refreshSpotifyAccounts(s.deviceId);
    }
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
            o["rawContentItemBytes"] = (uint16_t)p.rawContentItem.length();
            // containerArt-URL aus rawContentItem extrahieren, falls vorhanden.
            // DLNA-Slots (z.B. forum-user OG_Bad_ST10weiss slot 1) haben oft
            // `<containerArt>http://192.168.x.y/...</containerArt>` mit dem
            // Cover-Art-Link des lokalen DLNA-Servers. Wenn der Browser sich
            // im selben Netz befindet, laedt das Bild direkt.
            int caStart = p.rawContentItem.indexOf("<containerArt>");
            if (caStart >= 0) {
                int caEnd = p.rawContentItem.indexOf("</containerArt>", caStart);
                if (caEnd > caStart + 14) {
                    String ca = p.rawContentItem.substring(caStart + 14, caEnd);
                    if (ca.startsWith("http")) o["containerArt"] = ca;
                }
            }
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

// -----------------------------------------------------------------------------
// GET /api/speaker/{id}/preset/{n}/export
//   Liefert ein einzelnes Preset als JSON-Datei mit Content-Disposition
//   "attachment", damit Browser einen File-Download triggern (für drag-to-
//   desktop-Workflow). Schema ist kompatibel zum PUT-Endpoint:
//     { source, name, stationId, streamUrl, imageUrl, sixback_preset:1, exported_from }
// -----------------------------------------------------------------------------
void handleExportPreset(AsyncWebServerRequest* req) {
    String id  = req->pathArg(0);
    uint8_t slot = req->pathArg(1).toInt();
    if (slot < 1 || slot > 6) { req->send(400, "application/json", "{\"error\":\"slot 1..6\"}"); return; }
    auto p = sixback::PresetStore::instance().get(id, slot);
    JsonDocument doc;
    doc["sixback_preset"] = 1;     // schema marker for the importer
    doc["exported_from"]  = id;
    doc["slot"]           = slot;
    doc["source"]         = sixback::presetSourceToStr(p.source);
    doc["name"]           = p.name;
    doc["stationId"]      = p.stationId;
    doc["streamUrl"]      = p.streamUrl;
    doc["imageUrl"]       = p.imageUrl;
    if (p.source == sixback::PresetSource::OPAQUE) {
        doc["opaqueSourceName"] = p.opaqueSourceName;
    }
    String body; serializeJson(doc, body);

    // Filename: sanitized "<speaker-name>-slot-<n>.json" if we have a name in inventory,
    // else "preset-<dev>-<n>.json".
    String fname = "preset-" + id + "-" + String(slot) + ".json";
    {
        sixback::SpeakerInventory::LockGuard g(sixback::SpeakerInventory::instance());
        auto* sp = sixback::SpeakerInventory::instance().findById(id);
        if (sp && sp->name.length() > 0) {
            String safe;
            for (size_t i = 0; i < sp->name.length() && safe.length() < 32; ++i) {
                char c = sp->name[i];
                safe += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '-';
            }
            if (safe.length() > 0) fname = safe + "-slot-" + String(slot) + ".json";
        }
    }
    auto* resp = req->beginResponse(200, "application/json", body);
    resp->addHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    req->send(resp);
}

void handleDeletePreset(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    uint8_t slot = req->pathArg(1).toInt();
    bool ok = sixback::PresetStore::instance().clear(id, slot);
    req->send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"unknown\"}");
}

// -----------------------------------------------------------------------------
// GET /api/speaker/{id}/presets/export-set
//   Komplettes 6-Slot-Preset-Set eines Speakers als JSON-File.
//   Schema kompatibel zum Import-Endpunkt (siehe handleImportPresetsSet).
// -----------------------------------------------------------------------------
void handleExportPresetsSet(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto slots = sixback::PresetStore::instance().getForSpeaker(id);

    JsonDocument doc;
    doc["sixback_preset_set"] = 1;
    doc["exported_from"]      = id;
    String spName;
    {
        sixback::SpeakerInventory::LockGuard g(sixback::SpeakerInventory::instance());
        auto* sp = sixback::SpeakerInventory::instance().findById(id);
        if (sp) spName = sp->name;
    }
    doc["exported_speaker_name"] = spName;
    JsonArray arr = doc["presets"].to<JsonArray>();
    for (int i = 0; i < 6; ++i) {
        const sixback::Preset& p = (i < (int)slots.size()) ? slots[i] : sixback::Preset();
        JsonObject pj = arr.add<JsonObject>();
        pj["slot"]      = i + 1;
        pj["source"]    = sixback::presetSourceToStr(p.source);
        pj["name"]      = p.name;
        pj["stationId"] = p.stationId;
        pj["streamUrl"] = p.streamUrl;
        pj["imageUrl"]  = p.imageUrl;
        if (p.source == sixback::PresetSource::OPAQUE) {
            pj["rawContentItem"]   = p.rawContentItem;
            pj["opaqueSourceName"] = p.opaqueSourceName;
        }
    }
    String body; serializeJson(doc, body);

    String fname = "presets-" + id + ".json";
    if (spName.length() > 0) {
        String safe;
        for (size_t i = 0; i < spName.length() && safe.length() < 32; ++i) {
            char c = spName[i];
            safe += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '-';
        }
        if (safe.length() > 0) fname = safe + "-presets.json";
    }
    auto* resp = req->beginResponse(200, "application/json", body);
    resp->addHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    req->send(resp);
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/presets/import-set
//   Ersetzt das komplette 6-Slot-Set des Speakers durch das uebergebene JSON.
//   Akzeptiert sowohl das Export-Set-Schema (sixback_preset_set:1 mit
//   presets[]) als auch direkt ein presets[]-Array. Slots ohne Eintrag im
//   JSON werden gelöscht (=EMPTY). Push zur Hardware passiert NICHT
//   automatisch — User klickt danach den per-Slot- oder Push-All-Button.
// -----------------------------------------------------------------------------
void handleImportPresetsSet(AsyncWebServerRequest* req, JsonDocument& body) {
    String id = req->pathArg(0);
    auto& inv = sixback::SpeakerInventory::instance();
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        if (!inv.findById(id)) {
            req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}");
            return;
        }
    }

    JsonArray arr;
    if (body["presets"].is<JsonArray>())       arr = body["presets"].as<JsonArray>();
    else if (body.as<JsonArray>().size() > 0)  arr = body.as<JsonArray>();
    else {
        req->send(400, "application/json",
                  "{\"error\":\"expected {presets:[...]} or [...]\"}");
        return;
    }

    std::vector<sixback::Preset> slots(6);
    for (int i = 0; i < 6; ++i) {
        slots[i].slot   = i + 1;
        slots[i].source = sixback::PresetSource::EMPTY;
    }
    int n = 0;
    for (JsonObject pj : arr) {
        uint8_t slot = pj["slot"].as<uint8_t>();
        if (slot < 1 || slot > 6) continue;
        sixback::Preset& p = slots[slot - 1];
        p.slot      = slot;
        p.source    = sixback::presetSourceFromStr(String((const char*)(pj["source"] | "EMPTY")));
        p.name      = (const char*)(pj["name"]      | "");
        p.stationId = (const char*)(pj["stationId"] | "");
        p.streamUrl = (const char*)(pj["streamUrl"] | "");
        p.imageUrl  = (const char*)(pj["imageUrl"]  | "");
        p.rawContentItem   = (const char*)(pj["rawContentItem"]   | "");
        p.opaqueSourceName = (const char*)(pj["opaqueSourceName"] | "");
        if (p.source != sixback::PresetSource::EMPTY) ++n;
    }
    bool ok = sixback::PresetStore::instance().setSlots(id, slots);

    JsonDocument resp; resp["ok"] = ok; resp["imported"] = n;
    String b; serializeJson(resp, b);
    req->send(ok ? 200 : 500, "application/json", b);
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

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/preset/{n}/revert
//   Setzt EINEN Store-Slot zurueck auf den aktuellen Hardware-Stand des
//   Speakers. Genutzt vom UI als "✕ undo" auf einer noch nicht gepushten
//   (unsaved) Slot-Karte: der User hat eine Aenderung vorbereitet, will
//   sie aber doch nicht — Klick auf ✕ macht den Store wieder synchron
//   ohne dass die Hardware angefasst wird.
//
//   Verfahren: /presets-XML vom Speaker holen, Slot N extrahieren,
//   normalisieren (inkl. OPAQUE rawContentItem), Store-Slot ersetzen.
//   Wenn der Speaker den Slot leer hat → Store-Slot loeschen.
// -----------------------------------------------------------------------------
void handleRevertPresetToHw(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    uint8_t slot = req->pathArg(1).toInt();
    if (slot < 1 || slot > 6) {
        req->send(400, "application/json", "{\"error\":\"slot 1..6\"}");
        return;
    }
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(2000); http.setTimeout(3000);
    String url = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/presets";
    if (!http.begin(url)) {
        req->send(500, "application/json", "{\"error\":\"http begin failed\"}");
        return;
    }
    int code = http.GET();
    if (code != 200) {
        http.end();
        JsonDocument doc; doc["error"] = "speaker /presets http"; doc["http"] = code;
        String b; serializeJson(doc, b);
        req->send(502, "application/json", b);
        return;
    }
    String xml = http.getString();
    http.end();

    String openTag = "<preset id=\"" + String(slot) + "\"";
    int presetOpen = xml.indexOf(openTag);
    if (presetOpen < 0) {
        sixback::PresetStore::instance().clear(id, slot);
        JsonDocument doc; doc["ok"] = true; doc["action"] = "cleared"; doc["slot"] = slot;
        String b; serializeJson(doc, b);
        req->send(200, "application/json", b);
        return;
    }
    int idStart = presetOpen + openTag.length();
    int presetClose = xml.indexOf("</preset>", idStart);
    if (presetClose < 0) {
        req->send(502, "application/json", "{\"error\":\"malformed /presets xml\"}");
        return;
    }

    String src  = xmlExtractAttr(xml, idStart, presetClose, "source");
    String loc  = xmlExtractAttr(xml, idStart, presetClose, "location");
    String name = xmlExtractTag (xml, idStart, presetClose, "itemName");
    String img  = xmlExtractTag (xml, idStart, presetClose, "containerArt");

    sixback::Preset p;
    p.slot = slot;
    auto nr = sixback::normalizePreset(src, loc, name, img, p);

    if (nr.status == sixback::NormalizeStatus::OK_OPAQUE) {
        int ciOpen  = xml.indexOf("<ContentItem", idStart);
        int ciClose = xml.indexOf("</ContentItem>", ciOpen);
        if (ciOpen >= 0 && ciOpen < presetClose && ciClose > ciOpen) {
            p.rawContentItem = xml.substring(ciOpen, ciClose + 14);
        }
        if (p.name.length() == 0) p.name = String("[") + src + String("] preset");
    }
    if (nr.status == sixback::NormalizeStatus::ABANDONED) {
        sixback::PresetStore::instance().clear(id, slot);
        JsonDocument doc; doc["ok"] = true; doc["action"] = "cleared"; doc["slot"] = slot;
        doc["note"] = "speaker source not representable in store";
        String b; serializeJson(doc, b);
        req->send(200, "application/json", b);
        return;
    }

    bool ok = sixback::PresetStore::instance().set(id, p);
    JsonDocument doc;
    doc["ok"]     = ok;
    doc["action"] = "reverted";
    doc["slot"]   = slot;
    doc["source"] = sixback::presetSourceToStr(p.source);
    doc["name"]   = p.name;
    String b; serializeJson(doc, b);
    req->send(ok ? 200 : 500, "application/json", b);
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
        // sourceAccount: empirisch 2026-05-23 mit Emma:
        //   "TuneIn" -> HTTP 500 1005 UNKNOWN_SOURCE_ERROR
        //   ""       -> HTTP 200 + now_playing schaltet auf TUNEIN
        // Aeltere Memory-Notiz (reference_bose_select_sourceaccount.md) sagt
        // TUNEIN braeuchte "TuneIn" — gilt nicht auf der aktuellen Bose-FW
        // dieser Speaker (Emma SoundTouch 10 FW 27.0.3). Empty fuer beide
        // Source-Typen scheint zuverlaessig zu funktionieren.
        const char* srcAcct = "";
        // Per-source `type` attribute. Verified empirically against Emma
        // (ST10, FW 27.0.3) on 2026-05-23:
        //   TUNEIN  + type="stationurl" -> plays + slot saves correctly
        //   LOCAL_INTERNET_RADIO + type="stationurl" -> /select 200 but
        //       speaker dumps the ContentItem (now_playing returns empty
        //       with art SHOW_DEFAULT_IMAGE, no audio)
        //   LOCAL_INTERNET_RADIO + type="url" -> ContentItem echoes back,
        //       speaker starts streaming the URL.
        const char* ciType =
            (job.p.source == sixback::PresetSource::TUNEIN) ? "stationurl" : "url";
        String ci = "<ContentItem source=\"";
        ci += sixback::presetSourceToStr(job.p.source);
        ci += "\" type=\""; ci += ciType;
        ci += "\" location=\"";
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
    if (selectCode != 200) {
        // /select failed (network glitch, speaker STANDBY/busy, UNKNOWN_SOURCE,
        // etc.). Long-Press would save whatever happens to be playing at that
        // moment to this slot — almost guaranteed to be wrong. Abort.
        Serial.printf("[bg:push-preset] %s slot %u ABORT — /select=%d (no long-press attempted)\n",
                      job.spIp.c_str(), job.p.slot, selectCode);
        return;
    }
    delay(8000);  // Stream stabilisieren — 4 s war in der Praxis oft zu kurz
                  // (siehe Push-Reliability-Bug 2026-05-22): Bose-FW akzeptiert
                  // /select 200 bevor /now_playing tatsaechlich umgeschaltet ist,
                  // dann findet long-press kein "current playing" zum Speichern.
    // Long-Press PRESET_n
    auto sendKey = [&](const String& state) {
        HTTPClient h;
        h.setReuse(false);
        String u = "http://" + job.spIp + ":" + String(BOSE_BMX_PORT) + "/key";
        if (!h.begin(u)) return -1;
        h.addHeader("Content-Type", "text/xml");
        String body = "<key state=\"" + state + "\" sender=\"Gabbo\">PRESET_" + String(job.p.slot) + "</key>";
        int rc = h.POST(body);
        h.end();
        return rc;
    };
    int pressRc = sendKey("press");
    delay(2500);  // > 2s = Long-Press
    int releaseRc = sendKey("release");
    Serial.printf("[bg:push-preset] %s slot %u select=%d press=%d release=%d done\n",
                  job.spIp.c_str(), job.p.slot, selectCode, pressRc, releaseRc);
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
// POST /api/speaker/{id}/key/{KEY}
//   Proxy fuer Speaker-Side /key: kurzer Press + Release. KEY ist eines der
//   Bose-Tasten-Tokens (POWER, PRESET_1..6, AUX, BLUETOOTH, PLAY, PAUSE,
//   NEXT_TRACK, …). Erlaubt der WebUI die Hardware-Tasten zu simulieren ohne
//   CORS-Probleme mit dem Speaker-Endpoint.
// -----------------------------------------------------------------------------
void handleSpeakerKey(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    String key = req->pathArg(1);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    auto sendKey = [&](const String& state) {
        HTTPClient h;
        h.setReuse(false);
        String u = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/key";
        if (!h.begin(u)) return -1;
        h.addHeader("Content-Type", "text/xml");
        String body = "<key state=\"" + state + "\" sender=\"Gabbo\">" + key + "</key>";
        int rc = h.POST(body);
        h.end();
        return rc;
    };
    int p = sendKey("press");
    delay(150);  // Short press
    int r = sendKey("release");
    JsonDocument out;
    out["ok"] = (p == 200 && r == 200);
    out["key"] = key;
    out["press"] = p;
    out["release"] = r;
    String b; serializeJson(out, b);
    req->send(out["ok"] ? 200 : 502, "application/json", b);
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/play-preset/{n}
//   Short-press eines Preset-Slots am Speaker — spielt den Slot wie wenn man
//   physisch die Taste 1..6 drueckt. KEIN Long-Press, KEIN /select-Update.
//   Wenn Slot leer ist, macht der Speaker meistens nichts.
// -----------------------------------------------------------------------------
void handlePlayPreset(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    int slot = req->pathArg(1).toInt();
    if (slot < 1 || slot > 6) { req->send(400, "application/json", "{\"error\":\"slot 1..6\"}"); return; }
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    auto sendKey = [&](const String& state) {
        HTTPClient h;
        h.setReuse(false);
        String u = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/key";
        if (!h.begin(u)) return -1;
        h.addHeader("Content-Type", "text/xml");
        String body = "<key state=\"" + state + "\" sender=\"Gabbo\">PRESET_" + String(slot) + "</key>";
        int rc = h.POST(body);
        h.end();
        return rc;
    };
    int p = sendKey("press");
    delay(150);
    int r = sendKey("release");
    JsonDocument out;
    out["ok"] = (p == 200 && r == 200);
    out["slot"] = slot;
    String b; serializeJson(out, b);
    req->send(out["ok"] ? 200 : 502, "application/json", b);
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/play-source
//   Spielt eine Media-Source ad-hoc ab, OHNE einen Preset-Slot anzufassen.
//   Verwendet das gleiche /select-ContentItem wie doPush_ aber ohne den
//   nachfolgenden Long-Press. Use-case: Library-Tile-Preview in der WebUI —
//   User clickt ▶ auf einer Sidebar-Tile, hoert sofort den Stream im
//   Speaker, kann dann mit D&D dauerhaft in einen Slot legen.
// Body JSON:
//   { "source": "TUNEIN" | "LOCAL_INTERNET_RADIO",
//     "name":   "<itemName>",
//     "stationId": "s24896",       (TUNEIN only)
//     "streamUrl": "http://…"      (LOCAL_INTERNET_RADIO only) }
// -----------------------------------------------------------------------------
void handlePlaySource(AsyncWebServerRequest* req, JsonDocument& body) {
    String id = req->pathArg(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    String srcStr = String((const char*)(body["source"] | ""));
    sixback::PresetSource src = sixback::presetSourceFromStr(srcStr);
    if (src != sixback::PresetSource::TUNEIN &&
        src != sixback::PresetSource::LOCAL_INTERNET_RADIO) {
        req->send(400, "application/json",
                  "{\"error\":\"source must be TUNEIN or LOCAL_INTERNET_RADIO\"}");
        return;
    }
    String name      = (const char*)(body["name"]      | "");
    String stationId = (const char*)(body["stationId"] | "");
    String streamUrl = (const char*)(body["streamUrl"] | "");
    if (src == sixback::PresetSource::TUNEIN && stationId.length() == 0) {
        req->send(400, "application/json", "{\"error\":\"stationId required for TUNEIN\"}"); return;
    }
    if (src == sixback::PresetSource::LOCAL_INTERNET_RADIO && streamUrl.length() == 0) {
        req->send(400, "application/json", "{\"error\":\"streamUrl required for LOCAL_INTERNET_RADIO\"}"); return;
    }
    HTTPClient http;
    http.setReuse(false);
    String url = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/select";
    int code = -1;
    if (http.begin(url)) {
        // Match doPush_ exactly: per-source `type` attribute. See doPush_
        // comment block — TUNEIN uses "stationurl", LOCAL_INTERNET_RADIO
        // uses "url" (the latter verified 2026-05-23 against Emma).
        const char* ciType = (src == sixback::PresetSource::TUNEIN) ? "stationurl" : "url";
        String ci = "<ContentItem source=\"";
        ci += sixback::presetSourceToStr(src);
        ci += "\" type=\""; ci += ciType;
        ci += "\" location=\"";
        if (src == sixback::PresetSource::TUNEIN) {
            ci += "/v1/playback/station/" + stationId;
        } else {
            ci += streamUrl;
        }
        ci += "\" sourceAccount=\"\" isPresetable=\"true\"><itemName>" + name + "</itemName></ContentItem>";
        http.addHeader("Content-Type", "text/xml");
        code = http.POST(ci);
        http.end();
    }
    JsonDocument out;
    out["ok"]     = (code == 200);
    out["select"] = code;
    out["name"]   = name;
    out["source"] = sixback::presetSourceToStr(src);
    String b; serializeJson(out, b);
    req->send(code == 200 ? 200 : 502, "application/json", b);
}

// -----------------------------------------------------------------------------
// GET /api/speaker/{id}/now-playing-live
//   Proxy fuer Speaker /now_playing (Live-Pull, nicht aus dem SCMUDC-Event-
//   Cache). Gibt getrimmte JSON-Sicht zurueck mit location-derived
//   station_id und stream_url — das braucht der WebUI-Highlight um Slots
//   per stationId/streamUrl zu matchen, was die SCMUDC-Variante nicht kennt.
// -----------------------------------------------------------------------------
void handleNowPlayingLive(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    HTTPClient h;
    h.setReuse(false);
    String u = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/now_playing";
    if (!h.begin(u)) { req->send(502, "application/json", "{\"error\":\"http begin failed\"}"); return; }
    int code = h.GET();
    if (code != 200) {
        h.end();
        req->send(502, "application/json", String("{\"error\":\"speaker HTTP\",\"code\":") + code + "}");
        return;
    }
    String xml = h.getString();
    h.end();

    auto extractAttr = [&](const String& tag, const String& attr) -> String {
        int t = xml.indexOf("<" + tag);
        if (t < 0) return String();
        int e = xml.indexOf('>', t);
        if (e < 0) return String();
        String hdr = xml.substring(t, e);
        String needle = attr + "=\"";
        int s = hdr.indexOf(needle);
        if (s < 0) return String();
        s += needle.length();
        int q = hdr.indexOf('"', s);
        return q > s ? hdr.substring(s, q) : String();
    };
    auto extractTag = [&](const String& tag) -> String {
        String open  = "<" + tag + ">";
        String close = "</" + tag + ">";
        int s = xml.indexOf(open);
        if (s < 0) return String();
        s += open.length();
        int e = xml.indexOf(close, s);
        return e > s ? xml.substring(s, e) : String();
    };

    String src      = extractAttr("ContentItem", "source");
    String srcAcct  = extractAttr("ContentItem", "sourceAccount");
    String location = extractAttr("ContentItem", "location");
    String itemName = extractTag("itemName");
    String trackName = extractTag("track");
    String playStatus = extractTag("playStatus");
    String art = extractTag("art");

    // location für TUNEIN ist /v1/playback/station/<id> -> nimm den letzten Pfad-Teil als stationId
    String stationId;
    if (src == "TUNEIN" && location.startsWith("/v1/playback/station/")) {
        stationId = location.substring(21);
    }

    JsonDocument out;
    out["source"] = src;
    out["source_account"] = srcAcct;
    out["name"] = itemName;
    out["track"] = trackName;
    out["station_id"] = stationId;
    out["stream_url"] = (src == "LOCAL_INTERNET_RADIO") ? location : "";
    out["art_url"] = art;
    out["play_status"] = playStatus;
    String body; serializeJson(out, body);
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// Helper: Speaker-Side /presets-XML fetchen + grob parsen.
// Returnt true wenn Fetch+Parse erfolgreich. Slot-Slots ohne preset bleiben
// mit leerem source. Synchroner HTTP-Call (~100-300 ms) — akzeptabel weil
// nur 1× pro push-all User-Klick aufgerufen.
// -----------------------------------------------------------------------------
struct HwSlotInfo_ {
    String source;     // "TUNEIN" / "LOCAL_INTERNET_RADIO" / "STORED_MUSIC" / "" (empty)
    String location;   // raw "location" attr value
    String name;       // <itemName>
};

static bool fetchHardwarePresets_(const String& spIp, HwSlotInfo_ out[6]) {
    HTTPClient h; h.setReuse(false);
    h.setTimeout(3000);
    String url = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/presets";
    if (!h.begin(url)) return false;
    int code = h.GET();
    if (code != 200) { h.end(); return false; }
    String xml = h.getString();
    h.end();
    for (int slot = 1; slot <= 6; ++slot) {
        String tag = "<preset id=\"" + String(slot) + "\"";
        int idx = xml.indexOf(tag);
        if (idx < 0) continue;
        int end = xml.indexOf("</preset>", idx);
        if (end < 0) continue;
        String block = xml.substring(idx, end);
        int ci = block.indexOf("<ContentItem");
        if (ci < 0) continue;
        int ciEnd = block.indexOf('>', ci);
        if (ciEnd < 0) continue;
        String hdr = block.substring(ci, ciEnd);
        auto extract = [&](const String& attr) -> String {
            String n = attr + "=\"";
            int s = hdr.indexOf(n);
            if (s < 0) return String();
            s += n.length();
            int e = hdr.indexOf('"', s);
            return e > s ? hdr.substring(s, e) : String();
        };
        out[slot-1].source   = extract("source");
        out[slot-1].location = extract("location");
        int nm = block.indexOf("<itemName>");
        if (nm >= 0) {
            int ne = block.indexOf("</itemName>", nm);
            if (ne > nm) out[slot-1].name = block.substring(nm + 10, ne);
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// GET /api/speaker/{id}/hardware-presets
//   Liest /presets vom Speaker live aus (kein Store-Touch) und liefert
//   strukturierte JSON zurueck. Wird von der WebUI fuer die "on speaker:"-
//   Zeile in der Slot-Karte gebraucht, parallel zum Store-Endpoint.
// -----------------------------------------------------------------------------
void handleGetHardwarePresets(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    HwSlotInfo_ hw[6];
    bool ok = fetchHardwarePresets_(spIp, hw);
    JsonDocument doc;
    doc["ok"] = ok;
    JsonArray slots = doc["slots"].to<JsonArray>();
    for (uint8_t s = 1; s <= 6; ++s) {
        JsonObject o = slots.add<JsonObject>();
        o["slot"]     = s;
        o["source"]   = hw[s-1].source;
        o["location"] = hw[s-1].location;
        o["name"]     = hw[s-1].name;
        if (hw[s-1].source == "TUNEIN" && hw[s-1].location.startsWith("/v1/playback/station/")) {
            o["station_id"] = hw[s-1].location.substring(strlen("/v1/playback/station/"));
        }
    }
    String body; serializeJson(doc, body);
    req->send(ok ? 200 : 502, "application/json", body);
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/push-all
//   Pusht nur die Slots wo Store-Stand != Hardware-Stand. Holt initial das
//   Speaker-/presets-XML als Snapshot, vergleicht jeden Slot, und enqueued
//   nur die Diff-Slots. Beispiel: bei einem Slot 1↔2 Swap werden 2 von 6
//   Slots gepushed, nicht alle 6 (~30s vs ~75s).
//   Skipt: EMPTY (store), OPAQUE (store), und Slots die schon korrekt am
//   Speaker liegen ("synced" im Response).
//   Reihenfolge im Push: slot 1..6 in ascending order.
//
//   Warum manueller Trigger statt auto-push pro PUT/swap: siehe Comment in
//   handleSwapPresets — auto-push war flaky.
// -----------------------------------------------------------------------------
void handleBulkPushPresets(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }

    if (!g_pushQueue) {
        g_pushQueue = xQueueCreate(PUSH_QUEUE_DEPTH, sizeof(PushPresetJob_*));
        if (!g_pushQueue) {
            req->send(503, "application/json", "{\"error\":\"push queue alloc failed\"}"); return;
        }
        BaseType_t tr = xTaskCreate(pushPresetWorker_, "push-worker", 4096,
                                     nullptr, tskIDLE_PRIORITY + 1, &g_pushTask);
        if (tr != pdPASS) {
            vQueueDelete(g_pushQueue);
            g_pushQueue = nullptr;
            req->send(503, "application/json", "{\"error\":\"push worker spawn failed\"}"); return;
        }
    }

    HwSlotInfo_ hw[6];
    bool hwOk = fetchHardwarePresets_(spIp, hw);

    auto& store = sixback::PresetStore::instance();
    int enqueued = 0, synced = 0, skippedOpaque = 0, skippedEmpty = 0;
    for (uint8_t slot = 1; slot <= 6; ++slot) {
        auto p = store.get(id, slot);
        if (p.source == sixback::PresetSource::EMPTY)  { skippedEmpty++;  continue; }
        if (p.source == sixback::PresetSource::OPAQUE) { skippedOpaque++; continue; }

        if (hwOk) {
            const auto& h = hw[slot-1];
            bool already = false;
            if (p.source == sixback::PresetSource::TUNEIN && h.source == "TUNEIN") {
                String wantLoc = "/v1/playback/station/" + p.stationId;
                if (h.location == wantLoc && h.name == p.name) already = true;
            } else if (p.source == sixback::PresetSource::LOCAL_INTERNET_RADIO
                       && h.source == "LOCAL_INTERNET_RADIO") {
                if (h.location == p.streamUrl && h.name == p.name) already = true;
            }
            if (already) { synced++; continue; }
        }

        auto* job = new PushPresetJob_{spIp, p};
        if (xQueueSend(g_pushQueue, &job, 0) != pdTRUE) {
            delete job;
            req->send(503, "application/json",
                      "{\"error\":\"push queue full mid-bulk — retry in ~1 min\"}");
            return;
        }
        enqueued++;
    }

    UBaseType_t depth = uxQueueMessagesWaiting(g_pushQueue);
    JsonDocument out;
    out["ok"]            = true;
    out["enqueued"]      = enqueued;
    out["synced"]        = synced;        // slots already matching the hardware
    out["skipped_empty"] = skippedEmpty;
    out["skipped_opaque"]= skippedOpaque;
    out["hw_fetched"]    = hwOk;
    out["queue_depth"]   = (unsigned)depth;
    out["eta_seconds"]   = enqueued * 13;
    if (enqueued == 0) {
        out["message"] = hwOk
            ? "all slots already in sync with speaker — nothing to push"
            : "no slots needed pushing (speaker unreachable for diff, all slots EMPTY/OPAQUE)";
    } else {
        out["message"] = hwOk
            ? "pushing only the diff — " + String(enqueued) + " of 6 slots"
            : "pushing all non-empty slots (speaker /presets unreachable for diff)";
    }
    String body; serializeJson(out, body);
    req->send(202, "application/json", body);
}

// -----------------------------------------------------------------------------
// POST /api/preset/swap
//   body: { "src": {"dev":"...", "slot":N}, "dst": {"dev":"...", "slot":M} }
//   Tauscht zwei Slots (zwischen oder innerhalb von Speakern). Beide Stores
//   atomic update, beide Pushes in die existierende push-queue enqueued.
//   OPAQUE als Source oder Target verboten (rawContentItem ist DLNA-source-
//   spezifisch, Cross-Speaker-Replay nicht garantiert). Empty-Source ist
//   erlaubt = effektiv MOVE.
// -----------------------------------------------------------------------------
void handleSwapPresets(AsyncWebServerRequest* req, JsonDocument& body) {
    if (!body["src"].is<JsonObject>() || !body["dst"].is<JsonObject>()) {
        req->send(400, "application/json",
                  "{\"error\":\"src/dst required as objects\"}");
        return;
    }
    String srcDev = (const char*)(body["src"]["dev"] | "");
    String dstDev = (const char*)(body["dst"]["dev"] | "");
    int srcSlot = body["src"]["slot"] | 0;
    int dstSlot = body["dst"]["slot"] | 0;
    if (srcDev.isEmpty() || dstDev.isEmpty() ||
        srcSlot < 1 || srcSlot > 6 || dstSlot < 1 || dstSlot > 6) {
        req->send(400, "application/json",
                  "{\"error\":\"src.dev/dst.dev required, slots 1..6\"}");
        return;
    }
    if (srcDev == dstDev && srcSlot == dstSlot) {
        req->send(200, "application/json", "{\"ok\":true,\"noop\":true}");
        return;
    }

    auto& inv = sixback::SpeakerInventory::instance();
    String srcIp, dstIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp1 = inv.findById(srcDev);
        auto* sp2 = inv.findById(dstDev);
        if (!sp1) { req->send(404, "application/json", "{\"error\":\"unknown src.dev\"}"); return; }
        if (!sp2) { req->send(404, "application/json", "{\"error\":\"unknown dst.dev\"}"); return; }
        srcIp = sp1->ip;
        dstIp = sp2->ip;
    }

    auto& store = sixback::PresetStore::instance();
    auto pSrc = store.get(srcDev, srcSlot);
    auto pDst = store.get(dstDev, dstSlot);

    if (pSrc.source == sixback::PresetSource::OPAQUE ||
        pDst.source == sixback::PresetSource::OPAQUE) {
        req->send(400, "application/json",
            "{\"error\":\"OPAQUE preset cannot be swapped via WebUI — "
            "re-record at speaker via long-press\"}");
        return;
    }

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

    sixback::Preset newDst = pSrc; newDst.slot = (uint8_t)dstSlot;
    sixback::Preset newSrc = pDst; newSrc.slot = (uint8_t)srcSlot;

    bool ok1 = (newDst.source == sixback::PresetSource::EMPTY)
               ? store.clear(dstDev, dstSlot)
               : store.set(dstDev, newDst);
    bool ok2 = (newSrc.source == sixback::PresetSource::EMPTY)
               ? store.clear(srcDev, srcSlot)
               : store.set(srcDev, newSrc);
    if (!ok1 || !ok2) {
        req->send(500, "application/json", "{\"error\":\"store update failed\"}");
        return;
    }

    // Swap aktualisiert NUR den Store. Push zum Speaker passiert ueber den
    // expliziten "Push to speaker"-Button (handleBulkPushPresets), nicht
    // automatisch — siehe Forum-Feedback 2026-05-23: auto-push war flaky
    // (Speaker STANDBY, /select transient errors, timing-races) und tauschte
    // hardware-Slots oft nicht zuverlaessig. Manueller Trigger erlaubt User
    // die Slots in Ruhe zu komponieren + bewusst zu commit'n.

    JsonDocument out;
    out["ok"] = true;
    out["pushed"] = false;
    out["message"] = "store updated — click 'Push 1-6 to speaker' on each affected speaker to write to hardware";
    String s; serializeJson(out, s);
    req->send(200, "application/json", s);
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
    // index.html ist >100 KB; AsyncWebServer schafft das im uncompressed-Pfad
    // nicht zuverlaessig (Truncation/Timeouts unter Last). Wenn index.html.gz
    // im LittleFS liegt -> serve mit Content-Encoding: gzip. Browser entpackt.
    if (LittleFS.exists("/index.html.gz")) {
        auto* resp = req->beginResponse(LittleFS, "/index.html.gz", "text/html");
        resp->addHeader("Content-Encoding", "gzip");
        req->send(resp);
        return;
    }
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

// -----------------------------------------------------------------------------
// Spotify slot-mapping API (S3-only, siehe SIXBACK_SPOTIFY_ENABLED in
// platformio.ini)
// -----------------------------------------------------------------------------
#ifdef SIXBACK_SPOTIFY_ENABLED

// GET /api/spotify/slots  — alle aktuellen Mappings
void handleSpotifySlotsList(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["slots"].to<JsonArray>();
    for (const auto& m : sixback::spotify::listSlots()) {
        JsonObject o = arr.add<JsonObject>();
        o["device_id"]    = m.deviceId;
        o["slot"]         = m.slot;
        o["spotify_uri"]  = m.spotifyUri;
        o["display_name"] = m.displayName;
    }
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// PUT /api/spotify/slot/{deviceId}/{slot}  Body: {"uri":"spotify:...","name":"..."}
// Empty uri -> Mapping loeschen.
void handleSpotifySlotPut(AsyncWebServerRequest* req, JsonDocument& body) {
    String deviceId = req->pathArg(0);
    int slot = req->pathArg(1).toInt();
    if (slot < 1 || slot > 6) {
        req->send(400, "application/json", "{\"error\":\"slot 1..6\"}");
        return;
    }
    String uri  = (const char*)(body["uri"]  | "");
    String name = (const char*)(body["name"] | "");
    if (uri.length() > 0 && !uri.startsWith("spotify:")) {
        req->send(400, "application/json",
                  "{\"error\":\"uri must start with spotify:\"}");
        return;
    }
    sixback::spotify::setSlot(deviceId, slot, uri, name);
    req->send(200, "application/json", "{\"ok\":true}");
}

// PUT /api/spotify/auth  Body: {clientId, clientSecret, refreshToken, [accessToken], [expiresInSec]}
// Phase-A/B-stop-gap: User holt manuell einen Token via curl-Setup und stopft
// ihn hier rein. Phase-B-OAuth-Flow wird das ersetzen.
void handleSpotifyAuthPut(AsyncWebServerRequest* req, JsonDocument& body) {
    sixback::spotify::AuthState s;
    s.clientId     = (const char*)(body["clientId"]     | "");
    s.clientSecret = (const char*)(body["clientSecret"] | "");
    s.refreshToken = (const char*)(body["refreshToken"] | "");
    s.accessToken  = (const char*)(body["accessToken"]  | "");
    int expiresIn  = body["expiresInSec"] | 3600;
    s.expiresAtMs  = (uint64_t)millis() + (uint64_t)expiresIn * 1000;
    s.spotifyUserId      = (const char*)(body["spotifyUserId"]      | "");
    s.spotifyDisplayName = (const char*)(body["spotifyDisplayName"] | "");
    if (s.clientId.length() == 0 || s.clientSecret.length() == 0
        || s.refreshToken.length() == 0) {
        req->send(400, "application/json",
                  "{\"error\":\"clientId+clientSecret+refreshToken required\"}");
        return;
    }
    sixback::spotify::setAuth(s);
    req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/spotify/auth — Status ohne Secrets.
void handleSpotifyAuthGet(AsyncWebServerRequest* req) {
    auto s = sixback::spotify::getAuth();
    JsonDocument doc;
    doc["configured"]      = sixback::spotify::isAuthConfigured();
    doc["client_id"]       = s.clientId;          // public part nur
    doc["user_id"]         = s.spotifyUserId;
    doc["display_name"]    = s.spotifyDisplayName;
    int64_t rem = (int64_t)s.expiresAtMs - (int64_t)millis();
    doc["expires_in_sec"]  = rem / 1000;
    doc["has_access_token"]= s.accessToken.length() > 0;
    doc["has_refresh_token"]= s.refreshToken.length() > 0;
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// GET /api/spotify/auth/url?client_id=X&redirect_uri=Y
// Liefert die Spotify-OAuth-URL fuer den Browser-Auth-Flow.
// redirect_uri default: http://127.0.0.1:8888/callback (User kopiert URL aus
// 404-Page).
void handleSpotifyAuthUrl(AsyncWebServerRequest* req) {
    String clientId    = req->hasParam("client_id")    ? req->getParam("client_id")->value() : String();
    String redirectUri = req->hasParam("redirect_uri") ? req->getParam("redirect_uri")->value()
                                                      : String("http://127.0.0.1:8888/callback");
    if (clientId.length() == 0) {
        req->send(400, "application/json", "{\"error\":\"client_id required\"}");
        return;
    }
    String authUrl = sixback::spotify::genAuthUrl(clientId, redirectUri);
    JsonDocument doc;
    doc["auth_url"]     = authUrl;
    doc["redirect_uri"] = redirectUri;
    doc["client_id"]    = clientId;
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// POST /api/spotify/auth/exchange
//   Body: {code, clientId, clientSecret, redirectUri}
// Tauscht Code gegen Tokens + fetcht /v1/me + persistiert via setAuth().
void handleSpotifyAuthExchange(AsyncWebServerRequest* req, JsonDocument& body) {
    String code         = (const char*)(body["code"]         | "");
    String clientId     = (const char*)(body["clientId"]     | "");
    String clientSecret = (const char*)(body["clientSecret"] | "");
    String redirectUri  = (const char*)(body["redirectUri"]  | "");
    if (code.length() == 0 || clientId.length() == 0
        || clientSecret.length() == 0 || redirectUri.length() == 0) {
        req->send(400, "application/json",
                  "{\"error\":\"code+clientId+clientSecret+redirectUri required\"}");
        return;
    }
    String err;
    if (!sixback::spotify::exchangeAuthCode(code, clientId, clientSecret,
                                            redirectUri, err)) {
        JsonDocument out;
        out["ok"]    = false;
        out["error"] = err;
        String s; serializeJson(out, s);
        req->send(400, "application/json", s);
        return;
    }
    auto st = sixback::spotify::getAuth();
    JsonDocument out;
    out["ok"]              = true;
    out["user_id"]         = st.spotifyUserId;
    out["display_name"]    = st.spotifyDisplayName;
    out["expires_in_sec"]  = ((int64_t)st.expiresAtMs - (int64_t)millis()) / 1000;
    String s; serializeJson(out, s);
    req->send(200, "application/json", s);
}

// GET /api/spotify/last-trigger — letzte 8 Trigger-Versuche fuer Debug.
void handleSpotifyLastTrigger(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["triggers"].to<JsonArray>();
    uint32_t now = millis();
    for (auto& e : sixback::spotify::listTriggerLog()) {
        JsonObject o = arr.add<JsonObject>();
        o["ts_ms"]            = e.ts_ms;
        o["age_ms"]           = (int32_t)(now - e.ts_ms);
        o["device_id"]        = e.deviceId;
        o["slot"]             = e.slot;
        o["uri"]              = e.uri;
        o["speaker_name"]     = e.speakerName;
        o["spotify_device_id"]= e.spotifyDeviceId;
        o["play_http_code"]   = e.playHttpCode;
        o["result"]           = e.result;
    }
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// DELETE /api/spotify/auth — clear auth (Disconnect Spotify).
void handleSpotifyAuthDelete(AsyncWebServerRequest* req) {
    sixback::spotify::AuthState empty;
    sixback::spotify::setAuth(empty);
    req->send(200, "application/json", "{\"ok\":true}");
}

// DELETE /api/spotify/slot/{deviceId}/{slot}
void handleSpotifySlotDelete(AsyncWebServerRequest* req) {
    String deviceId = req->pathArg(0);
    int slot = req->pathArg(1).toInt();
    if (slot < 1 || slot > 6) {
        req->send(400, "application/json", "{\"error\":\"slot 1..6\"}");
        return;
    }
    sixback::spotify::setSlot(deviceId, slot, "", "");
    req->send(200, "application/json", "{\"ok\":true}");
}

#endif // SIXBACK_SPOTIFY_ENABLED

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
    ui.on("^/api/speaker/([^/]+)/reboot$",         HTTP_POST, handleReboot);
    ui.on("/api/speakers/refresh-status",          HTTP_POST, handleRefreshStatus);

    ui.on("^/api/speaker/([^/]+)/presets$",        HTTP_GET,    handleGetPresets);
    routeJsonBody(ui, "^/api/speaker/([^/]+)/preset/([1-6])$", HTTP_PUT, handlePutPreset);
    ui.on("^/api/speaker/([^/]+)/preset/([1-6])$", HTTP_DELETE, handleDeletePreset);
    ui.on("^/api/speaker/([^/]+)/preset/([1-6])/export$", HTTP_GET, handleExportPreset);
    ui.on("^/api/speaker/([^/]+)/preset/([1-6])/push-to-device$",
          HTTP_POST, handlePushPresetToDevice);
    ui.on("^/api/speaker/([^/]+)/preset/([1-6])/revert$",
          HTTP_POST, handleRevertPresetToHw);
    ui.on("^/api/speaker/([^/]+)/presets/import-from-device$",
          HTTP_POST, handleImportFromDevice);
    ui.on("^/api/speaker/([^/]+)/presets/export-set$",
          HTTP_GET, handleExportPresetsSet);
    routeJsonBody(ui, "^/api/speaker/([^/]+)/presets/import-set$",
                  HTTP_POST, handleImportPresetsSet);
    routeJsonBody(ui, "/api/preset/swap", HTTP_POST, handleSwapPresets);
    ui.on("^/api/speaker/([^/]+)/push-all$", HTTP_POST, handleBulkPushPresets);
    ui.on("^/api/speaker/([^/]+)/key/([A-Z_0-9]+)$", HTTP_POST, handleSpeakerKey);
    ui.on("^/api/speaker/([^/]+)/play-preset/([1-6])$", HTTP_POST, handlePlayPreset);
    routeJsonBody(ui, "^/api/speaker/([^/]+)/play-source$", HTTP_POST, handlePlaySource);
    ui.on("^/api/speaker/([^/]+)/hardware-presets$", HTTP_GET, handleGetHardwarePresets);
    ui.on("^/api/speaker/([^/]+)/now-playing-live$", HTTP_GET, handleNowPlayingLive);

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

#ifdef SIXBACK_SPOTIFY_ENABLED
    ui.on("/api/spotify/slots",                            HTTP_GET,    handleSpotifySlotsList);
    routeJsonBody(ui, "^/api/spotify/slot/([^/]+)/([1-6])$", HTTP_PUT,  handleSpotifySlotPut);
    ui.on("^/api/spotify/slot/([^/]+)/([1-6])$",           HTTP_DELETE, handleSpotifySlotDelete);
    // ASYNCWEBSERVER_REGEX=1 — Path-strings ohne ^$ matchen als Prefix.
    // /api/spotify/auth wuerde sonst /api/spotify/auth/url + /exchange
    // mitschlucken. Explizit anchored:
    ui.on("^/api/spotify/auth$",                           HTTP_GET,    handleSpotifyAuthGet);
    routeJsonBody(ui, "^/api/spotify/auth$",               HTTP_PUT,    handleSpotifyAuthPut);
    ui.on("^/api/spotify/auth$",                           HTTP_DELETE, handleSpotifyAuthDelete);
    ui.on("^/api/spotify/auth/url$",                       HTTP_GET,    handleSpotifyAuthUrl);
    routeJsonBody(ui, "^/api/spotify/auth/exchange$",      HTTP_POST,   handleSpotifyAuthExchange);
    ui.on("^/api/spotify/last-trigger$",                   HTTP_GET,    handleSpotifyLastTrigger);
#endif

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
