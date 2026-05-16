// SPDX-License-Identifier: GPL-3.0-or-later
#include "nvs_helper.h"
#include <Preferences.h>

namespace bosefix {

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

} // namespace bosefix
