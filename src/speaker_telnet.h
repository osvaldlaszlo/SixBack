// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Speaker-Bootstrap via Telnet Port 17000 (Bose Diag Shell)
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

// Fuehrt `getpdo CurrentSystemConfiguration` am Speaker aus und gibt die
// gesamte Antwort (alles bis zum '->'-Prompt, exklusive) in `out` zurueck.
// Liefert true wenn Telnet-Connect + Kommando erfolgreich, sonst false.
// Eigentlich angefragt war `sys configuration list` — das gibt es bei der
// Bose-Diag-Shell aber nicht (waere ein Setter). `getpdo` ist der Getter
// fuer dieselben Felder und ist auch der existierende Pattern in
// migrateSpeaker(). Eigene Lese-Schleife (kein Error-Marker-Check wie
// bei den Set-Kommandos) — die PDO-Ausgabe enthaelt voellig legitim
// Schluessel, die in der negativen Marker-Liste vorkommen wuerden.
bool captureSysConfigurationList(const String& speakerIP, String& out);

#endif // BOSEFIX32_SPEAKER_TELNET_H
