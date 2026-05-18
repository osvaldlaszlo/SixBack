// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Verwaltungs-REST-API (Port 80)
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
#include "auto_mode.h"
#include "source_normalizer.h"
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
    wifi["improv_active"]   = bosefix::improvIsActive();
    wifi["improv_window_s"] = bosefix::improvWindowRemainingS();
    wifi["captive_active"]   = bosefix::captiveIsActive();
    wifi["captive_window_s"] = bosefix::captiveWindowRemainingS();

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
    doc["speakers_count"]    = bosefix::SpeakerInventory::instance().list().size();
    doc["scan_in_progress"]  = bosefix::SpeakerInventory::instance().isScanRunning();

    // Health-Snapshot: boot/crash-counter, last reset reason, watchdog state
    JsonObject health = doc["health"].to<JsonObject>();
    bosefix::healthToJson(health);

    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// Speaker-Liste + Discovery
// -----------------------------------------------------------------------------
void emitSpeakers(AsyncWebServerRequest* req, JsonDocument& doc) {
    JsonArray arr = doc["speakers"].to<JsonArray>();
    auto& inv = bosefix::SpeakerInventory::instance();
    auto& ps  = bosefix::PresetStore::instance();
    for (auto& s : inv.list()) {
        JsonObject o = arr.add<JsonObject>();
        o["device_id"] = s.deviceId;
        o["name"]      = s.name;
        o["model"]     = s.model;
        o["firmware"]  = s.firmware;
        o["ip"]        = s.ip;
        o["account_id"]= s.accountId;
        o["status"]      = bosefix::migrationStatusToStr(s.status);
        o["cloud_url"]   = s.cloudUrl;
        o["owned_by_us"] = s.ownedByUs;
        o["group_id"]    = s.groupId;
        // Anzahl belegter Preset-Slots
        int n = 0;
        for (auto& p : ps.getForSpeaker(s.deviceId)) {
            if (p.source != bosefix::PresetSource::EMPTY) ++n;
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

void handleDiscover(AsyncWebServerRequest* req) {
    bosefix::SpeakerInventory::instance().discover();
    JsonDocument doc;
    emitSpeakers(req, doc);
}

void handleSpeakerAdd(AsyncWebServerRequest* req, JsonDocument& body) {
    String ip = (const char*)(body["ip"] | "");
    if (ip.length() == 0) { req->send(400, "application/json", "{\"error\":\"ip required\"}"); return; }
    bool ok = bosefix::SpeakerInventory::instance().addByIp(ip);
    req->send(ok ? 200 : 404, "application/json",
              ok ? "{\"ok\":true}" : "{\"error\":\"speaker not reachable\"}");
}

void handleSpeakerDelete(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    bool ok = bosefix::SpeakerInventory::instance().remove(id);
    req->send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"not found\"}");
}

// -----------------------------------------------------------------------------
// Speaker-Aktionen: migrate / revert / reboot / refresh-status
// -----------------------------------------------------------------------------
void handleMigrate(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto* sp = bosefix::SpeakerInventory::instance().findById(id);
    if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
    auto r = migrateSpeaker(sp->ip, myBaseUrl());
    if (r.ok) {
        sp->status     = bosefix::MigrationStatus::MIGRATED;
        sp->cloudUrl   = myBaseUrl();
        sp->ownedByUs  = true;   // gehoert ab jetzt UNS - ip_failsafe pflegt es
        bosefix::SpeakerInventory::instance().saveToNVS();
    }
    JsonDocument doc;
    doc["ok"]              = r.ok;
    doc["message"]         = r.message;
    doc["verified_config"] = r.verifiedConfig;
    String body; serializeJson(doc, body);
    req->send(r.ok ? 200 : 500, "application/json", body);
}

void handleRevert(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto* sp = bosefix::SpeakerInventory::instance().findById(id);
    if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
    auto r = revertSpeaker(sp->ip);
    if (r.ok) {
        sp->status    = bosefix::MigrationStatus::NOT_MIGRATED;
        sp->cloudUrl  = "https://streaming.bose.com";
        sp->ownedByUs = false;
        bosefix::SpeakerInventory::instance().saveToNVS();
    }
    JsonDocument doc;
    doc["ok"]              = r.ok;
    doc["message"]         = r.message;
    doc["verified_config"] = r.verifiedConfig;
    String body; serializeJson(doc, body);
    req->send(r.ok ? 200 : 500, "application/json", body);
}

void handleReboot(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto* sp = bosefix::SpeakerInventory::instance().findById(id);
    if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
    bool ok = rebootSpeaker(sp->ip);
    req->send(ok ? 200 : 500, "application/json",
              ok ? "{\"ok\":true,\"message\":\"speaker reboot triggered\"}"
                 : "{\"error\":\"telnet failed\"}");
}

void handleRefreshStatus(AsyncWebServerRequest* req) {
    bosefix::SpeakerInventory::instance().refreshMigrationStatus();
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
    for (auto& p : bosefix::PresetStore::instance().getForSpeaker(id)) {
        JsonObject o = arr.add<JsonObject>();
        o["slot"]      = p.slot;
        o["source"]    = bosefix::presetSourceToStr(p.source);
        o["name"]      = p.name;
        o["stationId"] = p.stationId;
        o["streamUrl"] = p.streamUrl;
        o["imageUrl"]  = p.imageUrl;
    }
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

void handlePutPreset(AsyncWebServerRequest* req, JsonDocument& body) {
    String id  = req->pathArg(0);
    uint8_t slot = req->pathArg(1).toInt();
    if (slot < 1 || slot > 6) { req->send(400, "application/json", "{\"error\":\"slot 1..6\"}"); return; }
    bosefix::Preset p;
    p.slot      = slot;
    p.source    = bosefix::presetSourceFromStr(String((const char*)(body["source"] | "TUNEIN")));
    p.name      = (const char*)(body["name"]      | "");
    p.stationId = (const char*)(body["stationId"] | "");
    p.streamUrl = (const char*)(body["streamUrl"] | "");
    p.imageUrl  = (const char*)(body["imageUrl"]  | "");
    bool ok = bosefix::PresetStore::instance().set(id, p);
    req->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"set failed\"}");
}

void handleDeletePreset(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    uint8_t slot = req->pathArg(1).toInt();
    bool ok = bosefix::PresetStore::instance().clear(id, slot);
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

void handleImportFromDevice(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto* sp = bosefix::SpeakerInventory::instance().findById(id);
    if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }

    HTTPClient http;
    String url = "http://" + sp->ip + ":" + String(BOSE_BMX_PORT) + "/presets";
    http.setConnectTimeout(2000); http.setTimeout(3000);
    if (!http.begin(url)) {
        req->send(500, "application/json", "{\"error\":\"http begin\"}"); return;
    }
    int code = http.GET();
    if (code != 200) {
        http.end();
        req->send(502, "application/json",
                  String("{\"error\":\"speaker http ") + code + "\"}");
        return;
    }
    String xml = http.getString();
    http.end();

    JsonDocument resp;
    JsonArray imported  = resp["imported"].to<JsonArray>();
    JsonArray abandoned = resp["abandoned"].to<JsonArray>();
    int countOk = 0, countAban = 0;
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

        bosefix::Preset p;
        p.slot = slot;
        auto nr = bosefix::normalizePreset(src, loc, name, img, p);

        if (nr.status == bosefix::NormalizeStatus::ABANDONED) {
            JsonObject o = abandoned.add<JsonObject>();
            o["slot"]   = slot;
            o["source"] = src;
            o["reason"] = nr.reason;
            ++countAban;
        } else if (bosefix::PresetStore::instance().set(id, p)) {
            JsonObject o = imported.add<JsonObject>();
            o["slot"]            = p.slot;
            o["name"]            = p.name;
            o["source"]          = bosefix::presetSourceToStr(p.source);
            o["stationId"]       = p.stationId;
            o["streamUrl"]       = p.streamUrl;
            o["normalize"]       = bosefix::normalizeStatusToStr(nr.status);
            if (nr.status == bosefix::NormalizeStatus::OK_CONVERTED) {
                o["converted_from"] = nr.originalSource;
                o["reason"]         = nr.reason;
            }
            ++countOk;
        }
        pos = presetClose;
    }

    resp["ok"]            = true;
    resp["count"]         = countOk;
    resp["abandoned_count"] = countAban;
    String body; serializeJson(resp, body);
    req->send(200, "application/json", body);
}

// POST /api/speaker/{id}/preset/{slot}/push-to-device
//   speichert das aktuelle Preset am echten Speaker (Long-Press-Simulation):
//   /select mit ContentItem → /key press PRESET_n → 2.5s → /key release PRESET_n
void handlePushPresetToDevice(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    uint8_t slot = req->pathArg(1).toInt();
    auto* sp = bosefix::SpeakerInventory::instance().findById(id);
    if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
    auto p = bosefix::PresetStore::instance().get(id, slot);
    if (p.source == bosefix::PresetSource::EMPTY) {
        req->send(400, "application/json", "{\"error\":\"empty slot\"}"); return;
    }
    // Speaker /select mit ContentItem
    HTTPClient http;
    String url = "http://" + sp->ip + ":" + String(BOSE_BMX_PORT) + "/select";
    if (!http.begin(url)) { req->send(500, "application/json", "{\"error\":\"http begin failed\"}"); return; }
    String ci = "<ContentItem source=\"";
    ci += bosefix::presetSourceToStr(p.source);
    ci += "\" type=\"stationurl\" location=\"";
    if (p.source == bosefix::PresetSource::TUNEIN) {
        ci += "/v1/playback/station/" + p.stationId;
    } else {
        ci += p.streamUrl;
    }
    ci += "\" sourceAccount=\"\" isPresetable=\"true\"><itemName>" + p.name + "</itemName></ContentItem>";
    http.addHeader("Content-Type", "text/xml");
    int code = http.POST(ci);
    http.end();
    delay(4000);  // Stream stabilisieren

    // Long-Press PRESET_n
    auto sendKey = [&](const String& state) {
        HTTPClient h;
        String u = "http://" + sp->ip + ":" + String(BOSE_BMX_PORT) + "/key";
        if (!h.begin(u)) return;
        h.addHeader("Content-Type", "text/xml");
        String body = "<key state=\"" + state + "\" sender=\"Gabbo\">PRESET_" + String(p.slot) + "</key>";
        h.POST(body);
        h.end();
    };
    sendKey("press");
    delay(2500);  // > 2s = Long-Press
    sendKey("release");

    JsonDocument doc;
    doc["ok"] = code == 200;
    doc["http_select"] = code;
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// Gruppen
// -----------------------------------------------------------------------------
void handlePutGroup(AsyncWebServerRequest* req, JsonDocument& body) {
    String id = req->pathArg(0);
    String groupId = (const char*)(body["group_id"] | "");
    auto* sp = bosefix::SpeakerInventory::instance().findById(id);
    if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown\"}"); return; }
    sp->groupId = groupId;
    bosefix::SpeakerInventory::instance().saveToNVS();
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
        for (auto& s : bosefix::SpeakerInventory::instance().list()) {
            if (s.groupId == gid && s.deviceId != src) targets.push_back(s.deviceId);
        }
    }
    int n = bosefix::PresetStore::instance().syncToGroup(src, targets);
    JsonDocument doc; doc["ok"] = true; doc["synced_to"] = n;
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

// -----------------------------------------------------------------------------
// TuneIn — Resolve fuer UI-Preview
// -----------------------------------------------------------------------------
void handleTuneInResolve(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    auto r = bosefix::resolveTuneInStruct(id);
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
    http.setConnectTimeout(3000); http.setTimeout(5000);
    String url = "http://opml.radiotime.com/Search.ashx?query=" + q + "&render=json";
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
    bosefix::factoryResetWifi();
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

void handleRoot(AsyncWebServerRequest* req) {
    // Wenn LittleFS Web-UI hat -> liefere index.html, sonst Mini-Page.
    if (LittleFS.exists("/index.html")) {
        req->send(LittleFS, "/index.html", "text/html");
        return;
    }
    String ip = WiFi.localIP().toString();
    String html =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<title>BoseFix32 " FW_VERSION_STRING "</title>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:-apple-system,Segoe UI,sans-serif;max-width:40em;margin:3em auto;padding:0 1em}"
        "code{background:#fee;padding:.1em .3em;border-radius:3px}</style></head><body>"
        "<h1>BoseFix32</h1><p>Web UI not flashed. Use the JSON API directly:</p>"
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
        " &middot; <a href=\"https://github.com/tostmann/BoseFix32\">github.com/tostmann/BoseFix32</a></p>"
        "</body></html>";
    req->send(200, "text/html; charset=utf-8", html);
}

} // anon

// -----------------------------------------------------------------------------
// IP-Failsafe Test-Endpoint
//   Setzt NVS `bosefix-net.last_ip` auf einen anderen Wert, sodass beim
//   naechsten Boot `ipFailsafeCheck()` einen IP-Wechsel erkennt und alle
//   owned-Speaker per Telnet re-migriert. Reine Test-Hilfe, kein Cleanup
//   noetig — der Failsafe persistiert beim Lauf die aktuelle IP wieder.
// -----------------------------------------------------------------------------
void handleTestForceIpChange(AsyncWebServerRequest* req, JsonDocument& body) {
    String fakeIp = (const char*)(body["fake_ip"] | "10.10.99.99");
    Preferences p;
    if (!p.begin("bosefix-net", false)) {
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
    auto cfg = bosefix::loadAutoModeConfig();
    auto st  = bosefix::getAutoModeStatus();
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
    auto cfg = bosefix::loadAutoModeConfig();
    if (body["enabled"].is<bool>())            cfg.enabled       = body["enabled"].as<bool>();
    if (body["dry_run"].is<bool>())            cfg.dryRun        = body["dry_run"].as<bool>();
    if (body["boot_delay_ms"].is<uint32_t>())  cfg.bootDelayMs   = body["boot_delay_ms"].as<uint32_t>();
    if (body["max_per_boot"].is<uint32_t>())   cfg.maxPerBoot    = body["max_per_boot"].as<uint32_t>();
    if (body["cron_interval_s"].is<uint32_t>())cfg.cronIntervalS = body["cron_interval_s"].as<uint32_t>();
    bosefix::saveAutoModeConfig(cfg);
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

    routeJsonBody(ui, "^/api/speaker/([^/]+)/group$", HTTP_PUT, handlePutGroup);
    routeJsonBody(ui, "/api/group/sync",              HTTP_POST, handleSyncGroup);

    ui.on("^/api/tunein/resolve/([^/]+)$", HTTP_GET, handleTuneInResolve);
    ui.on("/api/tunein/search",            HTTP_GET, handleTuneInSearch);

    ui.on("/api/auto-mode",          HTTP_GET,  handleGetAutoMode);
    routeJsonBody(ui, "/api/auto-mode", HTTP_PUT, handlePutAutoMode);
    routeJsonBody(ui, "/api/test/force-ip-change", HTTP_POST, handleTestForceIpChange);

    ui.on("/api/factory_reset_wifi", HTTP_POST, handleFactoryResetWifi);
    ui.on("/api/reboot",             HTTP_POST, handleSelfReboot);

    // OTA — multipart upload. Mit ASYNCWEBSERVER_REGEX=1 binden plain
    // Path-Strings ohne `^...$` als Prefix — /api/ota wuerde dann auch
    // /api/ota/fs schlucken. Mit explizitem Anchor matched jede Route
    // exakt ihren Pfad.
    ui.on("^/api/ota$",    HTTP_POST, handleOtaFinalize,   handleOtaUpload);
    ui.on("^/api/ota/fs$", HTTP_POST, handleOtaFsFinalize, handleOtaFsUpload);
}
