// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Speaker-Bootstrap via Telnet Port 17000 (Bose Diag Shell)
//
// Diese Funktionen führen die Cloud-URL-Migration am Speaker durch.
// Nutzt die Bose-eigene Diagnostic-Shell (KEINE Linux-Shell — siehe
// /Public/CLAUDE/BOSE/docs/RESEARCH.md §12).
//
// Phase 0: nicht aktiv (manuelles Migrieren reicht für PoC).
// Phase 3: vollständig implementiert mit Wizard-UI-Anbindung.
#ifndef BOSEFIX32_SPEAKER_TELNET_H
#define BOSEFIX32_SPEAKER_TELNET_H

#include <Arduino.h>

struct MigrationResult {
    bool ok;
    String message;
    String verifiedConfig;  // Antwort vom getpdo CurrentSystemConfiguration
};

// Patcht die Cloud-URLs am Speaker auf die übergebene Base-URL.
// Beispiel: serverBaseUrl="http://192.168.1.42:8000"
// Führt am Ende `sys reboot` aus.
MigrationResult migrateSpeaker(const String& speakerIP,
                                const String& serverBaseUrl);

// Rückgängig-Machung: setzt die Cloud-URLs am Speaker auf die originalen
// Bose-Endpoints zurück (https://streaming.bose.com usw.) + Reboot.
// Speaker funktioniert danach wieder offiziell (auch wenn Bose-Cloud tot ist,
// fallen die Local-Features wieder darauf zurück).
MigrationResult revertSpeaker(const String& speakerIP);

// Sendet sys reboot per Telnet:17000. Liefert true bei TCP-Connect-Erfolg.
bool rebootSpeaker(const String& speakerIP);

#endif // BOSEFIX32_SPEAKER_TELNET_H
