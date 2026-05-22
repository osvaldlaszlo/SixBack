// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ota_pull.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace sixback {
namespace ota {

namespace {

// Manifest-Quelle. Aus dem build_release.sh / install.busware.de-Setup
// bekannt; absichtlich hardcoded statt konfigurierbar, weil das die
// einzige Distributionsquelle ist (vgl. reference_busware_install_host).
constexpr const char* kManifestUrl =
    "https://install.busware.de/sixback/manifest.json";

// Chip-spezifischer Datei-Name fuer firmware.bin oder littlefs.bin.
// build_release.sh erzeugt:
//   sixback-<tgt>-firmware.bin
//   sixback-<tgt>-littlefs.bin
// wobei <tgt> einer der platformio-envs ist (esp32, s3, c3, c6).
const char* chipPrefix_() {
#if   CONFIG_IDF_TARGET_ESP32S3
    return "sixback-s3-";
#elif CONFIG_IDF_TARGET_ESP32C6
    return "sixback-c6-";
#elif CONFIG_IDF_TARGET_ESP32C3
    return "sixback-c3-";
#elif CONFIG_IDF_TARGET_ESP32
    return "sixback-esp32-";
#else
    return nullptr;  // unbekannte Plattform — Install verweigern
#endif
}

Status            g_status;
SemaphoreHandle_t g_mtx = nullptr;

// Vergleicht zwei Versions-Strings im Format "MAJOR.MINOR.BUILD".
// Return: -1 wenn a < b, 0 wenn gleich, +1 wenn a > b, -2 bei Parse-Fehler.
// BUILD ist monotone, MAJOR+MINOR werden vom User explizit gesetzt.
int semverCompare_(const String& a, const String& b) {
    int aMaj=0, aMin=0, aBld=0, bMaj=0, bMin=0, bBld=0;
    int aGot = sscanf(a.c_str(), "%d.%d.%d", &aMaj, &aMin, &aBld);
    int bGot = sscanf(b.c_str(), "%d.%d.%d", &bMaj, &bMin, &bBld);
    if (aGot != 3 || bGot != 3) return -2;
    if (aMaj != bMaj) return aMaj < bMaj ? -1 : 1;
    if (aMin != bMin) return aMin < bMin ? -1 : 1;
    if (aBld != bBld) return aBld < bBld ? -1 : 1;
    return 0;
}

void lock_()   { if (g_mtx) xSemaphoreTake(g_mtx, portMAX_DELAY); }
void unlock_() { if (g_mtx) xSemaphoreGive(g_mtx); }

void setState_(State s) {
    lock_(); g_status.state = s; unlock_();
}

void setError_(const String& msg) {
    lock_();
    g_status.state = State::ERROR_;
    g_status.error = msg;
    unlock_();
    Serial.printf("[ota-pull] ERROR: %s\n", msg.c_str());
}

// Beziehe das Manifest und parse. Auf Erfolg: latest gesetzt + state
// AVAILABLE oder IDLE. Auf Fehler: state ERROR_.
void doCheck_() {
    WiFiClientSecure tls;
    tls.setInsecure();  // install.busware.de hat ein gueltiges Cert, aber
                        // wir wollen keinen Cert-Bundle mit-flashen muessen.
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    if (!http.begin(tls, kManifestUrl)) {
        setError_("http.begin manifest failed");
        return;
    }
    int code = http.GET();
    if (code != 200) {
        String msg = "manifest HTTP "; msg += code;
        http.end();
        setError_(msg);
        return;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    if (err) {
        String msg = "manifest parse: "; msg += err.c_str();
        setError_(msg);
        return;
    }
    String latest = doc["version"] | "";
    if (latest.length() == 0) {
        setError_("manifest has no 'version'");
        return;
    }

    // Semver-Vergleich. AVAILABLE nur wenn latest STRIKT NEUER als current —
    // Downgrades + Gleichstand werden zu IDLE. Bei Parse-Fehler Fallback auf
    // String-Inequality (alte Logik).
    int cmp = semverCompare_(g_status.current, latest);
    State next;
    const char* reason;
    if (cmp == -2) {
        next = (latest == g_status.current) ? State::IDLE : State::AVAILABLE;
        reason = (next == State::AVAILABLE) ? "diff (semver-parse-failed)" : "equal (semver-parse-failed)";
    } else if (cmp == 0) {
        next = State::IDLE;
        reason = "up-to-date";
    } else if (cmp < 0) {
        next = State::AVAILABLE;
        reason = "update available (latest > current)";
    } else {
        next = State::IDLE;
        reason = "current > latest — no downgrade";
    }
    lock_();
    g_status.latest = latest;
    g_status.error  = "";
    g_status.state  = next;
    unlock_();
    Serial.printf("[ota-pull] check: latest=%s current=%s cmp=%d -> %s (%s)\n",
                  latest.c_str(), g_status.current.c_str(), cmp,
                  next == State::AVAILABLE ? "AVAILABLE" : "IDLE",
                  reason);
}

// Pull eine Datei aus install.busware.de + schreibe sie via Update.begin
// in den angegebenen Update-Bereich (U_FLASH = firmware-slot, U_SPIFFS =
// LittleFS-Partition). Setzt unterwegs g_status.progress + g_status.total.
// Return true on success.
bool pullAndFlashOne_(const char* path, int updateType,
                      const char* phaseName, uint8_t phaseIdx) {
    String url = "https://install.busware.de/sixback/";
    url += path;
    Serial.printf("[ota-pull] phase %d/%u start: %s -> %s\n",
                  phaseIdx, (unsigned)g_status.phaseN, url.c_str(),
                  updateType == U_FLASH ? "FLASH" : "SPIFFS");

    lock_();
    g_status.phase    = phaseName;
    g_status.phaseIdx = phaseIdx;
    g_status.progress = 0;
    g_status.total    = 0;
    unlock_();

    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(8000);
    http.setTimeout(15000);
    if (!http.begin(tls, url)) {
        setError_(String(phaseName) + ": http.begin failed");
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        String msg = phaseName; msg += ": HTTP "; msg += code;
        http.end();
        setError_(msg);
        return false;
    }
    int contentLen = http.getSize();
    // 2026-05-22 Pre-Release: empirisch sieht WiFiClientSecure-HTTPClient auf
    // C6 + S3 fuer einzelne .bin-Downloads ein Content-Length kleiner als
    // die Apache-tatsaechlich-Antwort (z.B. C6-firmware.bin: file=1822160 B,
    // ESP sah total=1797920 B = 24 KB short). Loop exit'd bei written>=total,
    // Update.end commit'te truncated Image → boot-image-magic-fail →
    // Rollback → device-brick. UPDATE_SIZE_UNKNOWN umgeht das: Stream bis
    // EOF/disconnect, Update finalisiert was tatsaechlich kam, partition-
    // boundary-check macht Update.cpp selbst.
    int total = UPDATE_SIZE_UNKNOWN;

    lock_();
    g_status.total = (contentLen > 0) ? (uint32_t)contentLen : 0;
    unlock_();
    Serial.printf("[ota-pull] phase %d GET 200, ContentLength=%d "
                  "(using UPDATE_SIZE_UNKNOWN for write)\n",
                  phaseIdx, contentLen);

    // FS muss vor U_SPIFFS-Write unmountet werden, sonst sehen wir die
    // alten Daten vom alten Mount + Heap-Pressure.
    if (updateType == U_SPIFFS) {
        LittleFS.end();
    }
    if (!Update.begin(total, updateType)) {
        setError_(String(phaseName) + ": Update.begin: " + Update.errorString());
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        setError_(String(phaseName) + ": no stream from HTTPClient");
        Update.abort();
        http.end();
        return false;
    }

    const size_t bufSz = 4096;
    uint8_t* buf = (uint8_t*)malloc(bufSz);
    if (!buf) {
        setError_(String(phaseName) + ": malloc buf failed");
        Update.abort();
        http.end();
        return false;
    }

    uint32_t written = 0;
    uint32_t lastReport = millis();
    uint32_t lastDataMs = millis();
    // Read until connection closes OR no new bytes for 5s (idle-timeout).
    // Pre-Release-fix: NICHT auf written>=total brechen — total ist
    // (auf ESP HTTPS) unzuverlaessig.
    while (http.connected()) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (millis() - lastDataMs > 5000) {
                Serial.printf("[ota-pull] phase %d idle >5s — assume EOF at %u bytes\n",
                              phaseIdx, (unsigned)written);
                break;
            }
            delay(5);
            continue;
        }
        int got = stream->readBytes(buf, std::min(avail, bufSz));
        if (got <= 0) break;
        if (Update.write(buf, got) != (size_t)got) {
            free(buf);
            setError_(String(phaseName) + ": Update.write: " + Update.errorString());
            Update.abort();
            http.end();
            return false;
        }
        written += got;
        lastDataMs = millis();
        if (millis() - lastReport > 500) {
            lock_();
            g_status.progress = written;
            unlock_();
            lastReport = millis();
        }
        if ((written & 0x3FFF) == 0) delay(1);
    }
    free(buf);
    http.end();

    // Wenn die HTTP-Antwort eine vernuenftig wirkende Content-Length hatte,
    // muss `written` auch nahe dran sein. Wenn deutlich kleiner (z.B. < 90 %),
    // war der Download offensichtlich abgebrochen → Update.end mit abort
    // statt commit, damit wir NICHT auf eine truncated Image-Partition
    // umschalten (= brick + rollback).
    if (contentLen > 0 && written < (uint32_t)(contentLen * 9 / 10)) {
        Serial.printf("[ota-pull] phase %d ABORT: only %u/%d bytes\n",
                      phaseIdx, (unsigned)written, contentLen);
        Update.abort();
        setError_(String(phaseName) + ": truncated download (" +
                  String(written) + "/" + String(contentLen) + ")");
        return false;
    }

    if (!Update.end(true)) {
        setError_(String(phaseName) + ": Update.end: " + Update.errorString());
        return false;
    }
    lock_();
    g_status.progress = written;
    if (g_status.total == 0) g_status.total = written;
    unlock_();
    Serial.printf("[ota-pull] phase %d DONE, %u bytes written "
                  "(announced %d).\n",
                  phaseIdx, (unsigned)written, contentLen);
    return true;
}

// Background-Task: Online-Install in 2 Phasen (FS + FW).
//   Phase 1: sixback-<chip>-littlefs.bin -> U_SPIFFS (~384 KB)
//   Phase 2: sixback-<chip>-firmware.bin -> U_FLASH  (~1.7 MB)
//   -> ESP.restart()
// Reihenfolge: FS zuerst damit die NEUE UI nach Reboot sofort verfuegbar
// ist. Wenn FW-Flash fail (Update.end-Fehler), bleibt der Speaker auf
// app0 — alte Firmware + neue UI. Funktioniert weiter, war "graceful
// degradation" (die alte FW liefert die UI aus, neue Endpoints fehlen).
void installTask_(void* /*arg*/) {
    const char* prefix = chipPrefix_();
    if (!prefix) {
        setError_("unknown chip family");
        vTaskDelete(nullptr);
        return;
    }

    String fsName = prefix;  fsName += "littlefs.bin";
    String fwName = prefix;  fwName += "firmware.bin";

    if (!pullAndFlashOne_(fsName.c_str(), U_SPIFFS, "fs", 1)) {
        vTaskDelete(nullptr);
        return;
    }
    if (!pullAndFlashOne_(fwName.c_str(), U_FLASH,  "fw", 2)) {
        vTaskDelete(nullptr);
        return;
    }

    lock_();
    g_status.state = State::DONE;
    g_status.phase = "";
    unlock_();
    Serial.println("[ota-pull] both phases DONE. Reboot in 2s.");
    delay(2000);
    ESP.restart();
}

}  // anon

Status getStatus() {
    lock_();
    Status s = g_status;
    unlock_();
    return s;
}

void checkOnline() {
    setState_(State::CHECKING);
    lock_(); g_status.error = ""; unlock_();
    doCheck_();
}

namespace {
bool spawnInstallTask_() {
    // WICHTIG: state SOFORT (synchron, vor xTaskCreate) auf INSTALLING setzen.
    // Sonst hat der HTTP-Handler eine Race: er antwortet mit getStatus()-
    // Snapshot bevor der Task seine erste Anweisung ausgefuehrt hat — state
    // bleibt scheinbar idle/available, UI startet keinen Poll-Loop und
    // blendet den Progress-Bar nicht ein.
    lock_();
    g_status.state    = State::INSTALLING;
    g_status.progress = 0;
    g_status.total    = 0;
    g_status.error    = "";
    unlock_();

    BaseType_t r = xTaskCreate(installTask_, "ota-pull", 8192, nullptr,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (r != pdPASS) {
        setError_("xTaskCreate failed");
        return false;
    }
    return true;
}
} // anon

bool installOnlineAsync() {
    if (g_status.state != State::AVAILABLE) {
        Serial.printf("[ota-pull] installAsync rejected: state=%d\n",
                      (int)g_status.state);
        return false;
    }
    return spawnInstallTask_();
}

bool installOnlineForceAsync() {
    // INSTALLING-Lock pruefen damit wir nicht zwei Pull-Tasks parallel starten
    if (g_status.state == State::INSTALLING) {
        Serial.println("[ota-pull] forceInstall rejected: already installing");
        return false;
    }
    Serial.printf("[ota-pull] FORCE install requested (state was %d)\n",
                  (int)g_status.state);
    return spawnInstallTask_();
}

void init(const String& myVersion) {
    if (!g_mtx) g_mtx = xSemaphoreCreateMutex();
    lock_();
    g_status.current = myVersion;
    g_status.state   = State::IDLE;
    unlock_();
}

}  // namespace ota
}  // namespace sixback
