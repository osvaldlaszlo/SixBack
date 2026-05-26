// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Diagnostic Snapshot Implementation

#include "speaker_diagnostic.h"
#include "config.h"
#include "speaker_inventory.h"
#include "speaker_telnet.h"
#include "version.h"
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace sixback {

namespace {

constexpr const char* SNAPSHOT_DIR  = "/snapshots";
constexpr size_t      MAX_BODY_BYTES = 8192;

struct Endpoint { const char* key; const char* path; };

// Reihenfolge bewusst: /info zuerst, weil das der einzige Pflicht-Endpoint ist.
// Wenn der schon fehlschlaegt, ist der Speaker offline und der Rest macht keinen
// Sinn. /presets + /sources sind die fuer Source-Type-Reverse-Engineering
// wichtigsten Felder.
const Endpoint ENDPOINTS[] = {
    { "info",              "/info"              },
    { "presets",           "/presets"           },
    { "sources",           "/sources"           },
    { "now_playing",       "/now_playing"       },
    { "getZone",           "/getZone"           },
    { "getGroup",          "/getGroup"          },
    { "listMediaServers",  "/listMediaServers"  },
    { "sourceServiceList", "/sourceServiceList" },
};

bool fetchOne_(const String& ip, const char* path, int& httpCodeOut, String& bodyOut) {
    HTTPClient http;
    http.setReuse(false);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + path;
    http.setConnectTimeout(2000);
    http.setTimeout(3000);
    if (!http.begin(url)) {
        httpCodeOut = 0;
        return false;
    }
    int code = http.GET();
    httpCodeOut = code;
    if (code != 200) {
        http.end();
        return false;
    }
    bodyOut = http.getString();
    http.end();
    if (bodyOut.length() > MAX_BODY_BYTES) {
        bodyOut.remove(MAX_BODY_BYTES);
        bodyOut += "\n<!-- truncated by SixBack diagnostic capture -->";
    }
    return true;
}

void ensureSnapshotDir_() {
    if (!LittleFS.exists(SNAPSHOT_DIR)) {
        LittleFS.mkdir(SNAPSHOT_DIR);
    }
}

String snapshotPath_(const String& deviceId) {
    String p = SNAPSHOT_DIR;
    p += "/";
    p += deviceId;
    p += ".json";
    return p;
}

// Rotation: vor dem Schreiben rotieren wir vorhandene Snapshots
//   /snapshots/<dev>.json      -> /snapshots/<dev>.prev.json
//   /snapshots/<dev>.prev.json -> /snapshots/<dev>.prev2.json
// So bleiben die letzten 3 Stände erhalten. Schutz gegen den Fall dass
// die User-Presets sich nach einer Migration aendern und der allererste
// Snapshot vom leeren-Zustand stammt — dann liegen mindestens 2 aeltere
// Versionen daneben.
String snapshotPathRot_(const String& deviceId, int gen) {
    String p = SNAPSHOT_DIR;
    p += "/";
    p += deviceId;
    if (gen == 1) p += ".prev";
    else if (gen == 2) p += ".prev2";
    p += ".json";
    return p;
}

void rotateSnapshots_(const String& deviceId) {
    String p0 = snapshotPath_(deviceId);
    String p1 = snapshotPathRot_(deviceId, 1);
    String p2 = snapshotPathRot_(deviceId, 2);
    if (LittleFS.exists(p2)) LittleFS.remove(p2);
    if (LittleFS.exists(p1)) LittleFS.rename(p1, p2);
    if (LittleFS.exists(p0)) LittleFS.rename(p0, p1);
}

// Push Snapshot zum Maintainer-Receiver — sixback.io/snapshots/bosefix/snapshot
// (Apache-Reverse-Proxy via WireGuard zu 10.10.11.113:8788, dort FastAPI
// mit /bosefix/snapshot-Route).
// Schweigend bei Fehler: Push ist best-effort, das primaere Backup liegt
// schon in LittleFS. Setzt User-Agent damit der Receiver weiss dass es
// ein automatischer Upload ist.
bool uploadSnapshotToMaintainer_(const String& jsonBody) {
    if (jsonBody.length() == 0) return false;
    WiFiClientSecure client;
    client.setInsecure();  // sixback.io mit Let's-Encrypt; Skip-Verify auf ESP
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(4000);
    http.setTimeout(8000);
    const char* url = "https://sixback.io/snapshots/bosefix/snapshot";
    if (!http.begin(client, url)) return false;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "SixBack/auto-pre-migrate");
    int code = http.POST((uint8_t*)jsonBody.c_str(), jsonBody.length());
    bool ok = (code == 200 || code == 201);
    Serial.printf("[diag] upload to maintainer: HTTP %d (%u bytes), ok=%d\n",
                  code, (unsigned)jsonBody.length(), (int)ok);
    http.end();
    return ok;
}

} // namespace

bool captureLiveSnapshot(const String& deviceId, JsonDocument& out) {
    auto& inv = SpeakerInventory::instance();
    String ip, name, model, firmware, statusStr;
    bool ownedByUs = false;
    String cloudUrl;
    {
        SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(deviceId);
        if (!sp) return false;
        ip        = sp->ip;
        name      = sp->name;
        model     = sp->model;
        firmware  = sp->firmware;
        statusStr = migrationStatusToStr(sp->status);
        ownedByUs = sp->ownedByUs;
        cloudUrl  = sp->cloudUrl;
    }
    if (ip.length() == 0) return false;

    out["sixback_version"]  = FW_VERSION_STRING;
    out["sixback_build"]    = FW_BUILD_DATE;
    out["captured_at_ms"]   = (uint32_t)millis();
    out["esp_base_url"]     = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);

    JsonObject sp = out["speaker"].to<JsonObject>();
    sp["device_id"]         = deviceId;
    sp["ip"]                = ip;
    sp["name"]              = name;
    sp["model"]             = model;
    sp["firmware"]          = firmware;
    sp["status_at_capture"] = statusStr;
    sp["owned_by_us"]       = ownedByUs;
    sp["cloud_url"]         = cloudUrl;

    JsonObject bmx = out["bmx"].to<JsonObject>();
    bool infoOk = false;
    for (auto& e : ENDPOINTS) {
        int code = 0; String body;
        bool ok = fetchOne_(ip, e.path, code, body);
        JsonObject entry = bmx[e.key].to<JsonObject>();
        entry["http"] = code;
        entry["ok"]   = ok;
        entry["body"] = body;
        if (strcmp(e.key, "info") == 0 && ok) infoOk = true;
        delay(50);
    }

    JsonObject telnet = out["telnet"].to<JsonObject>();
    JsonObject tcfg   = telnet["getpdo_currentsystemconfiguration"].to<JsonObject>();
    String cfg;
    bool tok = captureSysConfigurationList(ip, cfg);
    tcfg["ok"]   = tok;
    tcfg["body"] = cfg;

    Serial.printf("[diag] snapshot %s ip=%s info=%d telnet=%d size~%u\n",
                  deviceId.c_str(), ip.c_str(), (int)infoOk, (int)tok,
                  (unsigned)out.memoryUsage());
    return infoOk;
}

void persistPreMigrateSnapshot(const String& deviceId, bool force) {
    if (deviceId.length() == 0) return;
    ensureSnapshotDir_();
    String path = snapshotPath_(deviceId);

    // 2026-05-21: jeder Migrate-Pfad zieht einen frischen Snapshot —
    // bestehende werden NICHT mehr ueberschrieben sondern rotiert
    // (.prev / .prev2). Damit bleibt der erste Original-Stand erhalten
    // auch wenn spaetere Migrations laufen, UND der aktuellste Stand wird
    // mit jedem Migrate aufgefrischt. Force-Param bedeutet jetzt nur
    // noch "auch wenn capture leer waere".
    rotateSnapshots_(deviceId);

    JsonDocument doc;
    if (!captureLiveSnapshot(deviceId, doc)) {
        Serial.printf("[diag] capture failed for %s — no snapshot written\n",
                      deviceId.c_str());
        return;
    }
    doc["snapshot_kind"] = force ? "manual" : "pre_migrate";

    // Serialisieren in einen String — wir brauchen ihn sowohl fuer
    // LittleFS als auch fuer den optionalen Maintainer-Upload.
    String jsonBody;
    serializeJson(doc, jsonBody);

    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf("[diag] open(%s, w) failed\n", path.c_str());
    } else {
        size_t n = f.print(jsonBody);
        f.close();
        Serial.printf("[diag] persisted %s (%u bytes, rotated prev gens)\n",
                      path.c_str(), (unsigned)n);
    }

    // Auto-Upload zum Maintainer-Receiver — best-effort. Schutz gegen den
    // Fall "User reflasht ESP, LittleFS leer, Original-Presets weg".
    // sixback.io/snapshots/bosefix/snapshot via Apache-Proxy zu Pi5:8788.
    uploadSnapshotToMaintainer_(jsonBody);
}

bool loadStoredSnapshot(const String& deviceId, String& outJson) {
    String path = snapshotPath_(deviceId);
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    outJson = f.readString();
    f.close();
    return outJson.length() > 0;
}

bool hasStoredSnapshot(const String& deviceId) {
    return LittleFS.exists(snapshotPath_(deviceId));
}

} // namespace sixback
