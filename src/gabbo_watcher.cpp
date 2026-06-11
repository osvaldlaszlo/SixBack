// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gabbo_watcher.h"

#ifdef SIXBACK_GABBO_WATCHER_ENABLED

#include "gabbo_ws.h"
#include "speaker_inventory.h"
#include "preset_store.h"
#include "api_endpoints.h"   // selectStationOnSpeaker
#include "config.h"

#include <Arduino.h>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace sixback {
namespace {

constexpr uint32_t kInitialWaitMs     = 20000;   // nach Boot: WiFi + Inventory warm
constexpr uint32_t kReconcileMs       = 15000;   // Soll/Ist-Abgleich Inventory<->Conns
constexpr uint32_t kReconnectMs       = 10000;   // pro Conn Reconnect-Throttle
constexpr uint32_t kSuppressMs        = 20000;   // Self-Select-Echo-Fenster (doPush_ haengt bis 18s)
constexpr uint32_t kPendingTimeoutMs  = 5000;    // Frist nach Selection: INVALID_SOURCE ODER kein PLAY_STATE -> Re-Arm
constexpr uint8_t  kMaxAttempts       = 2;       // Re-Arm-Versuche je Slot pro Cooldown
constexpr uint32_t kAttemptCooldownMs = 60000;   // Cooldown fuer den Versuchs-Cap
constexpr uint32_t kPingMs            = 30000;   // periodischer WS-Ping (Liveness)
constexpr uint32_t kIdleMs            = 90000;   // kein Frame so lange -> Socket tot -> reconnect

// v0.8.24 #15-Hotfix: Bisher hielt der Watcher PRO Speaker eine persistente gabbo-TCP-
// Socket offen. Bei Grossinstallationen (~10 Boxen) erschoepfen 9-10 Dauer-Sockets das
// geteilte lwIP-Budget (~16 BSD-Sockets + globaler TCP-PCB-Pool) — zusammen mit einem
// Discovery-Sweep (je ein WiFiClient/Speaker), Cloud-/Spotify-TLS und OTA. Folge:
// AsyncWebServer/OTA bekommen keine Verbindung mehr -> "SixBack nicht erreichbar",
// obwohl das Geraet laeuft. Schutz: hoechstens kMaxActiveConns gabbo-Sockets GLEICHZEITIG
// offen. Hat ein Geraet mehr LIR-Speaker als das Cap, rotiert das aktive Fenster fair
// (Best-Effort-Abdeckung statt Erreichbarkeits-Verlust). Bis kMaxActiveConns Speaker:
// volle Abdeckung wie bisher, keine Rotation. 4 laesst >=12 Sockets fuer Web/OTA/Cloud frei.
constexpr uint8_t  kMaxActiveConns    = 4;       // max. gleichzeitig offene gabbo-Sockets
constexpr uint32_t kRotateMs          = 45000;   // Fenster-Rotation bei mehr LIR-Speakern als Cap

// ---------- Loop-Guard Suppress-Map (von mehreren Tasks beschrieben) ----------
SemaphoreHandle_t g_supMtx = nullptr;
std::map<String, uint32_t> g_suppress;   // spIp -> lastSelfSelectMs

bool suppressed_(const String& ip) {
    if (!g_supMtx) return false;
    bool s = false;
    xSemaphoreTake(g_supMtx, portMAX_DELAY);
    auto it = g_suppress.find(ip);
    if (it != g_suppress.end() && (millis() - it->second) < kSuppressMs) s = true;
    xSemaphoreGive(g_supMtx);
    return s;
}

// ---------- Per-Connection State (NUR im Watcher-Task -> kein Mutex) ----------
struct Conn {
    String         deviceId;
    String         ip;
    GabboWsClient  ws;
    int            pendingSlot = -1;          // Slot aus letztem nowSelectionUpdated
    uint32_t       pendingTs = 0;
    uint32_t       lastConnectTry = 0;
    uint32_t       lastRxMs = 0;              // letzter empfangener Frame (Liveness)
    uint32_t       lastPingMs = 0;            // letzter gesendeter WS-Ping
    uint8_t        attempts[7]   = {0,0,0,0,0,0,0};   // [slot] Versuchs-Cap
    uint32_t       attemptWin[7] = {0,0,0,0,0,0,0};
};
std::map<String, Conn> g_conns;   // deviceId -> Conn (Knoten stabil, kein realloc-copy)

// v0.8.24: Rotations-Fenster fuer den Socket-Cap (nur Watcher-Task -> kein Mutex).
uint32_t g_rotOffset  = 0;        // Start-Index des aktiven Fensters in g_conns (geordnet)
uint32_t g_lastRotate = 0;        // millis() der letzten Fenster-Rotation

// nowSelectionUpdated -> preset id (1..6), sonst -1.
int parsePresetId_(const String& f) {
    int p = f.indexOf("preset id=\"");
    if (p < 0) return -1;
    p += 11;                                  // hinter das oeffnende Quote
    if (p >= (int)f.length()) return -1;
    char c = f.charAt(p);
    return (c >= '1' && c <= '6') ? (c - '0') : -1;
}

bool hasLirPreset_(const String& deviceId) {
    for (const auto& p : PresetStore::instance().getForSpeaker(deviceId))
        if (p.source == PresetSource::LOCAL_INTERNET_RADIO) return true;
    return false;
}

void reArm_(Conn& c, int slot) {
    if (slot < 1 || slot > 6) return;
    if (suppressed_(c.ip)) {
        Serial.printf("[gabbo] %s slot %d INVALID_SOURCE -> suppressed (own recent /select)\n",
                      c.deviceId.c_str(), slot);
        return;
    }
    uint32_t now = millis();
    if (now - c.attemptWin[slot] > kAttemptCooldownMs) { c.attemptWin[slot] = now; c.attempts[slot] = 0; }
    if (c.attempts[slot] >= kMaxAttempts) {
        Serial.printf("[gabbo] %s slot %d -> re-arm cap reached (source likely dead), giving up\n",
                      c.deviceId.c_str(), slot);
        return;
    }
    Preset p = PresetStore::instance().get(c.deviceId, (uint8_t)slot);
    if (p.source != PresetSource::LOCAL_INTERNET_RADIO) {
        // Nur kalte LIR/ORION re-armen. TUNEIN-INVALID_SOURCE = Migrations-/account-
        // full-Thema (#10/#11), nicht #15. OPAQUE/STORED_MUSIC = nicht re-armbar.
        return;
    }
    c.attempts[slot]++;
    Serial.printf("[gabbo] %s slot %d cold LIR -> RE-ARM via ORION /select (attempt %u)\n",
                  c.deviceId.c_str(), slot, c.attempts[slot]);
    int code = selectStationOnSpeaker(c.ip, p);   // ruft intern gabboMarkSelfSelect(ip)
    Serial.printf("[gabbo] %s slot %d re-arm /select -> HTTP %d\n", c.deviceId.c_str(), slot, code);
}

void handleFrame_(Conn& c, const String& f) {
    if (f.indexOf("nowSelectionUpdated") >= 0) {
        int slot = parsePresetId_(f);
        if (slot >= 1) { c.pendingSlot = slot; c.pendingTs = millis(); }
        return;
    }
    if (f.indexOf("PLAY_STATE") >= 0) { c.pendingSlot = -1; return; }   // Erfolg -> nichts tun
    if (f.indexOf("INVALID_SOURCE") >= 0 || f.indexOf("<errorUpdate") >= 0) {
        if (c.pendingSlot >= 1 && (millis() - c.pendingTs) < kPendingTimeoutMs) {
            int slot = c.pendingSlot;
            c.pendingSlot = -1;
            reArm_(c, slot);
        }
        return;
    }
}

void reconcile_() {
    auto snap = SpeakerInventory::instance().list();   // copy-by-value, lock-frei
    std::map<String, String> desired;                  // deviceId -> ip
    for (const auto& sp : snap) {
        if (!sp.ownedByUs) continue;
        if (sp.status == MigrationStatus::OFFLINE) continue;
        if (sp.ip.length() == 0) continue;
        if (!hasLirPreset_(sp.deviceId)) continue;     // nur Speaker mit kalt-faehigen LIR-Slots
        desired[sp.deviceId] = sp.ip;
    }
    // entfernen, was nicht mehr gewuenscht ist
    for (auto it = g_conns.begin(); it != g_conns.end(); ) {
        if (desired.find(it->first) == desired.end()) {
            Serial.printf("[gabbo] drop %s (not owned/migrated or no LIR preset)\n", it->first.c_str());
            it->second.ws.close();
            it = g_conns.erase(it);
        } else {
            ++it;
        }
    }
    // hinzufuegen / IP-Drift behandeln
    for (auto& d : desired) {
        auto it = g_conns.find(d.first);
        if (it == g_conns.end()) {
            Conn& c = g_conns[d.first];
            c.deviceId = d.first;
            c.ip = d.second;
            c.lastConnectTry = 0;
            Serial.printf("[gabbo] track %s @ %s\n", c.deviceId.c_str(), c.ip.c_str());
        } else if (it->second.ip != d.second) {
            Serial.printf("[gabbo] %s ip %s -> %s (reconnect)\n",
                          it->first.c_str(), it->second.ip.c_str(), d.second.c_str());
            it->second.ws.close();
            it->second.ip = d.second;
            it->second.lastConnectTry = 0;
        }
    }
}

void watcherTask_(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(kInitialWaitMs));
    Serial.println("[gabbo] watcher started");
    uint32_t lastReconcile = 0;
    while (true) {
        uint32_t now = millis();
        if (now - lastReconcile > kReconcileMs) { reconcile_(); lastReconcile = now; }
        // v0.8.24 Socket-Cap: aktives Fenster bestimmen (+ ggf. rotieren), damit nie mehr
        // als kMaxActiveConns gabbo-Sockets gleichzeitig offen sind.
        const size_t nConns = g_conns.size();
        if (nConns) g_rotOffset %= nConns;          // normalisieren (nConns kann via reconcile schrumpfen)
        if (nConns > kMaxActiveConns && millis() - g_lastRotate > kRotateMs) {
            g_lastRotate = millis();
            g_rotOffset  = (g_rotOffset + kMaxActiveConns) % nConns;
        }
        size_t connIdx = 0;
        for (auto& kv : g_conns) {
            Conn& c = kv.second;
            // Bis zum Cap alle aktiv (keine Rotation); darueber nur das rotierende Fenster
            // [g_rotOffset, g_rotOffset+kMaxActiveConns) modulo nConns.
            bool active = (nConns <= kMaxActiveConns) ||
                          (((connIdx + nConns - g_rotOffset) % nConns) < kMaxActiveConns);
            ++connIdx;
            if (!active) {
                // Inaktiv: Socket freigeben -> lwIP-Budget bleibt fuer Web/OTA/Cloud frei.
                // pendingSlot loeschen, sonst feuert beim Wiedereintritt ein stale Re-Arm.
                if (c.ws.connected()) c.ws.close();
                c.pendingSlot = -1;
                continue;
            }
            if (!c.ws.connected()) {
                if (millis() - c.lastConnectTry > kReconnectMs) {
                    c.lastConnectTry = millis();
                    if (c.ws.connect(c.ip, BOSE_GABBO_WS_PORT, 1500)) {
                        c.lastRxMs = millis();
                        c.lastPingMs = millis();
                        Serial.printf("[gabbo] connected %s @ %s\n", c.deviceId.c_str(), c.ip.c_str());
                    } else {
                        Serial.printf("[gabbo] connect failed %s @ %s\n", c.deviceId.c_str(), c.ip.c_str());
                    }
                }
                continue;
            }
            String frame;
            int budget = 32;                  // pro Conn max. 32 Frames je Runde
            while (budget-- > 0 && c.ws.poll(frame)) { c.lastRxMs = millis(); handleFrame_(c, frame); }
            // Liveness: ein still verschwundener Speaker (kein FIN/RST) laesst
            // connected() ewig true -> sonst verpasst der Watcher JEDEN Press.
            // gabbo sendet im Normalbetrieb regelmaessig Frames; periodischer Ping
            // erzwingt Antwort/Schreibfehler, kein Frame fuer kIdleMs = Socket tot
            // -> close -> Reconnect ueber den !connected()-Pfad.
            if (millis() - c.lastPingMs > kPingMs) {
                c.lastPingMs = millis();
                if (!c.ws.ping()) { c.ws.close(); continue; }
            }
            if (millis() - c.lastRxMs > kIdleMs) {
                Serial.printf("[gabbo] %s idle -> reconnect\n", c.deviceId.c_str());
                c.ws.close();
                continue;
            }
            // Timeout-Trigger: LIR-Slot selektiert, aber kein PLAY_STATE in der Frist.
            // Deckt den "live-but-stuck"-Modus ab: eine erreichbare Roh-URL liefert
            // HTTP 200/Audio statt eines Station-Deskriptors -> der Speaker wirft KEIN
            // INVALID_SOURCE, steckt aber ohne playStatus fest. Das ist der REALE
            // #15-Fall (echte LIR-Streams sind live). On-device verifiziert 06-11.
            if (c.pendingSlot >= 1 && (millis() - c.pendingTs) > kPendingTimeoutMs) {
                int slot = c.pendingSlot;
                c.pendingSlot = -1;
                reArm_(c, slot);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

} // anon

void gabboMarkSelfSelect(const String& spIp) {
    if (!g_supMtx) return;
    xSemaphoreTake(g_supMtx, portMAX_DELAY);
    g_suppress[spIp] = millis();
    xSemaphoreGive(g_supMtx);
}

void startGabboWatcher() {
    static bool started = false;
    if (started) return;
    started = true;
    g_supMtx = xSemaphoreCreateMutex();
    BaseType_t r = xTaskCreate(watcherTask_, "gabbo-watch", 8192,
                               nullptr, tskIDLE_PRIORITY + 1, nullptr);
    if (r != pdPASS) {
        Serial.println("[gabbo] watcher spawn FAILED");
        started = false;
    }
}

} // namespace sixback

#endif // SIXBACK_GABBO_WATCHER_ENABLED
