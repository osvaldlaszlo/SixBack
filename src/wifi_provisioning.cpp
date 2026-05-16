// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — WiFi-Provisionierung

#include "wifi_provisioning.h"
#include "captive_portal.h"
#include "version.h"
#include <WiFi.h>
#include <Preferences.h>
#include "ImprovWiFiLibrary.h"
#include <esp_wps.h>
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

// WPS Push-Button-Config (laeuft autark via esp_wps + WiFi-Event-Handler).
constexpr uint32_t WPS_WINDOW_MS = 120 * 1000;
bool        wpsActive  = false;
uint32_t    wpsStartMs = 0;

void onImprovConnected(const char* ssid, const char* pw) {
    Serial.printf("[improv] connected -> ssid=%s\n", ssid);
    persistCreds(ssid ? ssid : "", pw ? pw : "");
    improvActive = false;
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
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());
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


// WPS-Event-Handler — wird bei SUCCESS/FAILED/TIMEOUT vom WiFi-Driver
// gerufen. Bei SUCCESS holt er sich die vom Router gelieferten Creds aus
// der STA-Config, persistiert sie und stoesst einen WiFi.begin() an.
void onWpsEvent(arduino_event_id_t event, arduino_event_info_t /*info*/) {
    switch (event) {
        case ARDUINO_EVENT_WPS_ER_SUCCESS: {
            wifi_config_t cfg;
            esp_wifi_get_config(WIFI_IF_STA, &cfg);
            String ssid((const char*)cfg.sta.ssid);
            String psk((const char*)cfg.sta.password);
            Serial.printf("[wps] SUCCESS — got ssid=%s from router\n", ssid.c_str());
            persistCreds(ssid, psk);
            esp_wifi_wps_disable();
            wpsActive = false;
            WiFi.begin(ssid.c_str(), psk.c_str());
            break;
        }
        case ARDUINO_EVENT_WPS_ER_FAILED:
            Serial.println("[wps] FAILED");
            esp_wifi_wps_disable();
            wpsActive = false;
            break;
        case ARDUINO_EVENT_WPS_ER_TIMEOUT:
            Serial.println("[wps] TIMEOUT");
            esp_wifi_wps_disable();
            wpsActive = false;
            break;
        default: break;
    }
}

void startWpsMode() {
    Serial.println("[wps] arming WPS-PBC — press the WPS button on your router (120 s window)");
    WiFi.mode(WIFI_STA);
    WiFi.onEvent(onWpsEvent);
    esp_wps_config_t cfg = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
    strncpy((char*)cfg.factory_info.manufacturer, "Busware",      sizeof(cfg.factory_info.manufacturer) - 1);
    strncpy((char*)cfg.factory_info.model_number, FW_VERSION_STRING,
            sizeof(cfg.factory_info.model_number) - 1);
    strncpy((char*)cfg.factory_info.model_name,   "BoseFix32",    sizeof(cfg.factory_info.model_name) - 1);
    strncpy((char*)cfg.factory_info.device_name,  "BoseFix32",    sizeof(cfg.factory_info.device_name) - 1);
    esp_err_t e1 = esp_wifi_wps_enable(&cfg);
    if (e1 != ESP_OK) {
        Serial.printf("[wps] esp_wifi_wps_enable failed: %d\n", (int)e1);
        return;
    }
    esp_err_t e2 = esp_wifi_wps_start(0);  // 0 = use default timeout
    if (e2 != ESP_OK) {
        Serial.printf("[wps] esp_wifi_wps_start failed: %d\n", (int)e2);
        esp_wifi_wps_disable();
        return;
    }
    wpsActive  = true;
    wpsStartMs = millis();
}

void startImprovMode() {
    improvIdleMs = nvsHasCreds() ? IMPROV_IDLE_HASCREDS : IMPROV_IDLE_FRESH;
    Serial.printf("[improv] starting (idle-window %us, %sNVS-creds present)\n",
                  improvIdleMs / 1000, nvsHasCreds() ? "" : "no ");
    improvSerial.setDeviceInfo(
        ImprovTypes::CF_ESP32_S3,
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
        // WPS bleibt aus, weil die NVS-Creds gut sind.
        return;
    }

    // Kein Connect aus NVS (oder keine Creds vorhanden) — Improv (laeuft
    // schon), WPS und Captive-Portal parallel arm-en. Sobald eine Quelle
    // erfolgreich provisioniert, ist WiFi.status() == WL_CONNECTED und wir
    // brechen aus, schliessen alle drei Fenster.
    startWpsMode();
    captiveStart();

    Serial.println("[provision] no NVS connect — waiting on improv OR WPS OR captive");
    while (improvActive || wpsActive || captiveIsActive()) {
        improvTickInternal();
        captiveTick();
        if (WiFi.status() == WL_CONNECTED) break;
        // Safety: WPS-Lib sollte sich selbst nach WPS_WINDOW_MS deaktivieren,
        // aber wenn der Event-Handler aus irgendwelchen Gruenden nicht feuert,
        // ziehen wir hier mit 5 s Puffer den Stecker.
        if (wpsActive && (millis() - wpsStartMs) > WPS_WINDOW_MS + 5000) {
            Serial.println("[wps] safety-timeout exceeded — disabling");
            esp_wifi_wps_disable();
            wpsActive = false;
        }
        delay(10);
    }

    // Captive ggf. noch laufen lassen, damit der User die Success-Page sieht
    // — nur wenn der Connect NICHT via captive selbst kam (sonst hat captive
    // schon eine response auf /save geschickt, brauchen wir nichts extra).
    // Aber den AP muss man irgendwann zumachen. Lass den loop oben drueber
    // entscheiden — wenn captive_idle-Window ablaeuft, stoppt es sich selbst.
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

bool wpsIsActive() { return wpsActive; }

uint32_t wpsWindowRemainingS() {
    if (!wpsActive) return 0;
    const uint32_t elapsed = millis() - wpsStartMs;
    return elapsed >= WPS_WINDOW_MS ? 0 : (WPS_WINDOW_MS - elapsed) / 1000;
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

} // namespace bosefix
