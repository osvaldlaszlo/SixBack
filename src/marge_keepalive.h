// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Marge-Stream Keep-Alive
//
// Periodisch (alle 5 min) ein POST /setMargeAccount an jeden bekannten
// Speaker schicken. Halt den Marge-Bootstrap-Handshake "frisch" damit der
// Speaker den scmudc-Event-Stream nicht aufgibt.
//
// Symptomatik die das adressiert: Speaker stoppt nach unbestimmter Zeit
// (oft <2h) das Posten an /v1/scmudc/{deviceId} trotz korrekter
// statsServerUrl. preset-pressed + andere Events kommen nicht mehr durch
// → kein Spotify-Trigger. /setMargeAccount re-affirmt die Association
// und hat in Tests den Stream sofort wieder hochgefahren.

#ifndef SIXBACK_MARGE_KEEPALIVE_H
#define SIXBACK_MARGE_KEEPALIVE_H

namespace sixback {

// Spawnt FreeRTOS-Task die alle 5 min /setMargeAccount an alle Speaker im
// Inventory pingt. No-op wenn schon gestartet.
void startMargeKeepAlive();

} // namespace sixback

#endif
