// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "nvs_helper.h"
#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace sixback {

bool nvsLoadJson(const char* ns, const char* key, JsonDocument& doc) {
    Preferences p;
    if (!p.begin(ns, true)) return false;
    String s = p.getString(key, "");
    p.end();
    if (s.length() == 0) return false;
    return deserializeJson(doc, s) == DeserializationError::Ok;
}

bool nvsSaveJson(const char* ns, const char* key, JsonDocument& doc) {
    String s;
    serializeJson(doc, s);
    Preferences p;
    if (!p.begin(ns, false)) return false;
    size_t n = p.putString(key, s);
    p.end();
    return n > 0;
}

bool nvsErase(const char* ns, const char* key) {
    Preferences p;
    if (!p.begin(ns, false)) return false;
    bool ok = p.remove(key);
    p.end();
    return ok;
}

namespace {

// Liefert true wenn der Namespace mindestens einen Eintrag hat.
bool nvsNamespaceHasEntries(const char* ns) {
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", ns, NVS_TYPE_ANY, &it);
    if (err != ESP_OK || it == nullptr) {
        if (it) nvs_release_iterator(it);
        return false;
    }
    nvs_release_iterator(it);
    return true;
}

// Kopiert einen einzelnen Eintrag basierend auf seinem Typ. Gibt true bei
// Erfolg. Auf Fehler wird der Eintrag uebersprungen und false zurueckgegeben.
bool copyEntry(nvs_handle_t src, nvs_handle_t dst, const nvs_entry_info_t& info) {
    switch (info.type) {
        case NVS_TYPE_U8:  { uint8_t  v; if (nvs_get_u8 (src, info.key, &v) == ESP_OK) return nvs_set_u8 (dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I8:  { int8_t   v; if (nvs_get_i8 (src, info.key, &v) == ESP_OK) return nvs_set_i8 (dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U16: { uint16_t v; if (nvs_get_u16(src, info.key, &v) == ESP_OK) return nvs_set_u16(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I16: { int16_t  v; if (nvs_get_i16(src, info.key, &v) == ESP_OK) return nvs_set_i16(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U32: { uint32_t v; if (nvs_get_u32(src, info.key, &v) == ESP_OK) return nvs_set_u32(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I32: { int32_t  v; if (nvs_get_i32(src, info.key, &v) == ESP_OK) return nvs_set_i32(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U64: { uint64_t v; if (nvs_get_u64(src, info.key, &v) == ESP_OK) return nvs_set_u64(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I64: { int64_t  v; if (nvs_get_i64(src, info.key, &v) == ESP_OK) return nvs_set_i64(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_STR: {
            size_t len = 0;
            if (nvs_get_str(src, info.key, nullptr, &len) != ESP_OK || len == 0) return false;
            char* tmp = (char*)malloc(len);
            if (!tmp) return false;
            bool ok = nvs_get_str(src, info.key, tmp, &len) == ESP_OK
                   && nvs_set_str(dst, info.key, tmp) == ESP_OK;
            free(tmp);
            return ok;
        }
        case NVS_TYPE_BLOB: {
            size_t len = 0;
            if (nvs_get_blob(src, info.key, nullptr, &len) != ESP_OK || len == 0) return false;
            uint8_t* tmp = (uint8_t*)malloc(len);
            if (!tmp) return false;
            bool ok = nvs_get_blob(src, info.key, tmp, &len) == ESP_OK
                   && nvs_set_blob(dst, info.key, tmp, len) == ESP_OK;
            free(tmp);
            return ok;
        }
        default:
            return false;
    }
    return false;
}

} // namespace

bool migrateNvsNamespace(const char* oldNs, const char* newNs) {
    // No-op wenn alte Daten gar nicht da sind.
    if (!nvsNamespaceHasEntries(oldNs)) return true;

    // Idempotent: wenn neue Namespace schon Daten hat, gilt Migration als
    // abgeschlossen. Wir loeschen die alte aber NICHT — Sicherheits-Backup
    // bleibt einen Boot lang erhalten falls beim Schreiben in neue NS was
    // schiefging und der naechste Boot retry will. Erst beim 2. Boot mit
    // sauberer neuer NS wird die alte tot.
    if (nvsNamespaceHasEntries(newNs)) {
        // Beide vorhanden: alte ist Cruft, wegputzen.
        nvs_handle_t h;
        if (nvs_open(oldNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
            Serial.printf("[nvs-migrate] %s already migrated, erased leftover\n", oldNs);
        }
        return true;
    }

    nvs_handle_t src = 0, dst = 0;
    if (nvs_open(oldNs, NVS_READWRITE, &src) != ESP_OK) {
        Serial.printf("[nvs-migrate] open %s failed\n", oldNs);
        return false;
    }
    if (nvs_open(newNs, NVS_READWRITE, &dst) != ESP_OK) {
        Serial.printf("[nvs-migrate] open %s failed\n", newNs);
        nvs_close(src);
        return false;
    }

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", oldNs, NVS_TYPE_ANY, &it);
    int copied = 0, failed = 0;
    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        if (copyEntry(src, dst, info)) {
            copied++;
        } else {
            failed++;
            Serial.printf("[nvs-migrate] copy fail %s/%s (type=%d)\n",
                          oldNs, info.key, (int)info.type);
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);

    if (failed == 0 && copied > 0) {
        nvs_commit(dst);
        nvs_erase_all(src);
        nvs_commit(src);
        Serial.printf("[nvs-migrate] %s -> %s (%d keys)\n", oldNs, newNs, copied);
    } else if (copied > 0) {
        nvs_commit(dst);
        Serial.printf("[nvs-migrate] %s -> %s partial (%d ok, %d fail, OLD KEPT)\n",
                      oldNs, newNs, copied, failed);
    }
    nvs_close(src);
    nvs_close(dst);
    return failed == 0;
}

void migrateAllBosefixNvs() {
    static const struct { const char* oldNs; const char* newNs; } map[] = {
        { "bosefix-wifi", "sixback-wifi" },  // ZUERST: WiFi-Creds entscheiden Live-Status
        { "bosefix-pre",  "sixback-pre"  },  // User-Presets
        { "bosefix-inv",  "sixback-inv"  },  // Speaker-Inventory
        { "bosefix-auto", "sixback-auto" },  // Auto-Mode-Settings
        { "bosefix-net",  "sixback-net"  },  // IP-Failsafe
        { "bosefix-sys",  "sixback-sys"  },  // Health-Counters
        { "bosefix-tune", "sixback-tune" },  // TuneIn-Cache (unkritisch)
    };
    for (auto& m : map) {
        migrateNvsNamespace(m.oldNs, m.newNs);
    }
}

} // namespace sixback
