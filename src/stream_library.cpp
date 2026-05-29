// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "stream_library.h"

#include "nvs_helper.h"

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace sixback {
namespace streams {
namespace {

constexpr const char* kNvsNs  = "sixback-strm";
constexpr const char* kNvsKey = "items";

std::vector<StreamItem> g_streams;
SemaphoreHandle_t       g_mtx = nullptr;

void ensureMutex_() {
    if (!g_mtx) g_mtx = xSemaphoreCreateMutex();
}

// Caller must hold g_mtx.
void saveToNVS_() {
    JsonDocument doc;
    JsonArray arr = doc["items"].to<JsonArray>();
    for (const auto& it : g_streams) {
        JsonObject o = arr.add<JsonObject>();
        o["name"]        = it.name;
        o["streamUrl"]   = it.streamUrl;
        o["imageUrl"]    = it.imageUrl;
        o["icyName"]     = it.icyName;
        o["contentType"] = it.contentType;
        o["bitrate"]     = it.bitrate;
    }
    nvsSaveJsonWithCleanup(kNvsNs, kNvsKey, doc);
}

}  // namespace

void init() {
    ensureMutex_();
    JsonDocument doc;
    if (!nvsLoadJson(kNvsNs, kNvsKey, doc)) {
        Serial.println("[stream] no NVS streams, start empty");
        return;
    }
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return;
    g_streams.clear();
    for (JsonObject o : doc["items"].as<JsonArray>()) {
        StreamItem it;
        it.name        = (const char*)(o["name"]        | "");
        it.streamUrl   = (const char*)(o["streamUrl"]   | "");
        it.imageUrl    = (const char*)(o["imageUrl"]    | "");
        it.icyName     = (const char*)(o["icyName"]     | "");
        it.contentType = (const char*)(o["contentType"] | "");
        it.bitrate     = (const char*)(o["bitrate"]     | "");
        if (it.streamUrl.length() > 0) g_streams.push_back(it);
    }
    xSemaphoreGive(g_mtx);
    Serial.printf("[stream] loaded %u stream(s) from NVS\n",
                  (unsigned)g_streams.size());
}

bool addStreamItem(const StreamItem& item) {
    ensureMutex_();
    if (item.streamUrl.length() == 0) return false;
    bool created = true;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    for (auto& s : g_streams) {
        if (s.streamUrl == item.streamUrl) {
            s.name        = item.name;
            s.imageUrl    = item.imageUrl;
            s.icyName     = item.icyName;
            s.contentType = item.contentType;
            s.bitrate     = item.bitrate;
            created = false;
            break;
        }
    }
    if (created) g_streams.push_back(item);
    saveToNVS_();
    xSemaphoreGive(g_mtx);
    Serial.printf("[stream] %s url=%s name=%s\n", created ? "add" : "upd",
                  item.streamUrl.c_str(), item.name.c_str());
    return created;
}

bool removeStreamItem(const String& streamUrl) {
    ensureMutex_();
    if (streamUrl.length() == 0) return false;
    bool removed = false;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    for (auto it = g_streams.begin(); it != g_streams.end(); ) {
        if (it->streamUrl == streamUrl) { it = g_streams.erase(it); removed = true; }
        else ++it;
    }
    if (removed) saveToNVS_();
    xSemaphoreGive(g_mtx);
    if (removed) Serial.printf("[stream] rm url=%s\n", streamUrl.c_str());
    return removed;
}

std::vector<StreamItem> listStreams() {
    ensureMutex_();
    std::vector<StreamItem> copy;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        copy = g_streams;
        xSemaphoreGive(g_mtx);
    }
    return copy;
}

}  // namespace streams
}  // namespace sixback
