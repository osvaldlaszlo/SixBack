// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — WiFi-Provisionierung via Improv (tostmann/improv-wifi-busware)
//
// Beim Boot (RFNETHM-Pattern, Stand 2026-05-16):
//   1. Improv-Window IMMER ab Boot oeffnen — egal ob NVS-Creds da sind.
//      Idle-basierte Dauer (verlaengert sich bei UART-Aktivitaet):
//        - Keine NVS-Creds:  120 s seit letzter Activity (volles Erstprovisioning)
//        - NVS-Creds da:      30 s seit letzter Activity (kurze Re-Provisioning)
//      Jedes empfangene UART-Byte setzt den Idle-Timer zurueck — User kann
//      entspannt im ESP-Web-Tools-Flow tippen/scrollen ohne mitten drin
//      rauszufliegen.
//   2. NVS-Credentials laden + STA-Connect parallel versuchen.
//   3. WiFi up → return; Improv laeuft im Hintergrund (via wifiProvisioningTick)
//      bis zum Idle-Window-Ablauf weiter. So kann der User jederzeit nach
//      Boot neue WLAN-Credentials einspeisen (Router-Wechsel etc.).
//   4. Kein NVS-Connect → WPS-PBC zusaetzlich aktivieren (120 s — User
//      drueckt WPS-Knopf am Router, ESP nimmt SSID/PSK direkt vom Router-
//      Frame), parallel zu Improv. Sobald eine Quelle erfolgreich
//      provisioniert → connected.
//   5. Beide Fenster zu ohne Connect → Restart nach 10 s.
#ifndef BOSEFIX32_WIFI_PROVISIONING_H
#define BOSEFIX32_WIFI_PROVISIONING_H

#include <Arduino.h>

namespace bosefix {

// Startet die Provisionierungs-Pipeline. Blockiert bis WiFi connected ist
// (entweder aus NVS oder via Improv). Bei harten Fehlern -> Restart nach 10 s.
void provisionWifi();

// Zu Service-/Debug-Zwecken: löscht NVS-WiFi-Credentials. Beim nächsten Boot
// startet wieder Improv-Mode.
void factoryResetWifi();

// Im loop() zu rufen — treibt Improv-Serial-State-Machine an + pflegt das
// Idle-Window. Sobald das Window zu ist, macht die Funktion nichts mehr.
void wifiProvisioningTick();

// Status fuer /api/status: ist das Improv-Window noch offen?
bool improvIsActive();

// Verbleibende Sekunden bis das Idle-Window schliesst (0 wenn zu / nicht aktiv).
uint32_t improvWindowRemainingS();

// WPS-Push-Button-Config — laeuft 120 s ab Aktivierung, automatisch
// gestartet wenn beim Boot keine NVS-Creds da sind oder der STA-Connect
// in 20 s nicht zustande kommt.  Captive-Portal laeuft unter den gleichen
// Bedingungen parallel; siehe `captive_portal.h`.
bool wpsIsActive();
uint32_t wpsWindowRemainingS();

// Persistiert Credentials in die gleiche NVS-Schublade die tryConnectFromNVS
// liest. Wird auch vom Captive-Portal verwendet.
void persistCreds(const String& ssid, const String& psk);

} // namespace bosefix

#endif // BOSEFIX32_WIFI_PROVISIONING_H
