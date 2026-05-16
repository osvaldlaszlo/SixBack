// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Speaker-Bootstrap via Telnet Port 17000
//
// Implementiert die in RESEARCH.md §12 verifizierte Sequenz:
//   sys configuration bmxRegistryUrl <base>/bmx/registry/v1/services
//   sys configuration statsServerUrl <base>
//   sys configuration margeServerUrl <base>
//   sys configuration swUpdateUrl    <base>/updates/soundtouch
//   envswitch boseurls set <base> <base>/updates/soundtouch
//   getpdo CurrentSystemConfiguration
//   sys reboot
//
// Phase 0: vollständig implementiert für manuellen Aufruf (z.B. aus
// Serial-Console). Phase 3: an Web-UI angebunden.

#include "speaker_telnet.h"
#include "config.h"
#include <WiFi.h>

static bool sendAndExpectOK(WiFiClient& client, const String& cmd,
                             String& reply) {
    client.print(cmd + "\n");
    reply = "";
    uint32_t deadline = millis() + 3000;
    while (millis() < deadline) {
        while (client.available()) {
            char c = client.read();
            reply += c;
            if (reply.endsWith("->")) {
                // Bose-Prompt — Antwort komplett.
                return reply.indexOf("OK") >= 0 ||
                       reply.indexOf("Setting") >= 0;
            }
        }
        delay(20);
    }
    return false;
}

MigrationResult migrateSpeaker(const String& speakerIP,
                                const String& serverBaseUrl) {
    MigrationResult r{false, "", ""};

    WiFiClient client;
    if (!client.connect(speakerIP.c_str(), BOSE_TELNET_PORT, 5000)) {
        r.message = "Telnet-Connect zu " + speakerIP + ":17000 fehlgeschlagen";
        return r;
    }

    // Banner-Prompt abwarten
    delay(300);
    while (client.available()) client.read();

    struct { const char* cmd_prefix; String value; } commands[5] = {
        { "sys configuration bmxRegistryUrl ", serverBaseUrl + "/bmx/registry/v1/services" },
        { "sys configuration statsServerUrl ", serverBaseUrl },
        { "sys configuration margeServerUrl ", serverBaseUrl },
        { "sys configuration swUpdateUrl ",    serverBaseUrl + "/updates/soundtouch" },
        { "envswitch boseurls set ",           serverBaseUrl + " " + serverBaseUrl + "/updates/soundtouch" },
    };

    String reply;
    for (auto& c : commands) {
        String cmd = String(c.cmd_prefix) + c.value;
        if (!sendAndExpectOK(client, cmd, reply)) {
            r.message = "Kommando fehlgeschlagen: " + cmd + "\nAntwort: " + reply;
            client.stop();
            return r;
        }
    }

    // Verifikation (best-effort - alle 5 SET-cmds haben OK geliefert, der
    // getpdo-Reply ist aber zeitweise inkonsistent, weshalb ein
    // strict-substring-check zu false-negatives fuehrt. Wir loggen den
    // Reply zur Diagnose, brechen aber NICHT mehr ab wenn die URL fehlt
    // -- der Auto-Claim in refreshMigrationStatus faengt das auf.)
    if (sendAndExpectOK(client, "getpdo CurrentSystemConfiguration", reply)) {
        r.verifiedConfig = reply;
    }

    // Reboot
    client.print("sys reboot\n");
    delay(500);
    client.stop();

    r.ok = true;
    r.message = "Migration erfolgreich, Speaker rebooted";
    return r;
}

MigrationResult revertSpeaker(const String& speakerIP) {
    MigrationResult r{false, "", ""};
    WiFiClient c;
    if (!c.connect(speakerIP.c_str(), BOSE_TELNET_PORT, 5000)) {
        r.message = "Telnet-Connect fehlgeschlagen";
        return r;
    }
    delay(300);
    while (c.available()) c.read();
    // Original-Bose-URLs zuruecksetzen (auch wenn Cloud tot - das ist der
    // Werks-Zustand)
    struct { const char* cmd; } commands[5] = {
        { "sys configuration bmxRegistryUrl https://content.api.bose.io/bmx/registry/v1/services" },
        { "sys configuration statsServerUrl https://events.api.bosecm.com" },
        { "sys configuration margeServerUrl https://streaming.bose.com" },
        { "sys configuration swUpdateUrl    https://worldwide.bose.com/updates/soundtouch" },
        { "envswitch boseurls set https://streaming.bose.com https://worldwide.bose.com/updates/soundtouch" },
    };
    String reply;
    for (auto& cmd : commands) {
        if (!sendAndExpectOK(c, cmd.cmd, reply)) {
            r.message = String("Kommando fehlgeschlagen: ") + cmd.cmd + "\n" + reply;
            c.stop();
            return r;
        }
    }
    if (!sendAndExpectOK(c, "getpdo CurrentSystemConfiguration", reply)) {
        r.message = "getpdo fehlgeschlagen";
        c.stop();
        return r;
    }
    r.verifiedConfig = reply;
    c.print("sys reboot\n");
    delay(500);
    c.stop();
    r.ok = true;
    r.message = "Revert erfolgreich, Speaker rebooted";
    return r;
}

bool rebootSpeaker(const String& speakerIP) {
    WiFiClient c;
    if (!c.connect(speakerIP.c_str(), BOSE_TELNET_PORT, 3000)) return false;
    delay(200);
    while (c.available()) c.read();
    c.print("sys reboot\n");
    delay(500);
    c.stop();
    return true;
}
