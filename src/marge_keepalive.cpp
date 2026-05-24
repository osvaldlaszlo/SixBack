// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "marge_keepalive.h"
#include "speaker_inventory.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>

namespace sixback {
namespace {

constexpr uint32_t kIntervalMs    = 60UL * 1000UL;         // 60s — aggressiver,
                                                            // weil Speaker den scmudc-Stream
                                                            // nach langer Pause droppt
constexpr uint32_t kInitialWaitMs = 30UL * 1000UL;         // 30s nach Boot warten
constexpr uint32_t kPerHostGapMs  = 250UL;                 // 250ms zwischen Hosts

bool g_taskStarted = false;

void pingOne_(const String& ip, const String& accountId) {
    if (ip.length() == 0 || accountId.length() == 0) return;
    WiFiClient wc;
    HTTPClient http;
    String url = "http://" + ip + ":8090/setMargeAccount";
    if (!http.begin(wc, url)) {
        Serial.printf("[marge-ka] begin failed %s\n", url.c_str());
        return;
    }
    http.addHeader("Content-Type", "application/xml");
    http.setTimeout(5000);
    String body = "<PairDeviceWithAccount><accountId>";
    body += accountId;
    body += "</accountId><userAuthToken>Bearer sixback-keepalive</userAuthToken></PairDeviceWithAccount>";
    int code = http.POST(body);
    http.end();
    Serial.printf("[marge-ka] %s -> HTTP %d\n", ip.c_str(), code);
}

void keepAliveTask_(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(kInitialWaitMs));
    Serial.println("[marge-ka] task started (5min interval)");
    while (true) {
        // Snapshot inventory (list() locked sich selbst und returnt copy by value).
        std::vector<std::pair<String, String>> targets;
        for (const auto& sp : SpeakerInventory::instance().list()) {
            // Skip offline + ohne accountId. SETTLING/MIGRATED/NOT_MIGRATED
            // alle gepingt.
            if (sp.status == MigrationStatus::OFFLINE) continue;
            if (sp.ip.length() == 0 || sp.accountId.length() == 0) continue;
            targets.push_back({sp.ip, sp.accountId});
        }
        if (targets.empty()) {
            Serial.println("[marge-ka] no eligible speakers");
        } else {
            for (const auto& t : targets) {
                pingOne_(t.first, t.second);
                vTaskDelay(pdMS_TO_TICKS(kPerHostGapMs));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kIntervalMs));
    }
}

} // anon

void startMargeKeepAlive() {
    if (g_taskStarted) return;
    g_taskStarted = true;
    BaseType_t r = xTaskCreate(keepAliveTask_, "marge-ka", 4096,
                                nullptr, tskIDLE_PRIORITY + 1, nullptr);
    if (r != pdPASS) {
        Serial.println("[marge-ka] task spawn FAILED");
        g_taskStarted = false;
    }
}

} // namespace sixback
