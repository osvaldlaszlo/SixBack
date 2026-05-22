// SPDX-License-Identifier: GPL-3.0-or-later
// SixBack — NVS-Helper (JSON-Persistenz)
#ifndef SIXBACK_NVS_HELPER_H
#define SIXBACK_NVS_HELPER_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace sixback {

// Liest ein NVS-key (string) und parsed JSON in 'doc'. Gibt false zurueck wenn
// nicht da oder Parse-Fehler.
bool nvsLoadJson(const char* ns, const char* key, JsonDocument& doc);

// Serialisiert 'doc' und schreibt unter ns/key. Bei zu grossem Payload
// erweitert Preferences automatisch (Limit ~ 4KB pro String).
bool nvsSaveJson(const char* ns, const char* key, JsonDocument& doc);

// Loescht einen Key.
bool nvsErase(const char* ns, const char* key);

// Einmalige Daten-Migration BoseFix32 -> SixBack.
// Kopiert alle Keys (STR, U8, U16, U32, U64, I8, I16, I32, I64, BLOB) von
// `oldNs` nach `newNs`, falls `newNs` noch leer ist. Nach erfolgreicher
// Migration wird `oldNs` komplett geloescht. No-op wenn newNs schon Daten
// hat oder oldNs leer ist.
//
// MUSS in setup() VOR jedem loadFromNVS()-Aufruf passieren, sonst geht
// User-Konfig beim Rename-OTA-Update verloren (WiFi-Creds, Presets, Inv).
// Gibt true zurueck wenn migriert oder schon migriert; false bei Fehler.
bool migrateNvsNamespace(const char* oldNs, const char* newNs);

// Convenience: ruft migrateNvsNamespace fuer alle 7 BoseFix32-Namespaces
// und ihre SixBack-Pendants auf. Loggt Status pro Namespace.
void migrateAllBosefixNvs();

} // namespace sixback

#endif
