// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — NVS-Helper (JSON-Persistenz)
#ifndef SIXBACK_NVS_HELPER_H
#define SIXBACK_NVS_HELPER_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace sixback {

// Liest ein NVS-key und parsed JSON in 'doc'. Erkennt drei Generationen
// transparent: Legacy-STRING (<= v0.8.14), Klartext-JSON-Blob (v0.8.15/16)
// und heatshrink-komprimiertes Blob ("HS"-Frame, seit der Kompressions-
// Stufe). Aeltere Formate werden beim naechsten Save in-place ersetzt.
// Gibt false zurueck wenn nicht da, Decode- oder Parse-Fehler.
bool nvsLoadJson(const char* ns, const char* key, JsonDocument& doc);

// Serialisiert 'doc' und schreibt als BLOB unter ns/key; Werte >= 512 B
// werden heatshrink-komprimiert (w=8/l=4, ~1,6 KB transienter Heap,
// Faktor ~2-3,6 auf Store-JSON), kleinere bleiben Klartext. Blob statt
// String, weil nvs_set_str hart bei 4000 B endet — das vernichtete ab
// ~5 Speakern jeden Save (Lab-Befund 2026-06-07).
bool nvsSaveJson(const char* ns, const char* key, JsonDocument& doc);

// Loescht einen Key.
bool nvsErase(const char* ns, const char* key);

// Eraset ALLE keys eines Namespaces.
bool nvsEraseAllInNamespace(const char* ns);

// Liefert NVS-Stats fuer die Default-Partition als JSON.
void nvsGetStatsJson(JsonDocument& out);

// Try-Save mit Auto-Cleanup-Fallback. Wenn der Blob-Write fehlschlaegt
// (NVS wirklich voll), werden Cache-Namespaces erased + retried. Pass 3
// sichert den alten Wert und stellt ihn bei erneutem Fehlschlag wieder
// her — vernichtet NIE den letzten guten Stand. Returns true bei Erfolg.
bool nvsSaveJsonWithCleanup(const char* ns, const char* key, JsonDocument& doc);

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
