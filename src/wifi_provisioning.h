// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — WiFi-Provisionierung via Improv + Captive-Portal
// (Schwester-Pattern aus ip4knx / TUL KNX-Gateway).
//
// Beim Boot:
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
//   4. Kein NVS-Connect → Captive-Portal (offener AP "SixBack-XXYYZZ" +
//      DNS-Hijack + HTTP-Form) parallel zu Improv arm-en. Sobald eine
//      Quelle erfolgreich provisioniert → connected.
//   5. Beide Fenster zu ohne Connect → Restart nach 10 s.
//
// WPS wurde bewusst entfernt: esp_wifi_wps_enable() beansprucht den
// WiFi-Event-Handler exklusiv und konkurriert mit dem Improv-eigenen
// WiFi.scanNetworks() (Symptom: leere AP-Liste in ESP Web Tools).
// ip4knx faehrt exakt mit dem Improv+Captive-Pattern und ist stabil.
#ifndef BOSEFIX32_WIFI_PROVISIONING_H
#define BOSEFIX32_WIFI_PROVISIONING_H

#include <Arduino.h>

namespace sixback {

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

// Persistiert Credentials in die gleiche NVS-Schublade die tryConnectFromNVS
// liest. Wird auch vom Captive-Portal verwendet.
void persistCreds(const String& ssid, const String& psk);

// Schaltet WiFi-Power-Save komplett ab, dreht TX-Power auf Max und
// pinned CPU auf die hoechste Frequenz. Wird nach JEDEM erfolgreichen
// STA-Connect aufgerufen — der Bose-Speaker muss die Cloud-Endpoints
// jederzeit (auch Stunden spaeter beim Preset-Druck) ohne PS-Latenz
// erreichen koennen, sonst gibt's gefuehlte Hangs am Speaker.
void wifiOptimizeForReliability();

} // namespace sixback

#endif // BOSEFIX32_WIFI_PROVISIONING_H
