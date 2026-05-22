// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Source-Normalizer
//
// Wandelt ein vom Speaker geliefertes ContentItem in einen Preset um, der von
// SixBack nach Migration spielbar ist:
//   TUNEIN / LOCAL_INTERNET_RADIO / INTERNET_RADIO  →  1:1 uebernommen
//   RADIO_BROWSER  →  UUID via radio-browser.info aufgeloest, als LOCAL gespeichert
//   alles andere   →  ABANDONED (Slot kann nach Migration nicht spielen)
//
// Hintergrund: SixBack deklariert in bmx_services.json RADIO_BROWSER als
// available; der Speaker meldet die Source als READY und akzeptiert /select.
// Es fehlen aber die /bmx/radiobrowser/v1/...-Resolver-Routen → Playback
// landet in INVALID_SOURCE. Verifiziert am Kuechen-Speaker 2026-05-17.
#ifndef BOSEFIX32_SOURCE_NORMALIZER_H
#define BOSEFIX32_SOURCE_NORMALIZER_H

#include <Arduino.h>
#include "preset_store.h"

namespace sixback {

enum class NormalizeStatus : uint8_t {
    OK_PASSTHROUGH = 0,  // Source nativ supported, 1:1 uebernommen
    OK_CONVERTED   = 1,  // Source wurde konvertiert (RADIO_BROWSER → LOCAL)
    ABANDONED      = 2,  // (legacy — wird nicht mehr produziert; bleibt im enum
                         //          fuer Kompatibilitaet mit alten NVS-Snapshots)
    OK_OPAQUE      = 3,  // Unbekannte Source. out.source==OPAQUE; Caller MUSS
                         // out.rawContentItem + out.opaqueSourceName selbst
                         // setzen (Normalizer kennt das XML nicht).
};

struct NormalizeResult {
    NormalizeStatus status;
    String          reason;          // human-readable, fuer UI/Log
    String          originalSource;  // "TUNEIN" / "RADIO_BROWSER" / "SPOTIFY" / ...
};

// Baut einen Preset aus den im Speaker-XML stehenden Rohdaten. Setzt
// out.slot NICHT (Caller kennt den Slot, der Normalisierer nur die Source).
// Bei ABANDONED bleibt out unveraendert ausser .slot — Caller darf das Preset
// dann nicht in den PresetStore schreiben.
NormalizeResult normalizePreset(const String& sourceStr,
                                const String& location,
                                const String& itemName,
                                const String& imageUrl,
                                Preset&       out);

const char* normalizeStatusToStr(NormalizeStatus s);

} // namespace sixback

#endif
