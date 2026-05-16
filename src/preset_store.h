// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Preset-Store
//
// Verwaltet pro Speaker eine Liste von max. 6 Presets, persistent in NVS.
// Plus Gruppen-Mechanik: ein Speaker kann Mitglied einer Gruppe sein,
// und ein PUT auf /api/groups/{groupId}/preset/{n} propagiert das Preset
// in alle Gruppen-Mitglieder.
//
// Preset kann sein:
//   - TuneIn-Station (sourceType=TUNEIN, stationId="s24896")
//   - Direkter Stream-URL (sourceType=LOCAL_INTERNET_RADIO, url="http://...")
//   - Spotify, Pandora etc. spaeter
#ifndef BOSEFIX32_PRESET_STORE_H
#define BOSEFIX32_PRESET_STORE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

namespace bosefix {

enum class PresetSource : uint8_t {
    EMPTY                = 0,
    TUNEIN               = 1,
    LOCAL_INTERNET_RADIO = 2,
};

struct Preset {
    uint8_t      slot;          // 1..6
    PresetSource source;
    String       name;          // z.B. "SWR3"
    String       stationId;     // bei TUNEIN: "s24896"
    String       streamUrl;     // bei LOCAL_INTERNET_RADIO: "http://liveradio.swr.de/tn2d2ac/swr3"
    String       imageUrl;      // optional
};

class PresetStore {
public:
    static PresetStore& instance();

    void loadFromNVS();
    void saveToNVS();

    // Lese alle 6 Slots fuer einen Speaker; leere Slots haben source=EMPTY.
    std::vector<Preset> getForSpeaker(const String& deviceId);
    // Lese Preset fuer einen Slot (1..6).
    Preset get(const String& deviceId, uint8_t slot);
    // Setze ein Preset (slot 1..6). Persistiert.
    bool   set(const String& deviceId, const Preset& p);
    // Leeren Slot setzen (delete).
    bool   clear(const String& deviceId, uint8_t slot);

    // Gruppen-Operationen: kopiert die Presets eines Source-Speakers auf alle
    // anderen Speaker mit gleicher groupId (oder explicit deviceId-Liste).
    int syncToGroup(const String& sourceDeviceId,
                    const std::vector<String>& targetDeviceIds);

    // Komplett-JSON fuer Backup/Restore.
    void exportJson(JsonDocument& out);
    bool importJson(JsonDocument& in);

    // Fuer den Speaker-Cloud-Mock (/bmx/marge ...): liefert die Preset-Liste
    // im Bose-XML-Format - alle Slots fuer den deviceId.
    String toBoseXml(const String& deviceId);

private:
    PresetStore() = default;

    // Pro Speaker: 6 Slots im Vector (slot=1 ist [0], slot=6 ist [5]).
    // Map-aehnlich: deviceId -> array.
    struct PerSpeaker {
        String  deviceId;
        Preset  slots[6];
    };
    std::vector<PerSpeaker> speakers_;

    PerSpeaker* findOrCreate_(const String& deviceId);
    PerSpeaker* find_(const String& deviceId);
};

const char* presetSourceToStr(PresetSource s);
PresetSource presetSourceFromStr(const String& s);

} // namespace bosefix

#endif
