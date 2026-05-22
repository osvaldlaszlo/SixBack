// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — IP-Change-Failsafe
//
// Hintergrund: Bose-Speaker speichern margeServerUrl/statsServerUrl/etc.
// als feste IP-Adressen (kein mDNS-Resolve im Speaker-Stack). Wenn der
// ESP eine neue IP vom DHCP bekommt (Router-Reboot, Lease-Expire), zeigen
// alle bereits migrierten Speaker auf eine tote IP und Presets gehen nicht.
//
// Diese Modul:
//   - Merkt sich die letzte eigene IP in NVS
//   - Beim Boot: vergleicht aktuelle IP mit gespeicherter
//   - Bei Änderung: triggert für alle Speaker mit status=MIGRATED eine
//     erneute Migration auf die neue ESP-IP (telnet sys configuration ...)
//   - Loggt jeden Auto-Remigrate im Serial
//
// User-Empfehlung im UI: DHCP-Reservation für ESP-MAC im Router setzen,
// dann tritt der Failsafe nie ein.
#ifndef BOSEFIX32_IP_FAILSAFE_H
#define BOSEFIX32_IP_FAILSAFE_H

#include <Arduino.h>

namespace sixback {

// Beim Boot rufen (nach Connect-WiFi, nach Inventory-Load).
// Loggt + ggf. re-migriert alle bekannten Speaker.
void ipFailsafeCheck();

// Liefert die zuletzt persistierte ESP-IP (oder "" wenn noch nie gespeichert).
String getLastKnownIp();

} // namespace sixback

#endif
