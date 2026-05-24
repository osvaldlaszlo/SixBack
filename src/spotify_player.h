// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Spotify Web-API Remote-Trigger
//
// Architektur (verifiziert 2026-05-23 via curl-tests, siehe Memo
// [[reference-bosefix32-spotify-webapi-proof]]):
//
//   1. User druckt PRESET_N am Speaker
//   2. Speaker sendet scmudc-Event preset-pressed (buttonId=PRESET_N)
//   3. event_store ruft unseren registrierten Callback
//   4. spotify_player schaut in Slot-Map ob (deviceId,slot) → spotify_uri
//      gemappt ist
//   5. Wenn ja: PUT https://api.spotify.com/v1/me/player/play
//      ?device_id=<spotify-connect-id> {context_uri:"spotify:..."}
//   6. Spotify Connect wacht den Speaker auf + startet den Stream
//
// Phase A: Slot-Map + Callback-Wiring + Log-only (NO actual API calls)
// Phase B: OAuth-Flow + Token-Storage + Refresh-Cycle
// Phase C: echte HTTPS-Aufrufe an Spotify Web API
//
// Voraussetzung Premium-Account, eine Spotify-Developer-App, einmaliger
// OAuth-Flow pro User.

#ifndef SIXBACK_SPOTIFY_PLAYER_H
#define SIXBACK_SPOTIFY_PLAYER_H

#include <Arduino.h>
#include <vector>

namespace sixback {
namespace spotify {

// Pro Speaker pro Slot 1..6: ist der Slot Spotify-gesteuert? Welche URI?
struct SlotMapping {
    String  deviceId;       // Bose Speaker MAC, e.g. "EC24B89C26AD"
    int     slot;           // 1..6
    String  spotifyUri;     // "spotify:playlist:XXX" / "spotify:album:XXX" / "spotify:track:XXX"
    String  displayName;    // optional, UI-only
};

// Init: hooks die preset-pressed-callback in event_store ein + lädt
// persistierten state aus NVS.
void init();

// Slot-Mapping setzen oder loeschen.
//   setSlot(dev, n, "")  -> mapping entfernen (Slot ist "nicht Spotify")
//   setSlot(dev, n, uri) -> mapping anlegen oder updaten
// NVS-write nach jeder Aenderung.
void setSlot(const String& deviceId, int slot, const String& uri,
             const String& displayName = "");

// Lookup. Returns false wenn keine Spotify-Belegung.
bool getSlot(const String& deviceId, int slot, SlotMapping& out);

// Alle Mappings als Liste (fuer UI).
std::vector<SlotMapping> listSlots();

// Callback der vom event_store geliefert wird wenn der Speaker eine
// Preset-Taste meldet. Nicht direkt aufrufen — wird in init() registriert.
void onPresetPressed(const String& deviceId, int slot, const String& origin);

// -----------------------------------------------------------------------------
// Phase C — Spotify Web API Auth + Player
// -----------------------------------------------------------------------------

// Vollstaendiger Auth-State (NVS-persistiert).
struct AuthState {
    String clientId;
    String clientSecret;
    String accessToken;
    String refreshToken;
    uint64_t expiresAtMs = 0;     // millis()-Wert wann der access-Token expired
    String spotifyUserId;         // "31olu3sd2zm6yj3pudkmpzc27sbm"
    String spotifyDisplayName;    // optional
};

// Token-State setzen (nach OAuth-Flow oder manuell via API).
// Persistiert sofort in NVS.
void setAuth(const AuthState& s);
AuthState getAuth();
bool isAuthConfigured();

// Aktuell gueltigen Access-Token holen. Refresht intern wenn expired/bald.
// Returns empty string bei Fehler.
String getValidAccessToken();

// Spotify Connect Device-Lookup. Cacht intern fuer 5min.
// Returns Spotify-Connect device_id (40 char hex) oder "" wenn Speaker
// nicht in Spotify's Geraeteliste ist.
String lookupSpotifyDeviceId(const String& speakerName);

// Triggert Stream-Start auf einem Spotify-Connect-Geraet.
// Returns true bei HTTP 204 oder 202.
bool playOnDevice(const String& spotifyDeviceId, const String& contextUri);

// -----------------------------------------------------------------------------
// Phase B — OAuth Authorization-Code Flow (Browser-driven)
// -----------------------------------------------------------------------------

// Baut die Spotify-Auth-URL fuer den Benutzer-Browser.
//   scope = "user-read-playback-state user-modify-playback-state user-read-email"
String genAuthUrl(const String& clientId, const String& redirectUri);

// Tauscht den OAuth-Code gegen access+refresh-Token + fetcht /v1/me.
// Persistiert die kompletten Credentials via setAuth(). Returns true bei Erfolg.
bool exchangeAuthCode(const String& code,
                      const String& clientId,
                      const String& clientSecret,
                      const String& redirectUri,
                      String& errorOut);

// Holt User-Profil (id, display_name) vom Spotify-/v1/me-Endpoint.
// Nutzt den aktuell guelitgen Access-Token. Returns true bei Erfolg.
bool fetchUserInfo(String& userIdOut, String& displayNameOut);

// Im loop() aufrufen — interner 60s-Rate-Limit. Refresht proaktiv den
// Access-Token wenn weniger als 5 Minuten Restlaufzeit. No-op wenn keine
// Auth konfiguriert.
void refreshTick();

// -----------------------------------------------------------------------------
// Debug — letzte Trigger-Versuche
// -----------------------------------------------------------------------------
struct TriggerLogEntry {
    uint32_t ts_ms          = 0;
    String   deviceId;
    int      slot           = 0;
    String   uri;
    String   speakerName;
    String   spotifyDeviceId;
    int      playHttpCode   = -1;   // /play HTTP code, -1 wenn nicht erreicht
    String   result;                 // "ok", "no-mapping", "no-auth", "not-in-inventory", "not-in-devices", "play-failed", "no-token"
};

std::vector<TriggerLogEntry> listTriggerLog();

} // namespace spotify
} // namespace sixback

#endif
