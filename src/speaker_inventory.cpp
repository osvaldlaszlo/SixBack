// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "speaker_inventory.h"
#include "nvs_helper.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <esp_log.h>

namespace sixback {

namespace {

constexpr const char* NVS_NS  = "sixback-inv";
constexpr const char* NVS_KEY = "speakers";

// Hilfs-Regex-frei: einfaches String-extract zwischen <tag> und </tag>
String xmlValue(const String& xml, const String& tag) {
    String open = "<" + tag + ">";
    String close = "</" + tag + ">";
    int a = xml.indexOf(open);
    if (a < 0) return "";
    a += open.length();
    int b = xml.indexOf(close, a);
    if (b < 0) return "";
    return xml.substring(a, b);
}

// Extrahiert Attribut-Wert aus z.B. <interface ssid="X" ...
String xmlAttr(const String& xml, const String& attr) {
    String key = attr + "=\"";
    int a = xml.indexOf(key);
    if (a < 0) return "";
    a += key.length();
    int b = xml.indexOf('"', a);
    if (b < 0) return "";
    return xml.substring(a, b);
}

// Mini-Telnet: schickt cmd, wartet auf "->" Prompt, liefert Reply zurueck.
bool telnetSend(WiFiClient& c, const String& cmd, String& reply, uint32_t timeoutMs = 3000) {
    c.print(cmd + "\n");
    reply = "";
    uint32_t deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        while (c.available()) {
            char ch = c.read();
            reply += ch;
            if (reply.endsWith("->")) return true;
        }
        delay(15);
    }
    return false;
}

} // anon

SpeakerInventory& SpeakerInventory::instance() {
    static SpeakerInventory s;
    return s;
}

void SpeakerInventory::initMutex_() {
    if (!mx_) mx_ = xSemaphoreCreateRecursiveMutex();
}

SpeakerInventory::LockGuard::LockGuard(SpeakerInventory& inv) : inv_(inv) {
    inv_.initMutex_();
    xSemaphoreTakeRecursive(inv_.mx_, portMAX_DELAY);
}

SpeakerInventory::LockGuard::~LockGuard() {
    xSemaphoreGiveRecursive(inv_.mx_);
}

const char* migrationStatusToStr(MigrationStatus s) {
    switch (s) {
        case MigrationStatus::NOT_MIGRATED:   return "not_migrated";
        case MigrationStatus::MIGRATED:       return "migrated";
        case MigrationStatus::OFFLINE:        return "offline";
        case MigrationStatus::SETTLING:       return "settling";
        default:                              return "unknown";
    }
}

const char* probeFailReasonStr(ProbeFailReason r) {
    switch (r) {
        case ProbeFailReason::OK:             return "ok";
        case ProbeFailReason::HTTP_BEGIN:     return "http_begin_failed";
        case ProbeFailReason::CONNECT_FAILED: return "connect_or_read_failed";
        case ProbeFailReason::HTTP_NOT_200:   return "http_not_200";
        case ProbeFailReason::EMPTY_BODY:     return "empty_body";
        case ProbeFailReason::WRONG_BODY:     return "wrong_body";
        case ProbeFailReason::NO_DEVICE_ID:   return "no_device_id";
        default:                              return "unknown";
    }
}

void SpeakerInventory::loadFromNVS() {
    LockGuard g(*this);
    JsonDocument doc;
    if (!nvsLoadJson(NVS_NS, NVS_KEY, doc)) {
        Serial.println("[inv] no NVS-state, starting fresh");
        return;
    }
    speakers_.clear();
    for (JsonObject o : doc["speakers"].as<JsonArray>()) {
        Speaker s;
        s.deviceId   = (const char*)o["deviceId"];
        s.name       = (const char*)o["name"];
        s.model      = (const char*)o["model"];
        s.firmware   = (const char*)o["firmware"];
        s.ip         = (const char*)o["ip"];
        s.accountId  = (const char*)o["accountId"];
        s.status     = (MigrationStatus)(uint8_t)o["status"];
        s.cloudUrl   = (const char*)(o["cloudUrl"] | "");
        s.ownedByUs  = o["ownedByUs"] | false;
        s.lastSeenMs = 0;  // resetten nach reboot
        s.groupId    = (const char*)(o["groupId"] | "");
        if (o["mediaServerUuids"].is<JsonArray>()) {
            for (JsonVariant v : o["mediaServerUuids"].as<JsonArray>()) {
                s.mediaServerUuids.emplace_back((const char*)v);
            }
        }
        if (o["spotifyAccounts"].is<JsonArray>()) {
            for (JsonObject sp : o["spotifyAccounts"].as<JsonArray>()) {
                Speaker::SpotifyAccount sa;
                sa.sourceAccount = (const char*)(sp["sourceAccount"] | "");
                sa.displayName   = (const char*)(sp["displayName"]   | "");
                if (sa.sourceAccount.length() > 0) s.spotifyAccounts.push_back(sa);
            }
        }
        speakers_.push_back(s);
    }
    Serial.printf("[inv] loaded %u speakers from NVS\n",
                  (unsigned)speakers_.size());
}

void SpeakerInventory::saveToNVS() {
    LockGuard g(*this);
    JsonDocument doc;
    JsonArray arr = doc["speakers"].to<JsonArray>();
    for (auto& s : speakers_) {
        JsonObject o = arr.add<JsonObject>();
        o["deviceId"]  = s.deviceId;
        o["name"]      = s.name;
        o["model"]     = s.model;
        o["firmware"]  = s.firmware;
        o["ip"]        = s.ip;
        o["accountId"] = s.accountId;
        o["status"]    = (uint8_t)s.status;
        o["cloudUrl"]  = s.cloudUrl;
        o["ownedByUs"] = s.ownedByUs;
        o["groupId"]   = s.groupId;
        if (!s.mediaServerUuids.empty()) {
            JsonArray msa = o["mediaServerUuids"].to<JsonArray>();
            for (const auto& uuid : s.mediaServerUuids) msa.add(uuid);
        }
        if (!s.spotifyAccounts.empty()) {
            JsonArray spa = o["spotifyAccounts"].to<JsonArray>();
            for (const auto& sa : s.spotifyAccounts) {
                JsonObject sp = spa.add<JsonObject>();
                sp["sourceAccount"] = sa.sourceAccount;
                sp["displayName"]   = sa.displayName;
            }
        }
    }
    nvsSaveJson(NVS_NS, NVS_KEY, doc);
}

void SpeakerInventory::mergeSpeaker_(const Speaker& s) {
    LockGuard g(*this);
    for (auto& existing : speakers_) {
        if (existing.deviceId == s.deviceId) {
            existing.name       = s.name;
            existing.model      = s.model;
            existing.firmware   = s.firmware;
            existing.ip         = s.ip;
            existing.accountId  = s.accountId;
            existing.lastSeenMs = millis();
            // status NICHT ueberschreiben - das macht refreshMigrationStatus
            return;
        }
    }
    Speaker s2 = s;
    s2.lastSeenMs = millis();
    speakers_.push_back(s2);
}

bool SpeakerInventory::probeIp_(const String& ip, Speaker& out,
                                 uint16_t connectMs, uint16_t readMs,
                                 ProbeFailure* failOut) {
    auto setFail = [&](ProbeFailReason r, const String& d) {
        if (failOut) { failOut->reason = r; failOut->detail = d; }
    };
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(connectMs);
    http.setTimeout(readMs);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/info";
    if (!http.begin(url)) {
        Serial.printf("[probe] %s http.begin() failed (URL invalid?)\n", ip.c_str());
        setFail(ProbeFailReason::HTTP_BEGIN, "http.begin() returned false");
        return false;
    }
    uint32_t t0 = millis();
    int code = http.GET();
    uint32_t dt = millis() - t0;
    if (code <= 0) {
        // Negative codes = HTTPClient internal errors (-1 connect, -11 read-timeout, etc.)
        Serial.printf("[probe] %s connect/read failed (%d) after %u ms — timeout=%u/%u\n",
                      ip.c_str(), code, dt, connectMs, readMs);
        http.end();
        setFail(ProbeFailReason::CONNECT_FAILED,
                String("HTTP client error code=") + String(code) +
                ", elapsed=" + String(dt) + "ms, connectTimeout=" +
                String(connectMs) + "ms readTimeout=" + String(readMs) + "ms");
        return false;
    }
    if (code != 200) {
        Serial.printf("[probe] %s HTTP %d (expected 200) after %u ms\n",
                      ip.c_str(), code, dt);
        http.end();
        setFail(ProbeFailReason::HTTP_NOT_200,
                String("HTTP ") + String(code) + " (expected 200)");
        return false;
    }
    String xml = http.getString();
    http.end();
    if (xml.length() == 0) {
        Serial.printf("[probe] %s empty body after HTTP 200\n", ip.c_str());
        setFail(ProbeFailReason::EMPTY_BODY, "HTTP 200 but empty body");
        return false;
    }
    if (xml.indexOf("<info") < 0) {
        Serial.printf("[probe] %s body has no <info ...> tag (len=%u, first=%.60s)\n",
                      ip.c_str(), (unsigned)xml.length(),
                      xml.c_str() ? xml.c_str() : "");
        setFail(ProbeFailReason::WRONG_BODY,
                String("HTTP 200 but body lacks <info ...> tag (len=") +
                String(xml.length()) + ")");
        return false;
    }
    out.deviceId  = xmlAttr(xml, "deviceID");
    out.name      = xmlValue(xml, "name");
    out.model     = xmlValue(xml, "type");
    out.firmware  = xmlValue(xml, "softwareVersion");
    out.accountId = xmlValue(xml, "margeAccountUUID");
    out.ip        = ip;
    out.status    = MigrationStatus::UNKNOWN;
    if (out.deviceId.length() == 0) {
        Serial.printf("[probe] %s <info> present but deviceID attribute empty\n", ip.c_str());
        setFail(ProbeFailReason::NO_DEVICE_ID,
                "<info> tag present but deviceID attribute empty");
        return false;
    }
    Serial.printf("[probe] %s OK after %u ms — %s (%s)\n",
                  ip.c_str(), dt, out.name.c_str(), out.model.c_str());
    if (failOut) failOut->reason = ProbeFailReason::OK;
    return true;
}

void SpeakerInventory::ssdpMSearch_() {
    WiFiUDP udp;
    if (!udp.begin(0)) return;
    const char* msg =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";
    IPAddress mcast(239, 255, 255, 250);
    for (int i = 0; i < 3; ++i) {
        udp.beginPacket(mcast, 1900);
        udp.write((const uint8_t*)msg, strlen(msg));
        udp.endPacket();
        delay(150);
    }
    Serial.println("[inv][ssdp] M-SEARCH burst sent, listening 4s ...");
    uint32_t deadline = millis() + 4000;
    char buf[1024];
    while (millis() < deadline) {
        int n = udp.parsePacket();
        if (n <= 0) { delay(20); continue; }
        IPAddress src = udp.remoteIP();
        int len = udp.read(buf, sizeof(buf) - 1);
        if (len <= 0) continue;
        buf[len] = 0;
        String resp(buf);
        // Pruefen ob es ein SoundTouch ist (Server-Header oder Location-Pattern)
        if (resp.indexOf("Location: http://") < 0) continue;
        // Bose-Speaker liefern z.B. http://10.10.11.196:8091/XD/BO5EBO5E-...
        String srcIp = src.toString();
        Speaker s;
        if (probeIp_(srcIp, s)) {
            Serial.printf("[inv][ssdp] %s -> %s (%s)\n",
                          srcIp.c_str(), s.name.c_str(), s.model.c_str());
            mergeSpeaker_(s);
        }
    }
    udp.stop();
}

// Probe-Pass ueber alle in NVS hinterlegten Speaker-IPs. Viel schneller +
// zuverlaessiger als SSDP/Active-Scan, weil wir genau die Adressen kennen
// die normalerweise antworten. Faengt Speaker auf, deren SSDP-M-SEARCH-
// Response von der WiFi-Multicast-Konvertierung verschluckt wurde — was
// auf ESP32-C6 + WiFi-6-Routern haeufiger passiert als auf S3.
void SpeakerInventory::knownIpProbe_() {
    std::vector<Speaker> snapshot;
    { LockGuard g(*this); snapshot = speakers_; }  // Kopie unter Lock
    if (snapshot.empty()) return;
    Serial.printf("[inv][known] probing %u known IPs ...\n",
                  (unsigned)snapshot.size());
    int hits = 0;
    for (auto& s : snapshot) {
        if (s.ip.length() == 0) continue;
        Speaker probe;
        if (probeIp_(s.ip, probe)) {
            Serial.printf("[inv][known] %s -> %s (%s) still alive\n",
                          s.ip.c_str(), probe.name.c_str(), probe.model.c_str());
            mergeSpeaker_(probe);
            ++hits;
        }
    }
    Serial.printf("[inv][known] %d/%u known IPs responded\n",
                  hits, (unsigned)snapshot.size());
}

void SpeakerInventory::activeScan_() {
    // Subnetz aus eigener IP ableiten: nur fuer /24-Netze, andere skippen
    IPAddress my = WiFi.localIP();
    IPAddress mask = WiFi.subnetMask();
    if (mask[0] != 255 || mask[1] != 255 || mask[2] != 255) {
        Serial.println("[inv][scan] non-/24 mask, skipping active scan");
        return;
    }
    // IPs, die bereits aus knownIpProbe_/SSDP im Inventory sind, koennen
    // wir hier ueberspringen — spart ~3 s pro bereits gefundenem Speaker.
    bool already[256] = {};
    already[my[3]] = true;       // selbst
    {
        LockGuard g(*this);
        for (auto& s : speakers_) {
            IPAddress addr;
            if (addr.fromString(s.ip) && addr[0] == my[0] &&
                addr[1] == my[1] && addr[2] == my[2]) {
                already[addr[3]] = true;
            }
        }
    }
    int toScan = 254 - (int)std::count(already, already + 256, true);
    Serial.printf("[inv][scan] active scan %d.%d.%d.1-254 (skipping %d known, %d to probe) ...\n",
                  my[0], my[1], my[2],
                  (int)std::count(already, already + 256, true) - 1,
                  toScan);

    // Network/HTTPClient-Log-Spam waehrend des /24-Scans unterdruecken:
    // jeder Nicht-Bose-Host loggt sonst [E] socket-reset + [W] connection-
    // refused + [I] timeout — ca. 1000 Zeilen pro Scan. Wir wollen nur
    // unsere eigene Progress-Anzeige sehen. NACH dem Scan sofort zurueck-
    // setzen, damit echte HTTP-Fehler (z.B. push-to-device) wieder sichtbar
    // werden.
    esp_log_level_t prev_net  = esp_log_level_get("NetworkClient");
    esp_log_level_t prev_http = esp_log_level_get("HTTPClient");
    esp_log_level_set("NetworkClient", ESP_LOG_NONE);
    esp_log_level_set("HTTPClient",    ESP_LOG_NONE);

    int found     = 0;
    int probed    = 0;
    uint32_t lastDot = millis();
    Serial.print("[inv][scan] progress: ");
    for (int i = 1; i <= 254; ++i) {
        if (already[i]) continue;
        char ip[20];
        snprintf(ip, sizeof(ip), "%d.%d.%d.%d", my[0], my[1], my[2], i);
        Speaker s;
        // Moderate Timeouts fuer noch laggy WiFi-Verbindungen (C6/C3 mit
        // WiFi 6 koennen in der Anfangsphase nach Boot 500 ms+ brauchen
        // bis SYN-ACK). 300 ms connect / 1200 ms read ergibt im worst
        // case 254 × 300 ≈ 76 s im Hintergrund-Task.
        bool hit = probeIp_(ip, s, 300, 1200);
        ++probed;
        if (hit) {
            ++found;
            // Newline vor Speaker-Treffer damit Progress-Punkte sauber bleiben:
            Serial.printf("\n[inv][scan] %d/%d HIT %s -> %s (%s)\n",
                          probed, toScan, ip, s.name.c_str(), s.model.c_str());
            Serial.print("[inv][scan] progress: ");
            mergeSpeaker_(s);
        } else if (millis() - lastDot > 2000) {
            // alle 2 s einen Progress-Punkt setzen, damit man sieht
            // dass der Scan lebt (nicht haengt). Counter alle 10 Probes:
            if (probed % 10 == 0) {
                Serial.printf("%d", probed);
            } else {
                Serial.print(".");
            }
            lastDot = millis();
        }
        // kleiner yield damit AsyncTCP nicht ausgehungert wird
        if ((i & 0xF) == 0) delay(1);
    }
    Serial.printf("\n[inv][scan] active scan done, %d/%d probed, %d new speakers found\n",
                  probed, toScan, found);

    // Log-Level zurueck — damit "echte" HTTP-Errors aus push-to-device,
    // import-from-device, migrate-telnet etc. wieder im Serial-Log auftauchen.
    esp_log_level_set("NetworkClient", prev_net);
    esp_log_level_set("HTTPClient",    prev_http);
}

void SpeakerInventory::activeScanTask_(void* arg) {
    auto* inv = static_cast<SpeakerInventory*>(arg);
    inv->activeScan_();
    inv->refreshMigrationStatus();  // gleich am Ende auch Status aktualisieren
    inv->saveToNVS();
    inv->scanRunning_.store(false);
    Serial.println("[inv] background scan task finished");
    vTaskDelete(nullptr);
}

MigrationStatus SpeakerInventory::detectStatus(const String& ip, const String& myBaseUrl,
                                                String* outCloudUrl) {
    if (outCloudUrl) *outCloudUrl = "";
    WiFiClient c;
    c.setTimeout(3);
    if (!c.connect(ip.c_str(), BOSE_TELNET_PORT, 3000)) {
        // Telnet (Diag-Shell auf Port 17000) ist transient unreachable z.B.
        // waehrend Cloud-Migration-Reboot oder hoher Speaker-Belastung. Vor
        // OFFLINE-Verdikt pruefen ob die BMX-API (Port 8090) noch antwortet —
        // wenn ja, lebt der Speaker, nur die Diag-Shell ist gerade nicht da.
        // → SETTLING statt OFFLINE, damit das UI keinen falschen "Speaker ist
        // aus"-Alarm wirft. Slot/Cloud-URL bleibt unbekannt bis Telnet wieder
        // antwortet.
        HTTPClient http;
        String bmxUrl = "http://" + ip + ":" + String(BOSE_HTTP_PORT) + "/info";
        if (http.begin(bmxUrl)) {
            http.setTimeout(2000);
            int code = http.GET();
            http.end();
            if (code == 200) return MigrationStatus::SETTLING;
        }
        return MigrationStatus::OFFLINE;
    }
    delay(200);
    while (c.available()) c.read();  // banner verwerfen
    String reply;
    if (!telnetSend(c, "getpdo CurrentSystemConfiguration", reply, 5000)) {
        c.stop();
        return MigrationStatus::UNKNOWN;
    }
    c.print("trigger close\n");
    delay(50);
    c.stop();

    // Extrahiere margeServerUrl-Wert aus reply:
    //   margeServerUrl {
    //     text: "https://streaming.bose.com"
    //   }
    int mPos = reply.indexOf("margeServerUrl");
    if (mPos >= 0) {
        int tPos = reply.indexOf("text:", mPos);
        if (tPos >= 0) {
            int q1 = reply.indexOf('"', tPos);
            int q2 = (q1 >= 0) ? reply.indexOf('"', q1 + 1) : -1;
            if (q1 >= 0 && q2 > q1 && outCloudUrl) {
                *outCloudUrl = reply.substring(q1 + 1, q2);
            }
        }
    }
    // Originalzustand = zeigt auf Bose-Cloud-Hostnames
    if (reply.indexOf("streaming.bose.com") >= 0) return MigrationStatus::NOT_MIGRATED;
    // Alles andere mit http:// in margeServerUrl = irgendeine Replacement-Cloud
    // (ggf. WIR mit veralteter IP nach IP-Wechsel, ggf. ein anderes Geraet wie Pi5).
    // Die Unterscheidung "gehoert uns / gehoert wem-anders" macht das UI via
    // ownedByUs-Flag - hier nur der bloecke Telnet-Befund.
    if (outCloudUrl && outCloudUrl->length() > 0 &&
        outCloudUrl->startsWith("http")) return MigrationStatus::MIGRATED;
    return MigrationStatus::UNKNOWN;
}

void SpeakerInventory::refreshMigrationStatus() {
    // Snapshot unter Lock, dann Telnet-Calls OHNE Lock — sonst blockt jeder
    // Reader (AsyncTCP-Handler, Bose-Cloud-Mock) fuer 3-5 s pro Speaker.
    // Ergebnisse danach unter Lock wieder ins inventory mergen.
    std::vector<Speaker> work;
    { LockGuard g(*this); work = speakers_; }
    String myBase = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
    for (auto& s : work) {
        s.status = detectStatus(s.ip, myBase, &s.cloudUrl);
    }
    LockGuard g(*this);
    for (auto& upd : work) {
        // Re-find: deviceId kann sich nicht aendern, slot nach reload aber schon.
        Speaker* live = nullptr;
        for (auto& s : speakers_) {
            if (s.deviceId == upd.deviceId) { live = &s; break; }
        }
        if (!live) continue;
        live->status   = upd.status;
        live->cloudUrl = upd.cloudUrl;
        // Auto-Claim: zeigt eh schon auf UNS, aber ownedByUs noch False
        // (Edge-Case nach Flash-Erase/NVS-Reset) — uebernehmen wir.
        if (!live->ownedByUs && live->cloudUrl == myBase) {
            Serial.printf("[inv] auto-claim %s (cloudUrl matches our base)\n",
                          live->name.c_str());
            live->ownedByUs = true;
            continue;
        }
        // Auto-Release: zeigt auf eine ANDERE Cloud (anderes ESP oder
        // streaming.bose.com nach Revert) — wir sind nicht mehr zustaendig.
        // Verhindert dass ip_failsafe ihn beim naechsten IP-Wechsel
        // zurueck-claimt und dass das UI ihn faelschlich als unsere
        // Verantwortung zeigt. cloudUrl leer = Speaker offline -> kein Change.
        if (live->ownedByUs && live->cloudUrl.length() > 0 && live->cloudUrl != myBase) {
            Serial.printf("[inv] auto-release %s (cloudUrl=%s != our base %s)\n",
                          live->name.c_str(), live->cloudUrl.c_str(), myBase.c_str());
            live->ownedByUs = false;
        }
    }
    saveToNVS();
}

void SpeakerInventory::discoverSync() {
    // Light variant fuer Cron-Ticks: nur Sync-Phasen, kein Background-Scan.
    Serial.println("[inv] discoverSync (light)");
    knownIpProbe_();
    ssdpMSearch_();
    refreshMigrationStatus();
    Serial.printf("[inv] discoverSync done, %u speakers known\n",
                  (unsigned)speakers_.size());
}

void SpeakerInventory::discover() {
    // scanRunning_ ist *uebergreifend* fuer Sync-Phase + Active-Scan-Phase:
    // damit handleDiscover() im HTTP-Handler sofort returnen kann, das UI
    // scan_in_progress sieht und solange pollt bis beide Phasen durch sind.
    bool expected = false;
    if (!scanRunning_.compare_exchange_strong(expected, true)) {
        Serial.println("[inv] discover() already in flight, skipping");
        return;
    }
    Serial.println("[inv] discovery starting (sync phase)");
    // Synchrone Phase, ~5 s: bekannte IPs + SSDP-Burst. Findet zuverlaessig
    // alles was schon mal "gesehen" wurde plus alles was per SSDP-Multicast
    // pingt. Reicht in 99 % der Faelle.
    knownIpProbe_();
    ssdpMSearch_();
    refreshMigrationStatus();
    Serial.printf("[inv] sync phase done, %u speakers known\n",
                  (unsigned)speakers_.size());

    // Active-Scan in Hintergrund-Task — kann je nach LAN-Groesse 30 s+
    // dauern. UI pollt /api/speakers solange isScanRunning() == true.
    // scanRunning_ ist bereits gesetzt, activeScanTask_ released es am Ende.
    BaseType_t r = xTaskCreate(activeScanTask_, "boseScan",
                                6144, this, 1, nullptr);
    if (r != pdPASS) {
        Serial.println("[inv] xTaskCreate failed — running active scan synchronously");
        activeScan_();
        refreshMigrationStatus();
        saveToNVS();
        scanRunning_.store(false);
    }
}

bool SpeakerInventory::addByIp(const String& ip, ProbeFailure* failOut) {
    Speaker s;
    // Manueller Add verdient grosszuegigere Timeouts als SSDP-Background-
    // Probe — User wartet aktiv, Speaker im Standby (SoundTouch Portable)
    // kann ein paar Sekunden brauchen bis er antwortet.
    constexpr uint16_t MANUAL_CONNECT_MS = 2500;
    constexpr uint16_t MANUAL_READ_MS    = 5000;
    if (!probeIp_(ip, s, MANUAL_CONNECT_MS, MANUAL_READ_MS, failOut)) {
        // ARP-Race-Retry (2026-05-21, fix fuer fred_feuerstein P0b-Bug):
        // Direkt nach SixBack-Boot kennt der Speaker den SixBack-MAC
        // noch nicht. Sein ARP-Request kommt zwar bei SixBack an, aber
        // der ARP-Reply kann im FRITZ!Mesh die Topologie nicht ueberwinden
        // (Layer-2-Race / Repeater-Forwarding-Stutter). Symptom: TCP-SYN
        // von SixBack zu Speaker geht durch, Speaker will antworten,
        // aber sein SYN-ACK kommt nicht zurueck weil ARP-Cache fuer
        // SixBack leer + ARP-Reply kommt nicht an. Timeout nach
        // connect-Timeout (~2500ms).
        //
        // Fix: wenn der Probe wegen TIMEOUT (nicht REFUSED) failed,
        // einmal 3s warten und nochmal mit laengeren Timeouts versuchen.
        // Inzwischen hat die Mesh-Layer-2-Topology Zeit sich einzu-
        // pendeln + die ARP-Caches sich gegenseitig zu lernen.
        // Siehe [[reference-bosefix32-p0b-arp-race]] fuer tcpdump-Beweis.
        if (failOut && failOut->reason == ProbeFailReason::CONNECT_FAILED &&
            failOut->detail.indexOf("elapsed=2") >= 0) {
            Serial.printf("[probe] %s ARP-race suspected (timeout @ %ums), "
                          "retry in 3s with longer timeouts\n",
                          ip.c_str(), MANUAL_CONNECT_MS);
            delay(3000);
            if (!probeIp_(ip, s, /*connectMs=*/5000, /*readMs=*/10000, failOut)) {
                return false;
            }
            Serial.printf("[probe] %s recovered after ARP-race retry\n", ip.c_str());
        } else {
            return false;
        }
    }
    String myBase = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
    // detectStatus ist Telnet — bewusst OHNE Lock; danach unter Lock mergen.
    String cloudUrl;
    MigrationStatus st = detectStatus(ip, myBase, &cloudUrl);
    LockGuard g(*this);
    mergeSpeaker_(s);
    if (Speaker* p = findByIp(ip)) {
        p->status   = st;
        p->cloudUrl = cloudUrl;
    }
    saveToNVS();
    return true;
}

// Holt `/listMediaServers` vom Speaker und persistiert die UUIDs in
// Speaker::mediaServerUuids. Schweigend bei Fehler (Speaker offline,
// keine DLNA-Server im LAN, etc.) — die Liste bleibt dann einfach leer.
void SpeakerInventory::refreshMediaServers(const String& deviceId) {
    String ip;
    {
        LockGuard g(*this);
        Speaker* p = findById(deviceId);
        if (!p) return;
        ip = p->ip;
    }
    if (ip.length() == 0) return;

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(1500);
    http.setTimeout(3000);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/listMediaServers";
    if (!http.begin(url)) return;
    int code = http.GET();
    if (code != 200) { http.end(); return; }
    String xml = http.getString();
    http.end();

    // Extrahiere alle id="UUID"-Attribute aus <media_server>-Elementen.
    // listMediaServers liefert UUIDs ohne "uuid:"-Prefix — z.B. id="fa095ecc-...".
    std::vector<String> uuids;
    int pos = 0;
    while (true) {
        int msStart = xml.indexOf("<media_server", pos);
        if (msStart < 0) break;
        int msEnd = xml.indexOf("/>", msStart);
        if (msEnd < 0) msEnd = xml.indexOf(">", msStart);
        if (msEnd < 0) break;
        String chunk = xml.substring(msStart, msEnd);
        int idIdx = chunk.indexOf("id=\"");
        if (idIdx > 0) {
            idIdx += 4;
            int idEnd = chunk.indexOf('"', idIdx);
            if (idEnd > idIdx) {
                String u = chunk.substring(idIdx, idEnd);
                if (u.length() > 0) uuids.push_back(u);
            }
        }
        pos = msEnd + 1;
    }

    LockGuard g(*this);
    if (Speaker* p = findById(deviceId)) {
        p->mediaServerUuids = uuids;
        Serial.printf("[inv][ms] %s -> %u DLNA-server UUIDs cached\n",
                      deviceId.c_str(), (unsigned)uuids.size());
    }
    saveToNVS();
}

// Holt BMX /sources vom Speaker und persistiert alle SPOTIFY-Items mit
// status="READY" in Speaker::spotifyAccounts. Diagnose-Only (Stufe 0) —
// macht linked Spotify-Accounts pro Speaker im UI sichtbar. Schema-Beleg
// (verifiziert aus Pre-Migration-Snapshots fred_feuerstein 2026-05-22):
//   <sourceItem source="SPOTIFY" sourceAccount="<user-id>" status="READY"
//       isLocal="false" multiroomallowed="true">DisplayText</sourceItem>
void SpeakerInventory::refreshSpotifyAccounts(const String& deviceId) {
    String ip;
    {
        LockGuard g(*this);
        Speaker* p = findById(deviceId);
        if (!p) return;
        ip = p->ip;
    }
    if (ip.length() == 0) return;

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(1500);
    http.setTimeout(3000);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/sources";
    if (!http.begin(url)) return;
    int code = http.GET();
    if (code != 200) { http.end(); return; }
    String xml = http.getString();
    http.end();

    // SPOTIFY READY-Items extrahieren. Mehrere Accounts pro Speaker moeglich
    // (fred-Snapshot 2026-05-22 hatte 2 verschiedene Spotify-User auf
    // demselben Speaker). Display-Text steht zwischen Tag-Ende und naechstem
    // <sourceItem> oder /-Selbst-Tag — bei status="READY" hat das Item den
    // Display-Namen (typisch eine Mail-Adresse) als XML-Textinhalt.
    std::vector<Speaker::SpotifyAccount> accs;
    int pos = 0;
    while (true) {
        int siStart = xml.indexOf("<sourceItem ", pos);
        if (siStart < 0) break;
        int tagEnd = xml.indexOf(">", siStart);
        if (tagEnd < 0) break;
        String tag = xml.substring(siStart, tagEnd);  // ohne ">"
        pos = tagEnd + 1;
        bool selfClose = (tag.length() > 0 && tag.endsWith("/"));
        // Nur SPOTIFY + READY weiter betrachten — UNAVAILABLE-Defaults
        // (SpotifyConnectUserName / SpotifyAlexaUserName) skippen.
        if (tag.indexOf("source=\"SPOTIFY\"") < 0) continue;
        if (tag.indexOf("status=\"READY\"")   < 0) continue;
        int accIdx = tag.indexOf("sourceAccount=\"");
        if (accIdx < 0) continue;
        int accStart = accIdx + 15;
        int accEnd = tag.indexOf("\"", accStart);
        if (accEnd <= accStart) continue;
        Speaker::SpotifyAccount sa;
        sa.sourceAccount = tag.substring(accStart, accEnd);
        // DisplayName: Textinhalt bis </sourceItem>, nur wenn nicht selfclose.
        if (!selfClose) {
            int closeIdx = xml.indexOf("</sourceItem>", pos);
            if (closeIdx > pos) {
                sa.displayName = xml.substring(pos, closeIdx);
                sa.displayName.trim();
                pos = closeIdx + 13;
            }
        }
        if (sa.sourceAccount.length() > 0) accs.push_back(sa);
    }

    LockGuard g(*this);
    if (Speaker* p = findById(deviceId)) {
        p->spotifyAccounts = accs;
        Serial.printf("[inv][spot] %s -> %u Spotify accounts (READY)\n",
                      deviceId.c_str(), (unsigned)accs.size());
        for (const auto& a : accs) {
            Serial.printf("[inv][spot]   account=%s display=%s\n",
                          a.sourceAccount.c_str(), a.displayName.c_str());
        }
    }
    saveToNVS();
}

bool SpeakerInventory::remove(const String& deviceId) {
    LockGuard g(*this);
    for (auto it = speakers_.begin(); it != speakers_.end(); ++it) {
        if (it->deviceId == deviceId) {
            speakers_.erase(it);
            saveToNVS();
            return true;
        }
    }
    return false;
}

// HINWEIS: Caller muss SpeakerInventory::LockGuard halten waehrend der
// zurueckgegebene Pointer benutzt wird — sonst race mit mergeSpeaker_/erase.
Speaker* SpeakerInventory::findById(const String& deviceId) {
    for (auto& s : speakers_) {
        if (s.deviceId == deviceId) return &s;
    }
    return nullptr;
}

Speaker* SpeakerInventory::findByIp(const String& ip) {
    for (auto& s : speakers_) {
        if (s.ip == ip) return &s;
    }
    return nullptr;
}

std::vector<Speaker> SpeakerInventory::list() {
    LockGuard g(*this);
    return speakers_;
}

} // namespace sixback
