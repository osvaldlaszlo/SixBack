// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "spotify_player.h"

#ifdef SIXBACK_SPOTIFY_ENABLED

#include "event_store.h"
#include "nvs_helper.h"
#include "speaker_inventory.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <map>

namespace sixback {
namespace spotify {
namespace {

constexpr const char* kNvsNs         = "sixback-spot";
constexpr const char* kNvsKeySlots   = "slots";
constexpr const char* kNvsKeyAuth    = "auth";
constexpr const char* kNvsKeyAppCrd  = "appcr";   // App-Creds (clientId + clientSecret) — persistent über Disconnect
constexpr const char* kNvsKeyLibrary = "library"; // Spotify-Library (Sidebar-Tiles)
constexpr uint32_t    kDevCacheTtlMs    = 5 * 60 * 1000;  // 5 min — positive hits
constexpr uint32_t    kDevCacheNegTtlMs = 30 * 1000;      // 30s — negative hits
                                                           // (kuerzer damit System schnell reagiert wenn User Speaker manuell in
                                                           // Spotify-App connected hat = Speaker erscheint dann in /devices)

std::vector<SlotMapping>   g_slots;
std::vector<LibraryItem>   g_library;
SemaphoreHandle_t          g_mtx = nullptr;

AuthState                  g_auth;
bool                       g_authLoaded = false;

struct DevCacheEntry {
    String  spotifyDeviceId;
    uint32_t fetchedMs = 0;
};
std::map<String, DevCacheEntry> g_devCache;   // speakerName -> entry

// -----------------------------------------------------------------------------
// Persistent TLS-Clients fuer Spotify-API.
// Statt pro Call frische WiFiClientSecure/HTTPClient zu allokieren (= jedesmal
// ~30-40KB internal-Heap fuer TLS-Handshake, fragmentiert ueber Stunden) halten
// wir 2 persistente Clients:
//   - accounts.spotify.com (Token-Refresh, typisch 1x pro Stunde)
//   - api.spotify.com      (/devices + /play, typisch pro Trigger)
// Connection-Reuse via HTTP-Keep-Alive — Spotify-Server supportet das.
// Bei Connection-Fehler (Stale TCP) macht HTTPClient automatisch Reconnect.
// -----------------------------------------------------------------------------
WiFiClientSecure*   g_tlsAccounts = nullptr;
WiFiClientSecure*   g_tlsApi      = nullptr;
SemaphoreHandle_t   g_tlsMtx      = nullptr;

void ensureTlsClients_() {
    if (g_tlsAccounts == nullptr) {
        g_tlsAccounts = new WiFiClientSecure();
        g_tlsAccounts->setInsecure();
        g_tlsAccounts->setTimeout(8);
    }
    if (g_tlsApi == nullptr) {
        g_tlsApi = new WiFiClientSecure();
        g_tlsApi->setInsecure();
        g_tlsApi->setTimeout(8);
    }
    if (g_tlsMtx == nullptr) {
        g_tlsMtx = xSemaphoreCreateMutex();
    }
}

// Debug — Ring-Buffer der letzten 8 Trigger-Versuche (FIFO, lock-protected).
constexpr size_t kTrigLogLen = 8;
TriggerLogEntry  g_trigLog[kTrigLogLen];
size_t           g_trigLogHead  = 0;
size_t           g_trigLogCount = 0;

void pushTrigLog_(const TriggerLogEntry& e) {
    // Lock-frei, aber wir laufen aus triggerTaskEntry_-Task; concurrent reads
    // aus dem AsyncWebServer-Handler sind moeglich. Wir nehmen das (eventually
    // consistent) als akzeptabel — kein crash-risk weil ringbuffer hat fixe
    // Groesse und String-copies sind atomar genug fuer das Debug-Display.
    g_trigLog[g_trigLogHead] = e;
    g_trigLogHead = (g_trigLogHead + 1) % kTrigLogLen;
    if (g_trigLogCount < kTrigLogLen) g_trigLogCount++;
}

void ensureMutex_() {
    if (!g_mtx) g_mtx = xSemaphoreCreateMutex();
}

void loadFromNVS_() {
    JsonDocument doc;
    if (!nvsLoadJson(kNvsNs, kNvsKeySlots, doc)) {
        Serial.println("[spot] no NVS slot-map, start empty");
        return;
    }
    g_slots.clear();
    for (JsonObject o : doc["slots"].as<JsonArray>()) {
        SlotMapping m;
        m.deviceId    = (const char*)(o["deviceId"]    | "");
        m.slot        = o["slot"]                       | 0;
        m.spotifyUri  = (const char*)(o["spotifyUri"]  | "");
        m.displayName = (const char*)(o["displayName"] | "");
        m.shuffle     = o["shuffle"]                    | false;
        m.repeatMode  = (const char*)(o["repeatMode"]  | "off");
        if (m.deviceId.length() > 0 && m.slot >= 1 && m.slot <= 6
            && m.spotifyUri.length() > 0) {
            g_slots.push_back(m);
        }
    }
    Serial.printf("[spot] loaded %u slot-mappings from NVS\n",
                  (unsigned)g_slots.size());
}

void saveToNVS_() {
    JsonDocument doc;
    JsonArray arr = doc["slots"].to<JsonArray>();
    for (const auto& m : g_slots) {
        JsonObject o = arr.add<JsonObject>();
        o["deviceId"]    = m.deviceId;
        o["slot"]        = m.slot;
        o["spotifyUri"]  = m.spotifyUri;
        o["displayName"] = m.displayName;
        o["shuffle"]     = m.shuffle;
        o["repeatMode"]  = m.repeatMode;
    }
    nvsSaveJsonWithCleanup(kNvsNs, kNvsKeySlots, doc);
}

void loadLibraryFromNVS_() {
    JsonDocument doc;
    if (!nvsLoadJson(kNvsNs, kNvsKeyLibrary, doc)) {
        Serial.println("[spot] no NVS library, start empty");
        return;
    }
    g_library.clear();
    for (JsonObject o : doc["items"].as<JsonArray>()) {
        LibraryItem it;
        it.uri        = (const char*)(o["uri"]        | "");
        it.name       = (const char*)(o["name"]       | "");
        it.imageUrl   = (const char*)(o["imageUrl"]   | "");
        it.shuffle    = o["shuffle"]                   | false;
        it.repeatMode = (const char*)(o["repeatMode"] | "off");
        if (it.uri.length() > 0) {
            g_library.push_back(it);
        }
    }
    Serial.printf("[spot] loaded %u library-items from NVS\n",
                  (unsigned)g_library.size());
}

void saveLibraryToNVS_() {
    JsonDocument doc;
    JsonArray arr = doc["items"].to<JsonArray>();
    for (const auto& it : g_library) {
        JsonObject o = arr.add<JsonObject>();
        o["uri"]        = it.uri;
        o["name"]       = it.name;
        o["imageUrl"]   = it.imageUrl;
        o["shuffle"]    = it.shuffle;
        o["repeatMode"] = it.repeatMode;
    }
    nvsSaveJsonWithCleanup(kNvsNs, kNvsKeyLibrary, doc);
}

// Beim Boot: wenn Library leer aber Slots haben Spotify-URIs → automatisch
// jeden unique URI in die Library uebernehmen (mit den Slot-Anzeigedaten +
// Shuffle/Repeat-Defaults). Damit kein "wo sind meine Tracks"-Schock nach
// dem v0.7.11-Update. Erwartet g_mtx bereits gehalten.
void migrateSlotsToLibrary_() {
    if (!g_library.empty()) return;
    if (g_slots.empty()) return;
    for (const auto& s : g_slots) {
        if (s.spotifyUri.length() == 0) continue;
        bool already = false;
        for (const auto& l : g_library) {
            if (l.uri == s.spotifyUri) { already = true; break; }
        }
        if (already) continue;
        LibraryItem it;
        it.uri        = s.spotifyUri;
        it.name       = s.displayName.length() > 0 ? s.displayName : s.spotifyUri;
        it.shuffle    = s.shuffle;
        it.repeatMode = s.repeatMode;
        g_library.push_back(it);
    }
    if (!g_library.empty()) {
        Serial.printf("[spot] migrated %u slot-uris into empty library\n",
                      (unsigned)g_library.size());
        saveLibraryToNVS_();
    }
}

void loadAuthFromNVS_() {
    if (g_authLoaded) return;
    g_authLoaded = true;
    JsonDocument doc;
    if (!nvsLoadJson(kNvsNs, kNvsKeyAuth, doc)) {
        Serial.println("[spot] no NVS auth-state");
        return;
    }
    g_auth.clientId           = (const char*)(doc["clientId"]           | "");
    g_auth.clientSecret       = (const char*)(doc["clientSecret"]       | "");
    g_auth.accessToken        = (const char*)(doc["accessToken"]        | "");
    g_auth.refreshToken       = (const char*)(doc["refreshToken"]       | "");
    // expiresAtMs aus NVS ist ein millis()-Wert vom letzten Boot — nach Reboot
    // bedeutungslos (millis() ist nicht persistent). Auf 0 setzen erzwingt
    // sofortigen Refresh über refreshToken beim ersten getValidAccessToken().
    g_auth.expiresAtMs        = 0;
    g_auth.spotifyUserId      = (const char*)(doc["spotifyUserId"]      | "");
    g_auth.spotifyDisplayName = (const char*)(doc["spotifyDisplayName"] | "");
    Serial.printf("[spot] loaded auth user=%s — accessToken markiert als expired "
                  "(Boot-Reset), wird beim ersten Use refresht\n",
                  g_auth.spotifyUserId.c_str());
}

void saveAuthToNVS_() {
    JsonDocument doc;
    doc["clientId"]           = g_auth.clientId;
    doc["clientSecret"]       = g_auth.clientSecret;
    doc["accessToken"]        = g_auth.accessToken;
    doc["refreshToken"]       = g_auth.refreshToken;
    doc["expiresAtMs"]        = g_auth.expiresAtMs;
    doc["spotifyUserId"]      = g_auth.spotifyUserId;
    doc["spotifyDisplayName"] = g_auth.spotifyDisplayName;
    nvsSaveJsonWithCleanup(kNvsNs, kNvsKeyAuth, doc);
}

// POST https://accounts.spotify.com/api/token mit grant_type=refresh_token.
// Held-Lock-free: nimmt selbst das Mutex nur kurz fuer lesen/schreiben von
// g_auth, NIE waehrend des HTTPS-Calls. So koennen parallele
// /api/spotify/auth-GETs den Auth-Status ohne 3-8s-Hang lesen.
bool doTokenRefresh_() {
    // Snapshot der benoetigten credentials unter Lock.
    String clientId, clientSecret, refreshToken;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        Serial.println("[spot] refresh: could not acquire mutex");
        return false;
    }
    clientId     = g_auth.clientId;
    clientSecret = g_auth.clientSecret;
    refreshToken = g_auth.refreshToken;
    xSemaphoreGive(g_mtx);

    if (refreshToken.length() == 0 || clientId.length() == 0
        || clientSecret.length() == 0) {
        Serial.println("[spot] refresh skip — no refresh-token/client-id/secret");
        return false;
    }
    ensureTlsClients_();
    if (xSemaphoreTake(g_tlsMtx, pdMS_TO_TICKS(15000)) != pdTRUE) {
        Serial.println("[spot] refresh: tls mutex timeout");
        return false;
    }
    // Cold-Start: nach Reboot ist BearSSL/DNS noch nicht warm — erster
    // HTTPS-Call kann HTTP -1 returnen. Internal-retry mit 500ms delay.
    int  code = -1;
    String resp;
    String basic = "Basic " + base64::encode(clientId + ":" + clientSecret);
    String body  = "grant_type=refresh_token&refresh_token=" + refreshToken;
    for (int attempt = 0; attempt < 3 && code != 200; ++attempt) {
        if (attempt > 0) {
            Serial.printf("[spot] refresh retry %d (last code=%d)\n", attempt, code);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        // Fresh local TLS pro Versuch (vorher: persistente g_tlsAccounts mit
        // setReuse(true)). Empirisch 2026-05-26 auf S3 fuehrte das persistente
        // Pattern zu mbedtls_ssl_setup-Alloc-Fail (-32512) selbst bei freiem
        // Heap > 50KB — Fragmentation der internen RAM. Fresh local + sofortiges
        // http.end() laesst mbedtls die ~32KB SSL-Buffer pro Call frei und neu
        // anfordern; das funktioniert konsistenter als Connection-Reuse.
        WiFiClientSecure tls;
        tls.setInsecure();
        tls.setTimeout(8);
        HTTPClient http;
        if (!http.begin(tls, "https://accounts.spotify.com/api/token")) {
            Serial.println("[spot] refresh begin failed");
            continue;
        }
        http.addHeader("Authorization",  basic);
        http.addHeader("Content-Type",   "application/x-www-form-urlencoded");
        http.setTimeout(8000);
        code = http.POST(body);
        resp = http.getString();
        http.end();
    }
    xSemaphoreGive(g_tlsMtx);
    if (code != 200) {
        Serial.printf("[spot] refresh HTTP %d body=%s\n", code,
                      resp.substring(0, 200).c_str());
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) {
        Serial.println("[spot] refresh JSON parse fail");
        return false;
    }
    String newAccess = (const char*)(doc["access_token"] | "");
    int    expSec    = doc["expires_in"]                 | 3600;
    if (newAccess.length() == 0) {
        Serial.println("[spot] refresh: no access_token in response");
        return false;
    }
    String newRefresh;
    const char* nrPtr = doc["refresh_token"];
    if (nrPtr && strlen(nrPtr) > 0) newRefresh = nrPtr;

    // Result unter Lock zurueckschreiben + persistieren.
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        Serial.println("[spot] refresh: post-mutex acquire failed (state stale)");
        return false;
    }
    g_auth.accessToken = newAccess;
    g_auth.expiresAtMs = (uint64_t)millis() + (uint64_t)expSec * 1000;
    if (newRefresh.length() > 0) {
        g_auth.refreshToken = newRefresh;
        Serial.println("[spot] refresh: refresh_token rotated");
    }
    saveAuthToNVS_();
    xSemaphoreGive(g_mtx);
    Serial.printf("[spot] refresh ok — new expiry in %d s\n", expSec);
    return true;
}

} // anon

// Forward-Declaration für den persistenten Worker.
namespace { void workerTask_(void* arg); }
static QueueHandle_t g_pressQueue = nullptr;

void init() {
    ensureMutex_();
    ensureTlsClients_();   // Eager-init: TLS-Heap-Allokation bei Boot, nicht
                           // im Trigger-Context (vermeidet Fragmentation-Spikes).
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        loadFromNVS_();
        loadLibraryFromNVS_();
        migrateSlotsToLibrary_();
        xSemaphoreGive(g_mtx);
    }
    eventStoreSetPresetPressedCallback(onPresetPressed);

    // Persistenter Worker-Task am Boot starten (statt xTaskCreate-per-press).
    // Empirisch: frisch-erzeugte Tasks haben HTTPS-Connection-Probleme (HTTP -1
    // in 50ms), permanent-laufende Tasks (Marge-keepalive etc.) machen LAN-HTTP
    // ohne Issue. Hypothese: irgendein TLS/lwIP-Init geschieht beim FIRST
    // network access einer Task; one-shot-Tasks haben dafür nicht genug Zeit.
    // Long-lived Worker macht dummy-Init im 1st Cycle.
    if (g_pressQueue == nullptr) {
        g_pressQueue = xQueueCreate(8, sizeof(void*));
#if CONFIG_FREERTOS_UNICORE || !defined(APP_CPU_NUM)
        // Single-core (C3/C6 RISC-V): kein APP_CPU_NUM, nutze PRO_CPU_NUM.
        const BaseType_t targetCore = PRO_CPU_NUM;
#else
        const BaseType_t targetCore = APP_CPU_NUM;
#endif
        xTaskCreatePinnedToCore(
            workerTask_, "spot-worker",
            12288, nullptr, tskIDLE_PRIORITY + 1, nullptr,
            targetCore);
    }

    Serial.println("[spot] init done — preset-pressed callback registered, TLS-clients ready, worker spawned");
}

void setSlot(const String& deviceId, int slot, const String& uri,
             const String& displayName,
             bool shuffle, const String& repeatMode) {
    ensureMutex_();
    if (slot < 1 || slot > 6) return;
    // repeatMode-Validierung an der Grenze, falls jemand Garbage uebergibt.
    String rm = repeatMode;
    if (rm != "off" && rm != "track" && rm != "context") rm = "off";
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return;
    // existing entry replace; if uri empty, delete
    auto it = g_slots.begin();
    while (it != g_slots.end()) {
        if (it->deviceId == deviceId && it->slot == slot) {
            it = g_slots.erase(it);
        } else {
            ++it;
        }
    }
    if (uri.length() > 0) {
        SlotMapping m;
        m.deviceId    = deviceId;
        m.slot        = slot;
        m.spotifyUri  = uri;
        m.displayName = displayName;
        m.shuffle     = shuffle;
        m.repeatMode  = rm;
        g_slots.push_back(m);
    }
    saveToNVS_();
    xSemaphoreGive(g_mtx);
    Serial.printf("[spot] setSlot dev=%s slot=%d uri=%s shuf=%d rep=%s%s\n",
                  deviceId.c_str(), slot,
                  uri.length() > 0 ? uri.c_str() : "(cleared)",
                  shuffle ? 1 : 0, rm.c_str(),
                  uri.length() > 0 ? "" : " (slot will be empty on speaker)");
}

bool getSlot(const String& deviceId, int slot, SlotMapping& out) {
    ensureMutex_();
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    bool found = false;
    for (const auto& m : g_slots) {
        if (m.deviceId == deviceId && m.slot == slot) {
            out = m;
            found = true;
            break;
        }
    }
    xSemaphoreGive(g_mtx);
    return found;
}

std::vector<SlotMapping> listSlots() {
    ensureMutex_();
    std::vector<SlotMapping> copy;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        copy = g_slots;
        xSemaphoreGive(g_mtx);
    }
    return copy;
}

// -----------------------------------------------------------------------------
// Library-API
// -----------------------------------------------------------------------------

bool addLibraryItem(const LibraryItem& item) {
    ensureMutex_();
    if (item.uri.length() == 0) return false;
    String rm = item.repeatMode;
    if (rm != "off" && rm != "track" && rm != "context") rm = "off";
    bool created = true;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    for (auto& l : g_library) {
        if (l.uri == item.uri) {
            l.name       = item.name;
            l.imageUrl   = item.imageUrl;
            l.shuffle    = item.shuffle;
            l.repeatMode = rm;
            created = false;
            break;
        }
    }
    if (created) {
        LibraryItem nit = item;
        nit.repeatMode = rm;
        g_library.push_back(nit);
    }
    saveLibraryToNVS_();
    xSemaphoreGive(g_mtx);
    Serial.printf("[spot] library %s uri=%s name=%s shuf=%d rep=%s\n",
                  created ? "add" : "upd", item.uri.c_str(),
                  item.name.c_str(), item.shuffle ? 1 : 0, rm.c_str());
    return created;
}

bool removeLibraryItem(const String& uri) {
    ensureMutex_();
    if (uri.length() == 0) return false;
    bool removed = false;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    for (auto it = g_library.begin(); it != g_library.end(); ) {
        if (it->uri == uri) {
            it = g_library.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) saveLibraryToNVS_();
    xSemaphoreGive(g_mtx);
    if (removed) Serial.printf("[spot] library rm uri=%s\n", uri.c_str());
    return removed;
}

std::vector<LibraryItem> listLibrary() {
    ensureMutex_();
    std::vector<LibraryItem> copy;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        copy = g_library;
        xSemaphoreGive(g_mtx);
    }
    return copy;
}

bool getLibraryItem(const String& uri, LibraryItem& out) {
    ensureMutex_();
    if (uri.length() == 0) return false;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    bool found = false;
    for (const auto& l : g_library) {
        if (l.uri == uri) { out = l; found = true; break; }
    }
    xSemaphoreGive(g_mtx);
    return found;
}

// -----------------------------------------------------------------------------
// Auth-API
// -----------------------------------------------------------------------------

void setAuth(const AuthState& s) {
    ensureMutex_();
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return;
    g_auth        = s;
    g_authLoaded  = true;
    saveAuthToNVS_();
    xSemaphoreGive(g_mtx);
    Serial.printf("[spot] setAuth user=%s exp_in=%lld s\n",
                  g_auth.spotifyUserId.c_str(),
                  ((int64_t)g_auth.expiresAtMs - (int64_t)millis()) / 1000);
}

AuthState getAuth() {
    ensureMutex_();
    AuthState copy;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        loadAuthFromNVS_();
        copy = g_auth;
        xSemaphoreGive(g_mtx);
    }
    return copy;
}

bool isAuthConfigured() {
    auto s = getAuth();
    return s.clientId.length() > 0 && s.refreshToken.length() > 0;
}

// ----- App-Credentials (separat von Tokens persistiert) ---------------------

void setAppCreds(const AppCreds& c) {
    JsonDocument doc;
    doc["clientId"]     = c.clientId;
    doc["clientSecret"] = c.clientSecret;
    nvsSaveJsonWithCleanup(kNvsNs, kNvsKeyAppCrd, doc);
    Serial.printf("[spot] setAppCreds clientId=%s (len=%u) secret_len=%u\n",
                  c.clientId.c_str(), (unsigned)c.clientId.length(),
                  (unsigned)c.clientSecret.length());
}

AppCreds getAppCreds() {
    AppCreds out;
    JsonDocument doc;
    if (!nvsLoadJson(kNvsNs, kNvsKeyAppCrd, doc)) return out;
    out.clientId     = (const char*)(doc["clientId"]     | "");
    out.clientSecret = (const char*)(doc["clientSecret"] | "");
    return out;
}

bool hasAppCreds() {
    auto c = getAppCreds();
    return c.clientId.length() > 0 && c.clientSecret.length() > 0;
}

String getValidAccessToken() {
    ensureMutex_();
    // Fast-Path: Token ist noch gueltig — Lock kurz auf+zu, keine HTTPS.
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return "";
    loadAuthFromNVS_();
    int64_t remaining = (int64_t)g_auth.expiresAtMs - (int64_t)millis();
    if (g_auth.accessToken.length() > 0 && remaining >= 60 * 1000) {
        String tok = g_auth.accessToken;
        xSemaphoreGive(g_mtx);
        return tok;
    }
    // Slow-Path: refresh. WICHTIG: Lock VOR HTTPS-Call freigeben, sonst
    // hängen alle anderen Spotify-API-Calls (incl. /api/spotify/auth UI-
    // Status) 3-8s waehrend Spotify-Server-Roundtrip.
    xSemaphoreGive(g_mtx);
    if (!doTokenRefresh_()) return "";
    // doTokenRefresh_ acquired/released g_mtx selbst nicht — Lock erneut
    // nehmen fuer den finalen Read von g_auth.accessToken.
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return "";
    String tok = g_auth.accessToken;
    xSemaphoreGive(g_mtx);
    return tok;
}

// -----------------------------------------------------------------------------
// Spotify-Connect Device-Lookup (cached)
// -----------------------------------------------------------------------------

String lookupSpotifyDeviceId(const String& speakerName) {
    if (speakerName.length() == 0) return "";
    ensureMutex_();
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
        auto it = g_devCache.find(speakerName);
        // Nur POSITIVE hits werden gecached. Negative hits ("not in devices")
        // werden NICHT gecached — sonst koennen wir User nicht erkennen wenn
        // er zwischendurch in der Spotify-App manuell pairt.
        if (it != g_devCache.end() && it->second.spotifyDeviceId.length() > 0
            && (millis() - it->second.fetchedMs) < kDevCacheTtlMs) {
            String hit = it->second.spotifyDeviceId;
            xSemaphoreGive(g_mtx);
            Serial.printf("[spot] devCache hit %s -> %s\n",
                          speakerName.c_str(), hit.c_str());
            return hit;
        }
        xSemaphoreGive(g_mtx);
    }

    String tok = getValidAccessToken();
    if (tok.length() == 0) return "";

    // Fresh local WiFiClientSecure pro Call — Singleton g_tlsApi liefert
    // empirisch HTTP -1 unter ungeklärten Bedingungen (vermutlich
    // BearSSL-Internal-State-Corruption), während fresh local immer klappt.
    // Heap-Kost ~30 KB transient pro Call; bei seltenen Presets akzeptabel.
    // Mutex hält serialize damit nicht 2 parallel TLS-Handshakes laufen.
    Serial.printf("[spot/diag] devLookup pre-mtx tok_len=%u wifi_rssi=%d free_heap=%u largest_free_internal=%u\n",
                  (unsigned)tok.length(), WiFi.RSSI(), ESP.getFreeHeap(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (xSemaphoreTake(g_tlsMtx, pdMS_TO_TICKS(15000)) != pdTRUE) return "";
    Serial.println("[spot/diag] mtx acquired");
    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(8);
    HTTPClient http;
    bool beginOk = http.begin(tls, "https://api.spotify.com/v1/me/player/devices");
    Serial.printf("[spot/diag] http.begin -> %d (free_heap=%u)\n", (int)beginOk, ESP.getFreeHeap());
    if (!beginOk) {
        xSemaphoreGive(g_tlsMtx);
        return "";
    }
    http.addHeader("Authorization", "Bearer " + tok);
    http.setTimeout(8000);
    uint32_t t0 = millis();
    int code = http.GET();
    uint32_t dt = millis() - t0;
    Serial.printf("[spot/diag] http.GET -> %d in %u ms (free_heap=%u)\n",
                  code, dt, ESP.getFreeHeap());
    String resp = http.getString();
    Serial.printf("[spot/diag] body_len=%u (first 100 chars: %s)\n",
                  (unsigned)resp.length(), resp.substring(0, 100).c_str());
    http.end();
    xSemaphoreGive(g_tlsMtx);
    if (code != 200) {
        Serial.printf("[spot] /devices HTTP %d body=%s\n", code,
                      resp.substring(0, 200).c_str());
        return "";
    }
    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) {
        Serial.println("[spot] /devices JSON parse fail");
        return "";
    }
    String matchId;
    for (JsonObject d : doc["devices"].as<JsonArray>()) {
        const char* nm = d["name"];
        const char* id = d["id"];
        if (nm && id && speakerName == nm) {
            matchId = id;
            break;
        }
    }
    if (matchId.length() > 0) {
        // Nur positive hits cachen — siehe Kommentar oben.
        if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
            g_devCache[speakerName] = {matchId, millis()};
            xSemaphoreGive(g_mtx);
        }
        Serial.printf("[spot] devLookup %s -> %s (cached %d ms)\n",
                      speakerName.c_str(), matchId.c_str(), kDevCacheTtlMs);
    } else {
        Serial.printf("[spot] devLookup %s -> NOT FOUND. Spotify /devices raw body=%s\n",
                      speakerName.c_str(), resp.substring(0, 600).c_str());
    }
    return matchId;
}

// -----------------------------------------------------------------------------
// Play-Trigger
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// URL-Encoding (RFC3986 unreserved: A-Z a-z 0-9 - _ . ~)
// -----------------------------------------------------------------------------

namespace {
String urlEncode_(const String& in) {
    static const char* hex = "0123456789ABCDEF";
    String out;
    out.reserve(in.length() * 3);
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}
} // anon

// -----------------------------------------------------------------------------
// OAuth Authorization-Code Flow
// -----------------------------------------------------------------------------

String genAuthUrl(const String& clientId, const String& redirectUri,
                  const String& state) {
    String url = "https://accounts.spotify.com/authorize?";
    url += "client_id=" + urlEncode_(clientId);
    url += "&response_type=code";
    url += "&redirect_uri=" + urlEncode_(redirectUri);
    if (state.length() > 0) {
        url += "&state=" + urlEncode_(state);
    }
    url += "&scope=" + urlEncode_(
        "user-read-playback-state user-modify-playback-state "
        "user-read-currently-playing user-read-email");
    return url;
}

bool fetchUserInfo(String& userIdOut, String& displayNameOut) {
    String tok = getValidAccessToken();
    if (tok.length() == 0) return false;
    if (xSemaphoreTake(g_tlsMtx, pdMS_TO_TICKS(15000)) != pdTRUE) return false;
    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(8);
    HTTPClient http;
    if (!http.begin(tls, "https://api.spotify.com/v1/me")) {
        xSemaphoreGive(g_tlsMtx);
        return false;
    }
    http.addHeader("Authorization", "Bearer " + tok);
    http.setTimeout(8000);
    int code = http.GET();
    String resp = http.getString();
    http.end();
    xSemaphoreGive(g_tlsMtx);
    if (code != 200) {
        Serial.printf("[spot] /v1/me HTTP %d body=%s\n", code,
                      resp.substring(0, 200).c_str());
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) return false;
    userIdOut      = (const char*)(doc["id"]           | "");
    displayNameOut = (const char*)(doc["display_name"] | "");
    return userIdOut.length() > 0;
}

bool exchangeAuthCode(const String& code,
                      const String& clientId,
                      const String& clientSecret,
                      const String& redirectUri,
                      String& errorOut) {
    if (code.length() == 0 || clientId.length() == 0
        || clientSecret.length() == 0 || redirectUri.length() == 0) {
        errorOut = "missing-param";
        return false;
    }
    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(8);
    HTTPClient http;
    if (!http.begin(tls, "https://accounts.spotify.com/api/token")) {
        errorOut = "begin-failed";
        return false;
    }
    String basic = "Basic " + base64::encode(clientId + ":" + clientSecret);
    http.addHeader("Authorization", basic);
    http.addHeader("Content-Type",  "application/x-www-form-urlencoded");
    http.setTimeout(8000);
    String body = "grant_type=authorization_code";
    body += "&code=" + urlEncode_(code);
    body += "&redirect_uri=" + urlEncode_(redirectUri);
    int hc = http.POST(body);
    String resp = http.getString();
    http.end();
    if (hc != 200) {
        errorOut = "HTTP-" + String(hc) + ":" + resp.substring(0, 200);
        Serial.printf("[spot] exchange HTTP %d body=%s\n", hc,
                      resp.substring(0, 200).c_str());
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) {
        errorOut = "json-parse";
        return false;
    }
    String access  = (const char*)(doc["access_token"]  | "");
    String refresh = (const char*)(doc["refresh_token"] | "");
    int    expSec  = doc["expires_in"]                  | 3600;
    if (access.length() == 0 || refresh.length() == 0) {
        errorOut = "no-tokens-in-response";
        return false;
    }
    AuthState s;
    s.clientId      = clientId;
    s.clientSecret  = clientSecret;
    s.accessToken   = access;
    s.refreshToken  = refresh;
    s.expiresAtMs   = (uint64_t)millis() + (uint64_t)expSec * 1000;
    // App-Creds parallel persistieren — überleben Disconnect.
    AppCreds appc;
    appc.clientId     = clientId;
    appc.clientSecret = clientSecret;
    setAppCreds(appc);
    // /v1/me Fetch — Best-Effort, fail-non-fatal.
    setAuth(s);
    String userId, displayName;
    if (fetchUserInfo(userId, displayName)) {
        s.spotifyUserId      = userId;
        s.spotifyDisplayName = displayName;
        setAuth(s);   // re-persist mit User-Info
        Serial.printf("[spot] exchange ok user=%s name=\"%s\" exp_in=%d\n",
                      userId.c_str(), displayName.c_str(), expSec);
    } else {
        Serial.printf("[spot] exchange ok (no user info) exp_in=%d\n", expSec);
    }
    return true;
}

std::vector<TriggerLogEntry> listTriggerLog() {
    std::vector<TriggerLogEntry> out;
    out.reserve(g_trigLogCount);
    // Oldest first. head points to next-write, so oldest is at
    // (head - count) mod len.
    size_t start = (g_trigLogHead + kTrigLogLen - g_trigLogCount) % kTrigLogLen;
    for (size_t i = 0; i < g_trigLogCount; ++i) {
        out.push_back(g_trigLog[(start + i) % kTrigLogLen]);
    }
    return out;
}

void refreshTick() {
    static uint32_t lastTick = 0;
    if (millis() - lastTick < 60000) return;
    lastTick = millis();
    if (!isAuthConfigured()) return;
    auto s = getAuth();
    int64_t remaining = (int64_t)s.expiresAtMs - (int64_t)millis();
    // Wenn weniger als 5min Restlaufzeit → proaktiv refresh (intern).
    if (remaining < 5 * 60 * 1000) {
        Serial.printf("[spot] refreshTick — proactive refresh (remaining=%lld s)\n",
                      remaining / 1000);
        (void)getValidAccessToken();
    }
}

bool playOnDevice(const String& spotifyDeviceId, const String& contextUri) {
    if (spotifyDeviceId.length() == 0 || contextUri.length() == 0) return false;
    String tok = getValidAccessToken();
    if (tok.length() == 0) return false;

    // Fresh local TLS pro Call — siehe lookupSpotifyDeviceId-Kommentar.
    if (xSemaphoreTake(g_tlsMtx, pdMS_TO_TICKS(15000)) != pdTRUE) return false;
    String url = "https://api.spotify.com/v1/me/player/play?device_id=" + spotifyDeviceId;
    String body;
    if (contextUri.startsWith("spotify:track:")) {
        body = "{\"uris\":[\"" + contextUri + "\"]}";
    } else {
        body = "{\"context_uri\":\"" + contextUri + "\"}";
    }
    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(8);
    HTTPClient http;
    if (!http.begin(tls, url)) {
        xSemaphoreGive(g_tlsMtx);
        return false;
    }
    http.addHeader("Authorization", "Bearer " + tok);
    http.addHeader("Content-Type",  "application/json");
    http.setTimeout(8000);
    int code = http.PUT(body);
    bool ok = (code == 204 || code == 202);
    String resp = ok ? String() : http.getString();
    http.end();
    xSemaphoreGive(g_tlsMtx);
    if (ok) {
        Serial.printf("[spot] /play OK (HTTP %d) device=%s uri=%s\n",
                      code, spotifyDeviceId.c_str(), contextUri.c_str());
        return true;
    }
    Serial.printf("[spot] /play HTTP %d body=%s\n", code,
                  resp.substring(0, 300).c_str());
    return false;
}

// -----------------------------------------------------------------------------
// onPresetPressed — der Web-API-Trigger
// -----------------------------------------------------------------------------
//
// WICHTIG: dieser Callback laeuft im AsyncWebServer-Event-Loop (event_store
// ingestBody ruft uns aus dem POST-/v1/scmudc/...-Handler heraus). Wenn wir
// hier direkt HTTPS-Calls zu Spotify machen (token-refresh + /devices + /play,
// 3-8s realistisch), blockt der gesamte HTTP-Server. → wir spawnen einen
// kurzlebigen FreeRTOS-Task der den Spotify-Pfad off-loop ausfuehrt.

namespace {
struct PressJob {
    String deviceId;
    int    slot;
    String origin;
};

void triggerWorker_(const String& deviceId, int slot, const String& origin) {
    TriggerLogEntry log;
    log.ts_ms    = millis();
    log.deviceId = deviceId;
    log.slot     = slot;

    SlotMapping m;
    if (!getSlot(deviceId, slot, m)) {
        log.result = "no-mapping";
        pushTrigLog_(log);
        return;
    }
    log.uri = m.spotifyUri;

    if (!isAuthConfigured()) {
        Serial.printf("[spot][TRIGGER] dev=%s slot=%d uri=%s — NO AUTH\n",
                      deviceId.c_str(), slot, m.spotifyUri.c_str());
        log.result = "no-auth";
        pushTrigLog_(log);
        return;
    }

    String speakerName;
    auto& inv = SpeakerInventory::instance();
    {
        SpeakerInventory::LockGuard g(inv);
        Speaker* sp = inv.findById(deviceId);
        if (sp) speakerName = sp->name;
    }
    if (speakerName.length() == 0) {
        Serial.printf("[spot][TRIGGER] dev=%s slot=%d — speaker not in inventory\n",
                      deviceId.c_str(), slot);
        log.result = "not-in-inventory";
        pushTrigLog_(log);
        return;
    }
    log.speakerName = speakerName;
    Serial.printf("[spot][TRIGGER] dev=%s name=%s slot=%d origin=%s uri=%s\n",
                  deviceId.c_str(), speakerName.c_str(), slot, origin.c_str(),
                  m.spotifyUri.c_str());

    String spotifyDeviceId = lookupSpotifyDeviceId(speakerName);
    if (spotifyDeviceId.length() == 0) {
        // Race: Bose-Slot fire (TUNEIN etc.) hat die Spotify-Connect-Session
        // gerade beendet, Speaker fliegt fuer 1-2s aus /devices. Einmal mit
        // Delay erneut versuchen.
        Serial.printf("[spot][TRIGGER] '%s' not yet in /devices — retry in 2s\n",
                      speakerName.c_str());
        vTaskDelay(pdMS_TO_TICKS(2000));
        spotifyDeviceId = lookupSpotifyDeviceId(speakerName);
    }
    if (spotifyDeviceId.length() == 0) {
        Serial.printf("[spot][TRIGGER] '%s' not visible to Spotify after retry — give up\n",
                      speakerName.c_str());
        log.result = "not-in-devices";
        pushTrigLog_(log);
        return;
    }
    log.spotifyDeviceId = spotifyDeviceId;

    // Inline der /play-Call damit wir den HTTP-Code in den Log bekommen.
    String tok = getValidAccessToken();
    if (tok.length() == 0) {
        log.result = "no-token";
        pushTrigLog_(log);
        return;
    }

    // Lambda fuer einfache PUT-Calls (shuffle, repeat) — kein body, kein
    // response-handling ausser HTTP-Code. Nutzt fresh local TLS pro Call wie
    // /play unten. Macht die Pre-Play-Modi sequenziell innerhalb des g_tlsMtx
    // Locks gleich nebenan.
    auto putWebApi = [&](const String& url) -> int {
        WiFiClientSecure tls;
        tls.setInsecure();
        tls.setTimeout(6);
        HTTPClient http;
        if (!http.begin(tls, url)) return -1;
        http.addHeader("Authorization", "Bearer " + tok);
        http.addHeader("Content-Length", "0");
        http.setTimeout(6000);
        int c = http.PUT("");
        http.end();
        return c;
    };

    if (xSemaphoreTake(g_tlsMtx, pdMS_TO_TICKS(15000)) != pdTRUE) {
        log.result = "tls-busy";
        pushTrigLog_(log);
        return;
    }
    // Pre-Play: shuffle + repeat setzen. Spotify uebernimmt sonst den vorigen
    // Mode des Players, was bei einem "Pink-Floyd-Album"-Slot mit aktivem
    // shuffle vom letzten Slot zu durcheinandergeworfener Track-Order fuehrt.
    // Beide Calls sind ~200ms each. Fehler werden geloggt aber nicht abgebrochen
    // — der /play unten ist die einzig wirklich kritische Operation.
    {
        String shuf = "https://api.spotify.com/v1/me/player/shuffle?state=";
        shuf += (m.shuffle ? "true" : "false");
        shuf += "&device_id=" + spotifyDeviceId;
        int sc = putWebApi(shuf);
        if (sc != 204 && sc != 202) {
            Serial.printf("[spot] /shuffle HTTP %d (continuing)\n", sc);
        }
        String rep = "https://api.spotify.com/v1/me/player/repeat?state=";
        rep += m.repeatMode;  // "off"|"track"|"context", validiert in setSlot
        rep += "&device_id=" + spotifyDeviceId;
        int rc = putWebApi(rep);
        if (rc != 204 && rc != 202) {
            Serial.printf("[spot] /repeat HTTP %d (continuing)\n", rc);
        }
    }

    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(8);
    HTTPClient http;
    String url = "https://api.spotify.com/v1/me/player/play?device_id=" + spotifyDeviceId;
    if (!http.begin(tls, url)) {
        xSemaphoreGive(g_tlsMtx);
        log.result = "play-begin-failed";
        pushTrigLog_(log);
        return;
    }
    http.addHeader("Authorization", "Bearer " + tok);
    http.addHeader("Content-Type",  "application/json");
    http.setTimeout(8000);
    String body;
    if (m.spotifyUri.startsWith("spotify:track:")) {
        body = "{\"uris\":[\"" + m.spotifyUri + "\"]}";
    } else {
        body = "{\"context_uri\":\"" + m.spotifyUri + "\"}";
    }
    int code = http.PUT(body);
    log.playHttpCode = code;
    if (code == 204 || code == 202) {
        http.end();
        xSemaphoreGive(g_tlsMtx);
        log.result = "ok";
        Serial.printf("[spot] /play OK (HTTP %d) device=%s uri=%s\n",
                      code, spotifyDeviceId.c_str(), m.spotifyUri.c_str());
    } else {
        String resp = http.getString();
        http.end();
        xSemaphoreGive(g_tlsMtx);
        log.result = String("play-failed:") + resp.substring(0, 80);
        Serial.printf("[spot] /play HTTP %d body=%s\n", code,
                      resp.substring(0, 200).c_str());
    }
    pushTrigLog_(log);
}

void triggerTaskEntry_(void* arg) {
    PressJob* job = static_cast<PressJob*>(arg);
    triggerWorker_(job->deviceId, job->slot, job->origin);
    delete job;
    vTaskDelete(nullptr);
}

// Long-lived Worker: hängt an g_pressQueue, dequeues Jobs sequenziell.
// Vorteil: TLS/lwIP-Init nur 1× beim Task-Boot, nicht per Press.
void workerTask_(void* /*arg*/) {
    Serial.println("[spot/worker] running");
    while (true) {
        PressJob* job = nullptr;
        if (xQueueReceive(g_pressQueue, &job, portMAX_DELAY) == pdTRUE && job) {
            triggerWorker_(job->deviceId, job->slot, job->origin);
            delete job;
        }
    }
}
} // anon

void onPresetPressed(const String& deviceId, int slot, const String& origin) {
    // Dispatch via localhost-HTTP-Loopback ans UI-Server (Port 80) damit der
    // Trigger im UI-AsyncWebServer-handler-Thread läuft. Empirisch verifiziert:
    //   • UI-handler-Thread: HTTPS klappt (probe-api, simulate)
    //   • BMX-SCMUDC-handler-Thread: HTTPS schlägt fehl (refresh -1, devices -1)
    //   • xTaskCreate-Tasks: HTTPS schlägt fehl (alle Versuche)
    // → UI-Handler ist der einzig funktionierende Context. Plain-HTTP-POST zu
    // localhost dispatcht synchron, blockiert BMX-handler nur 1-5ms (LAN-HTTP).
    // UI-handler übernimmt dann den HTTPS-Trigger.
    if (!isAuthConfigured()) {
        Serial.printf("[spot][TRIGGER] dev=%s slot=%d (origin=%s) — no auth, skip\n",
                      deviceId.c_str(), slot, origin.c_str());
        return;
    }
    // Wenn wir bereits aus dem UI-Handler-Context kommen (simulate-Endpoint),
    // direkt inline triggern — Dispatch wäre Recursion.
    if (origin == "ui-simulate") {
        triggerWorker_(deviceId, slot, origin);
        return;
    }
    // Aus BMX-SCMUDC-Handler-Context: HTTPS scheitert hier (siehe Diag).
    // Dispatch via localhost-HTTP-Loopback an UI-Server damit Trigger im
    // UI-AsyncWebServer-handler-Thread läuft (dort funktioniert HTTPS).
    HTTPClient http;
    String url = "http://127.0.0.1/api/spotify/simulate/" + deviceId + "/" + String(slot);
    if (!http.begin(url)) {
        Serial.println("[spot] dispatch http.begin fail");
        return;
    }
    http.setTimeout(3000);
    int code = http.POST("");
    http.end();
    Serial.printf("[spot] dispatch %s slot=%d -> HTTP %d\n",
                  deviceId.c_str(), slot, code);
}

} // namespace spotify
} // namespace sixback

#else // SIXBACK_SPOTIFY_ENABLED

// No-op stubs damit api_endpoints/main weiterhin gegen die Header linken
// kann. Auf C3/C6/ESP32-classic ist Spotify-Trigger bewusst deaktiviert
// (zu wenig Heap fuer WiFiClientSecure + Token-Refresh).

#include <Arduino.h>

namespace sixback {
namespace spotify {

void init() {
    Serial.println("[spot] disabled in this build (SIXBACK_SPOTIFY_ENABLED undef)");
}
void setSlot(const String&, int, const String&, const String&,
             bool, const String&) {}
bool getSlot(const String&, int, SlotMapping&) { return false; }
std::vector<SlotMapping> listSlots() { return {}; }
bool addLibraryItem(const LibraryItem&) { return false; }
bool removeLibraryItem(const String&) { return false; }
std::vector<LibraryItem> listLibrary() { return {}; }
bool getLibraryItem(const String&, LibraryItem&) { return false; }
void onPresetPressed(const String&, int, const String&) {}
void setAuth(const AuthState&) {}
AuthState getAuth() { return {}; }
bool isAuthConfigured() { return false; }
String getValidAccessToken() { return ""; }
String lookupSpotifyDeviceId(const String&) { return ""; }
bool playOnDevice(const String&, const String&) { return false; }
String genAuthUrl(const String&, const String&, const String&) { return ""; }
bool exchangeAuthCode(const String&, const String&, const String&,
                      const String&, String& err) { err = "spotify-disabled"; return false; }
bool fetchUserInfo(String&, String&) { return false; }
void refreshTick() {}
std::vector<TriggerLogEntry> listTriggerLog() { return {}; }

} // namespace spotify
} // namespace sixback

#endif // SIXBACK_SPOTIFY_ENABLED
