// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Online-Update (HTTPS-Pull von sixback.io)
//
// Im Geist von tul-knx-gateway: das Webfrontend triggert
// /api/update/check und /api/update/install; der ESP zieht manifest.json
// + chip-spezifisches firmware.bin selbst und flasht es ueber die
// arduino-esp32 Update-API in den zweiten OTA-Slot. Polling per
// /api/update/status.
//
// Anti-Brick: bei jedem Schreibfehler bleibt der laufende Slot aktiv,
// der bootloader faellt auf app0 zurueck wenn app1 nicht bestaetigt
// wird.
#ifndef BOSEFIX32_OTA_PULL_H
#define BOSEFIX32_OTA_PULL_H

#include <Arduino.h>

namespace sixback {
namespace ota {

enum class State : uint8_t {
    IDLE       = 0,
    CHECKING   = 1,
    AVAILABLE  = 2,
    INSTALLING = 3,
    DONE       = 4,
    ERROR_     = 5,  // _ weil ERROR makro-kollision mit log.h
};

struct Status {
    State    state    = State::IDLE;
    String   latest;     // Version aus manifest.json (z.B. "0.5.372")
    String   current;    // Eigene Version (z.B. "0.5.379")
    String   error;
    uint32_t progress = 0;  // Bytes der aktuellen Phase geflasht
    uint32_t total    = 0;  // Bytes der aktuellen Phase erwartet
    String   phase;         // "fs" | "fw" | "" — fuer UI-Anzeige
    uint8_t  phaseIdx = 0;  // 1 = waehrend FS, 2 = waehrend FW
    uint8_t  phaseN   = 2;  // Anzahl Phasen total
};

// Liefert den letzten Status (Mutex-protected snapshot).
Status getStatus();

// Triggert Manifest-Pull. Synchron + schnell (<2s), kann direkt vom
// Web-Handler aufgerufen werden. Setzt state auf CHECKING->AVAILABLE
// (oder IDLE wenn schon aktuell) / ERROR.
void checkOnline();

// Startet das eigentliche Install im Hintergrund-Task. Liefert sofort
// zurueck; UI muss /api/update/status pollen. Nur erlaubt wenn state
// == AVAILABLE.
bool installOnlineAsync();

// Force-Variante: ignoriert state==AVAILABLE-Check. Sinnvoll fuer
// Re-Install (gleicher Version) oder Demo des Progress-Bars wenn das
// Repo absichtlich keine neuere Version hat.
bool installOnlineForceAsync();

// Init — nutzbar in setup(). Setzt Mutex und Default-Status.
void init(const String& myVersion);

} // namespace ota
} // namespace sixback

#endif
