// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — NVS-Helper (JSON-Persistenz)
#ifndef BOSEFIX32_NVS_HELPER_H
#define BOSEFIX32_NVS_HELPER_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace bosefix {

// Liest ein NVS-key (string) und parsed JSON in 'doc'. Gibt false zurueck wenn
// nicht da oder Parse-Fehler.
bool nvsLoadJson(const char* ns, const char* key, JsonDocument& doc);

// Serialisiert 'doc' und schreibt unter ns/key. Bei zu grossem Payload
// erweitert Preferences automatisch (Limit ~ 4KB pro String).
bool nvsSaveJson(const char* ns, const char* key, JsonDocument& doc);

// Loescht einen Key.
bool nvsErase(const char* ns, const char* key);

} // namespace bosefix

#endif
