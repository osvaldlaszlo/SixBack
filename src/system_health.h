// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — System-Health (Watchdogs, Crash-Counter, Self-Ping)
//
// Pflicht in loop():  sixback::healthTick()
// Pflicht in setup(): sixback::healthInit()
//
// Was es macht:
//   - Liest beim Boot esp_reset_reason() + persistente Counter aus NVS
//     (Namespace "sixback-sys": boot_count, crash_count, wifi_reboots,
//      heap_reboots, last_reason, last_boot_ts).
//   - Subscribed die Loop-Task am Task-Watchdog (Timeout HEALTH_TWDT_S),
//     ruft esp_task_wdt_reset() in jedem tick().
//   - WiFi-Watchdog: nach HEALTH_WIFI_DOWN_REBOOT_S durchgaengiger
//     Disconnect-Phase -> ESP.restart() + wifi_reboots++.
//   - Heap-Watchdog: wenn free-heap < HEALTH_HEAP_LOW_BYTES laenger als
//     HEALTH_HEAP_LOW_REBOOT_S anhaelt -> ESP.restart() + heap_reboots++.
//   - Self-Ping: alle HEALTH_PING_INTERVAL_S sec ein leichter
//     GET /info (Port 8090) an jeden bekannten Speaker. Bei Erfolg:
//     lastSeenMs aktualisieren. Bei N Misses: status=OFFLINE.
//
// Health-Snapshot fuer /api/status: sixback::healthToJson(JsonObject&).

#ifndef BOSEFIX32_SYSTEM_HEALTH_H
#define BOSEFIX32_SYSTEM_HEALTH_H

#include <ArduinoJson.h>

namespace sixback {

// In setup() einmal aufrufen.
void healthInit();

// In loop() bei jeder Iteration aufrufen.
void healthTick();

// Fuer /api/status — fuegt "health"-Object an doc[key] ein.
void healthToJson(JsonObject out);

// Reset-Reason vom letzten Boot als kurzer Text ("POWERON","PANIC","WDT",...)
const char* lastResetReasonStr();

}  // namespace sixback

#endif  // BOSEFIX32_SYSTEM_HEALTH_H
