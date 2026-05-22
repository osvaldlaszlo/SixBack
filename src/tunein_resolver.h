// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — TuneIn-Live-Resolver mit Cache + User-Override
//
// Resolver-Pipeline:
//   1. PresetStore: gibt es einen Override (streamUrl) fuer diese stationId?
//      -> Bose-JSON-Wrapper, kein TuneIn-Call.
//   2. NVS-Cache (Namespace sixback-tune): TTL 7 Tage.
//      -> Cache-Hit -> JSON sofort aus Cache.
//   3. HTTP-GET zu http://opml.radiotime.com/Tune.ashx?id=<id>&render=json
//      -> Stream-URL extrahieren, in NVS cachen, JSON bauen.
//   4. Hardcoded-Fallback fuer die 6 Dirk-Stations (Notnagel wenn
//      Internet weg oder TuneIn-API tot ist).
#ifndef BOSEFIX32_TUNEIN_RESOLVER_H
#define BOSEFIX32_TUNEIN_RESOLVER_H

#include <Arduino.h>

namespace sixback {

struct TuneInResolution {
    String stationId;
    String name;
    String streamUrl;
    String imageUrl;
    String source;        // "preset_override" / "cache" / "opml" / "fallback"
    bool   ok;
};

TuneInResolution resolveTuneInStruct(const String& stationId);

} // namespace sixback

// Legacy/compat — Phase-0-API.
String resolveTuneInStation(const String& stationId);

#endif
