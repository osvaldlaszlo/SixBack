// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — WiFi-Provisionierung

#include "wifi_provisioning.h"
#include "captive_portal.h"
#include "version.h"
#include <WiFi.h>
#include <Preferences.h>
#include "ImprovWiFiLibrary.h"
#include <esp_wifi.h>

namespace bosefix {

namespace {

constexpr const char* NVS_NS    = "bosefix-wifi";
constexpr const char* KEY_SSID  = "ssid";
constexpr const char* KEY_PSK   = "psk";

// Idle-basiertes Window (RFNETHM-Pattern): keine Activity fuer idle_ms → zu.
//   Keine NVS-Creds:  120 s (Erstprovisioning, langsamer User)
//   NVS-Creds da:      30 s (schnelle Reconfig, sonst sofort zu)
// Jedes UART-Byte verlaengert das Window auf "idle_ms ab jetzt".
constexpr uint32_t IMPROV_IDLE_FRESH    = 120 * 1000;
constexpr uint32_t IMPROV_IDLE_HASCREDS =  30 * 1000;

ImprovWiFi  improvSerial(&Serial);
bool        improvActive          = false;
uint32_t    improvStartMs         = 0;
uint32_t    improvLastActivityMs  = 0;
uint32_t    improvIdleMs          = IMPROV_IDLE_FRESH;

void onImprovConnected(const char* ssid, const char* pw) {
    Serial.printf("[improv] connected -> ssid=%s\n", ssid);
    persistCreds(ssid ? ssid : "", pw ? pw : "");
    improvActive = false;
    wifiOptimizeForReliability();   // Power-Save aus, sobald STA up ist
}

void onImprovError(ImprovTypes::Error err) {
    Serial.printf("[improv] error 0x%02X\n", static_cast<unsigned>(err));
}

bool tryConnectFromNVS() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return false;
    String ssid = prefs.getString(KEY_SSID, "");
    String psk  = prefs.getString(KEY_PSK,  "");
    prefs.end();
    if (ssid.length() == 0) {
        Serial.println("[wifi] no NVS credentials");
        return false;
    }
    Serial.printf("[wifi] NVS credentials present, trying ssid=%s\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), psk.c_str());
    uint32_t t0 = millis();
    // WICHTIG: improvSerial waehrend des STA-connect-waits weiter pumpen.
    // tryConnectFromNVS blockt sonst bis zu 20 s und ESP Web Tools
    // bekommt keine Antwort auf seine Improv-Setup-Frames — Symptom:
    // "Initializing Improv Serial → SCHEDULE RETRY 0,1,2" + Abbruch.
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        improvSerial.handleSerial();
        delay(50);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());
        wifiOptimizeForReliability();
        improvLastActivityMs = millis();
        Serial.printf("[improv] reset window after AP connect — improvLastActivityMs=%lu improvActive=%d\n",
                      (unsigned long)improvLastActivityMs, (int)improvActive);
        return true;
    }
    Serial.println("[wifi] NVS-credential connect timed out");
    WiFi.disconnect(true);
    return false;
}

bool nvsHasCreds() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return false;
    String s = prefs.getString(KEY_SSID, "");
    prefs.end();
    return s.length() > 0;
}


void startImprovMode() {
    improvIdleMs = nvsHasCreds() ? IMPROV_IDLE_HASCREDS : IMPROV_IDLE_FRESH;
    Serial.printf("[improv] starting (idle-window %us, %sNVS-creds present)\n",
                  improvIdleMs / 1000, nvsHasCreds() ? "" : "no ");
    // WiFi-STA-Mode GARANTIEREN: Improv ruft WiFi.scanNetworks() wenn
    // ESP Web Tools "Scan WiFi" triggert.  Im no-creds-Boot ist Mode
    // sonst noch WIFI_MODE_NULL (tryConnectFromNVS skipt vor uns), und
    // scan returnt WIFI_SCAN_FAILED → leere Liste im UI.  Auch falls
    // captive spaeter AP_STA setzt: STA ist bereits angefordert.
    if (WiFi.getMode() == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_STA);
    }
    // Country-Code / Scan-Range EXPLIZIT auf EU-2.4GHz (Ch 1-13) setzen.
    // Default ist `CN` o.ae. mit policy=AUTO, was bei ESP32-C6 + WiFi-6-
    // Stack in einen NO_AP_FOUND-Pfad fuehrt wenn der AP auf Ch 12/13
    // sitzt (typisch nach Fritzbox-Auto-Channel-Selection). S3/C3 sind
    // toleranter, C6 strikter — daher symptomatisch nur C6-Boards.
    // POLICY_MANUAL stellt sicher dass der Connect-Pfad NICHT per
    // Beacon-Override auf einen restriktiveren Channel-Set zurueckfaellt.
    {
        wifi_country_t c = {
            .cc      = "DE",
            .schan   = 1,
            .nchan   = 13,
            .max_tx_power = 78,    // 19.5 dBm — Default Arduino-Core
            .policy  = WIFI_COUNTRY_POLICY_MANUAL,
        };
        esp_wifi_set_country(&c);
    }
    // PS-Mode AUS *VOR* irgendeinem WiFi.begin() — gilt sowohl fuer
    // tryConnectFromNVS als auch fuer den begin(), den die improv-Lib
    // bei Send-WiFi-Settings selbst ausfuehrt. C6+WiFi-6-Defaults
    // (WIFI_PS_MIN_MODEM, DTIM-basiert) kappen sonst das 4-way-
    // handshake-Timing → reproduzierbarer 4WAY_HANDSHAKE_TIMEOUT auf
    // WPA2-Mixed-APs. EULFW32 / ip4knx setzen das gleiche vor begin();
    // BoseFix32 hat es bis v0.5.450 erst NACH Connect via
    // wifiOptimizeForReliability() gesetzt — zu spaet wenn der Connect
    // selbst schon scheitert.
    WiFi.setSleep(WIFI_PS_NONE);
    WiFi.setAutoReconnect(true);
    // Chip-Family per IDF-Target, damit ESP Web Tools die korrekte
    // chipFamily-String fuer ihren UI-flow sieht.
    ImprovTypes::ChipFamily cf =
#if CONFIG_IDF_TARGET_ESP32S3
        ImprovTypes::CF_ESP32_S3
#elif CONFIG_IDF_TARGET_ESP32C3
        ImprovTypes::CF_ESP32_C3
#elif CONFIG_IDF_TARGET_ESP32C6
        ImprovTypes::CF_ESP32_C6
#elif CONFIG_IDF_TARGET_ESP32S2
        ImprovTypes::CF_ESP32_S2
#else
        ImprovTypes::CF_ESP32
#endif
        ;
    improvSerial.setDeviceInfo(
        cf,
        FW_NAME,
        FW_VERSION_STRING,
        "BoseFix32"
    );
    improvSerial.onImprovConnected(onImprovConnected);
    improvSerial.onImprovError(onImprovError);
    improvActive          = true;
    improvStartMs         = millis();
    improvLastActivityMs  = improvStartMs;
}

// Tick-Helper: jedes UART-Byte gilt als Aktivitaet → idle-Timer zuruecksetzen.
// Konservativ: auch User-Tipperei (= keine valide Improv-Frame) verlaengert
// das Window, damit niemand mitten im Provisioning rausgekickt wird.
void improvTickInternal() {
    if (!improvActive) return;
    if (Serial.available()) improvLastActivityMs = millis();
    improvSerial.handleSerial();
    const uint32_t idle = millis() - improvLastActivityMs;
    if (idle > improvIdleMs) {
        Serial.printf("[improv] idle %us — window closed\n", idle / 1000);
        improvActive = false;
    }
}

} // anon

void provisionWifi() {
    // Improv-Window IMMER ab Boot oeffnen (120 s), egal ob NVS-Creds da
    // sind. Damit kann der User auch ohne Factory-Reset jederzeit nach
    // einem Boot neue WLAN-Credentials einspeisen — z.B. weil sich der
    // Router/das WLAN geaendert hat oder der Speaker an einen anderen
    // Standort wandert. Pattern uebernommen aus RFNETHM (improv_glue.cpp):
    // Improv ist NICHT Fallback, sondern Standard-Service waehrend der
    // Boot-Phase; der NVS-Reconnect-Versuch laeuft parallel dazu.
    startImprovMode();

    if (tryConnectFromNVS()) {
        // WiFi up. Improv-Window laeuft via wifiProvisioningTick() noch
        // bis zum 120 s-Ende weiter — User kann jederzeit re-provisionieren.
        return;
    }

    // Kein Connect aus NVS (oder keine Creds vorhanden) — Improv (laeuft
    // schon) und Captive-Portal parallel arm-en. Sobald eine Quelle
    // erfolgreich provisioniert, ist WiFi.status() == WL_CONNECTED und
    // wir brechen aus, schliessen beide Fenster.
    captiveStart();

    Serial.println("[provision] no NVS connect — waiting on improv OR captive");
    uint32_t connectedAtMs = 0;
    constexpr uint32_t POST_CONNECT_GRACE_MS = 15 * 1000;
    while (improvActive || captiveIsActive()) {
        improvTickInternal();
        captiveTick();
        if (WiFi.status() == WL_CONNECTED && connectedAtMs == 0) {
            connectedAtMs = millis();
            improvLastActivityMs = connectedAtMs;
            Serial.println("[provision] STA up — keeping captive alive for 15s grace "
                           "(browser still polling /save_status)");
        }
        if (connectedAtMs && millis() - connectedAtMs > POST_CONNECT_GRACE_MS) {
            Serial.println("[provision] grace expired — tearing down captive AP");
            break;
        }
        delay(10);
    }
    captiveStop();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[provision] all windows expired, no provisioning — restart in 10s");
        delay(10000);
        ESP.restart();
    }
}

void factoryResetWifi() {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.clear();
    prefs.end();
    Serial.println("[wifi] NVS credentials erased");
}

void wifiProvisioningTick() {
    improvTickInternal();
}

bool improvIsActive() { return improvActive; }

uint32_t improvWindowRemainingS() {
    if (!improvActive) return 0;
    const uint32_t idle = millis() - improvLastActivityMs;
    return idle >= improvIdleMs ? 0 : (improvIdleMs - idle) / 1000;
}

// Public-API-Variante (Header-deklariert) — delegiert auf die anonyme
// Helper-Variante oben, damit captive_portal.cpp denselben NVS-Pfad nutzt.
void persistCreds(const String& ssid, const String& psk) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putString(KEY_SSID, ssid);
    p.putString(KEY_PSK,  psk);
    p.end();
    Serial.printf("[wifi] credentials persisted (ssid=%s)\n", ssid.c_str());
}

void wifiOptimizeForReliability() {
    // 1) WiFi-Modem-Sleep KOMPLETT AUS. Default ist WIFI_PS_MIN_MODEM
    //    (DTIM-basiert), das auf C3/C6 mit WiFi 6 zu Ping-Latenzen
    //    > 1 s fuehrt — und damit zu spuerbaren Verzoegerungen bzw
    //    Hangs beim Speaker, wenn er die Cloud-Endpoints abfragt.
    WiFi.setSleep(WIFI_PS_NONE);

    // 2) TX-Power auf Maximum.
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    // 3) Auto-Reconnect explizit ein.
    WiFi.setAutoReconnect(true);

    // 4) Keine WiFi-Creds in den internen ESP-WiFi-NVS spiegeln (wir
    //    persistieren selbst). Spart Flash-Schreibzyklen + verhindert
    //    konkurrierende Quellen.
    WiFi.persistent(false);

    // 5) CPU auf maximale Frequenz pinnen. arduino-esp32 cappt das
    //    automatisch auf den Chip-Max: S3/Classic 240 MHz, C3/C6 160 MHz.
    setCpuFrequencyMhz(240);

    Serial.printf("[wifi] PS=NONE TX=max CPU=%lu MHz — optimized for reliability\n",
                  (unsigned long)getCpuFrequencyMhz());
}

} // namespace bosefix
