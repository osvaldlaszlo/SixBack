// SPDX-License-Identifier: GPL-3.0-or-later
// SixBack — Diagnostic Snapshot
//
// Snapshot des Speakers im IST-Zustand: alle relevanten BMX-API-Antworten
// (XML wie der Speaker sie liefert) + Telnet `sys configuration list`.
// Sinn: User kann das per WebUI-Button runterladen und einreichen, damit
// wir BMX-Quellen / Source-Types nachbauen koennen die noch nicht
// unterstuetzt sind (DLNA/STORED_MUSIC, BLUETOOTH, ...).
//
// Pre-Migrate-Schnappschuss wird beim ERSTEN Migrate eines Speakers
// einmalig in LittleFS unter /snapshots/<deviceId>.json persistiert.
// Wiederholte Migrate/Revert-Zyklen ueberschreiben den File NICHT — wir
// wollen den allerersten Original-Zustand erhalten.
#ifndef BOSEFIX32_SPEAKER_DIAGNOSTIC_H
#define BOSEFIX32_SPEAKER_DIAGNOSTIC_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace sixback {

// Live-Capture. Holt alle BMX-Endpoints am Speaker (Port 8090, XML-Bodies)
// + Telnet `sys configuration list` + setzt die Header-Felder aus dem
// uebergebenen Inventory. Schreibt das Ergebnis in `out`. Reservierte
// Heap-Spitzenlast: ~15 KB.
//
// Liefert true wenn mindestens /info erfolgreich war — dann ist der
// Snapshot brauchbar. Andere Felder duerfen einzeln scheitern; das wird
// als http-Code 0 / ok=false pro Endpoint vermerkt.
bool captureLiveSnapshot(const String& deviceId, JsonDocument& out);

// Bringt einen Pre-Migrate-Snapshot in LittleFS — nur wenn noch keiner
// existiert. Idempotent ueber wiederholte Aufrufe. Wird vom Migrate-Pfad
// (manuell + Auto-Mode) als Pflicht-Hook aufgerufen.
//
// `force=true` ueberschreibt einen evtl. bestehenden Snapshot — fuer
// manuelle Re-Captures aus der UI.
void persistPreMigrateSnapshot(const String& deviceId, bool force = false);

// Laedt einen persistierten Snapshot. Liefert true bei Erfolg.
bool loadStoredSnapshot(const String& deviceId, String& outJson);

// True wenn fuer das Geraet ein persistierter Snapshot vorliegt.
bool hasStoredSnapshot(const String& deviceId);

} // namespace sixback

#endif
