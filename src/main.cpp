// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Main
//
// Vollständige SixBack-Firmware:
//   - WiFi via Improv-Serial (NVS-persistent, SixBack läuft auf NVS-Credentials)
//   - HTTP-Server :8000 = Bose-Cloud-Replacement (11 Endpoints)
//   - HTTP-Server :80   = REST-API + Web-UI (SPA aus LittleFS)
//   - mDNS-Hostname: sixback.local
//   - Speaker-Inventory + Preset-Store persistent in NVS

#include <Arduino.h>
// Network.h MUSS vor WiFi.h includiert sein (arduino-esp32 3.x split),
// sonst zieht LDF die Network-Lib nicht und Linker bricht ab.
#include <Network.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "config.h"
#include "version.h"
#include "bose_endpoints.h"
#include "api_endpoints.h"
#include "event_store.h"
#include "spotify_player.h"
#include "wifi_provisioning.h"
#include "speaker_inventory.h"
#include "preset_store.h"
#include "ip_failsafe.h"
#include "system_health.h"
#include "auto_mode.h"
#include "nvs_helper.h"
#include "marge_keepalive.h"
#include "mbedtls_psram_alloc.h"

static AsyncWebServer boseServer(BOSE_HTTP_PORT);
static AsyncWebServer uiServer(UI_HTTP_PORT);

static void banner() {
    Serial.println();
    Serial.println("=========================================");
    Serial.printf("  %s %s\n", FW_NAME, FW_VERSION_STRING);
    Serial.printf("  Build: %s\n", FW_BUILD_DATE);
    Serial.println("  Copyright (c) 2026 Dirk Tostmann");
    Serial.println("  License: PolyForm Noncommercial 1.0.0");
    Serial.println("  Commercial use prohibited — see LICENSE");
    Serial.println("=========================================");
}

static void mountFS() {
    // Erst no-format probieren — wenn das klappt, ist alles normal.
    if (LittleFS.begin(false)) {
        Serial.printf("[fs]   LittleFS mounted, %u/%u bytes used\n",
                      (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
        return;
    }
    // Mount failed (z.B. Brownout-Korruption oder erster Boot nach Erase).
    // Jetzt explizit formatieren MIT Log — sonst wuerde die WebUI lautlos
    // verschwinden und der Bose-Cloud-Mock haette keine bmx_services.json
    // mehr zum Ausliefern.
    Serial.println("[fs]   LittleFS mount failed — formatting (WebUI must be re-uploaded via /api/ota/fs)");
    if (!LittleFS.begin(true)) {
        Serial.println("[fs]   LittleFS format ALSO failed — FS unavailable");
        return;
    }
    Serial.printf("[fs]   LittleFS formatted, %u/%u bytes used\n",
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
}

static void connectWifi() {
    sixback::provisionWifi();
    Serial.printf("[wifi] up, IP=%s MAC=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.macAddress().c_str(),
                  WiFi.RSSI());
}

static void startMDNS() {
    if (!MDNS.begin(MDNS_HOSTNAME)) { Serial.println("[mdns] failed"); return; }
    MDNS.addService("http", "tcp", UI_HTTP_PORT);
    MDNS.addService("sixback", "tcp", BOSE_HTTP_PORT);
    MDNS.addServiceTxt("sixback", "tcp", "version", FW_VERSION_STRING);
    MDNS.addServiceTxt("sixback", "tcp", "build",   FW_BUILD_DATE);
    Serial.printf("[mdns] %s.local advertised\n", MDNS_HOSTNAME);
}

static void startBoseServer() {
    sixback::eventStoreInit();
    sixback::spotify::init();      // registriert preset-pressed-Callback
    registerBoseEndpoints(boseServer);
    boseServer.begin();
    Serial.printf("[bose] cloud-replacement listening on :%d\n", BOSE_HTTP_PORT);
    Serial.printf("[bose] tell speakers to use: http://%s:%d\n",
                  WiFi.localIP().toString().c_str(), BOSE_HTTP_PORT);
}

static void startUiServer() {
    registerApiEndpoints(uiServer);
    uiServer.begin();
    Serial.printf("[ui]   web/api listening on http://%s:%d/\n",
                  WiFi.localIP().toString().c_str(), UI_HTTP_PORT);
    Serial.printf("[ui]   mDNS: http://%s.local/\n", MDNS_HOSTNAME);
}

static void initInventory() {
    sixback::SpeakerInventory::instance().loadFromNVS();
    sixback::PresetStore::instance().loadFromNVS();
    Serial.printf("[inv]  %u known speakers, presets loaded\n",
                  (unsigned)sixback::SpeakerInventory::instance().list().size());
}

void setup() {
    Serial.begin(115200);
    delay(500);
    banner();

    // mbedtls auf PSRAM umlenken BEVOR irgendwas TLS spricht. Loest -32512
    // (SSL_MEMORY_ALLOC_FAILED) auf Sticks mit min_free < 32KB.
    sixback::installPsramMbedtlsAllocator();

    mountFS();
    // BoseFix32 -> SixBack: einmalige NVS-Daten-Migration VOR jedem
    // loadFromNVS()/connectWifi(). Idempotent + no-op nach erstem
    // erfolgreichen Boot. Siehe nvs_helper.{h,cpp}.
    sixback::migrateAllBosefixNvs();
    connectWifi();
    startMDNS();
    initInventory();
    sixback::ipFailsafeCheck();   // IP-Change-Detection + Auto-Remigrate
    startBoseServer();
    startUiServer();
    sixback::healthInit();        // Crash-Counter, Task-WDT, WiFi-/Heap-Watchdog
    sixback::startAutoModeTask(); // No-op wenn NVS-Flag auto-mode disabled
    sixback::startMargeKeepAlive(); // 5min-Ping an /setMargeAccount damit
                                    // scmudc-Event-Stream nicht stehen bleibt

    Serial.println();
    Serial.printf("[setup] SixBack ready - UI: http://%s.local/\n", MDNS_HOSTNAME);
    Serial.printf("[setup] Trigger speaker discovery: curl -X POST http://%s.local/api/speakers/discover\n",
                  MDNS_HOSTNAME);
}

void loop() {
    sixback::wifiProvisioningTick();
    sixback::healthTick();         // Task-WDT-feed, WiFi/Heap-Watchdog, Self-Ping
    sixback::spotify::refreshTick(); // proaktiver Access-Token-Refresh, 60s-Rate-Limit intern
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat > 30000) {
        lastBeat = millis();
        Serial.printf("[heartbeat] up=%lus  RSSI=%d  free-heap=%u  PSRAM-free=%u\n",
                      millis() / 1000, WiFi.RSSI(),
                      ESP.getFreeHeap(), ESP.getFreePsram());
    }
    delay(50);
}
