// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Bose Cloud Replacement Endpoints
//
// Verifizierte Pflicht-Endpoints aus den AfterTouch-Live-Logs (Pi5-Migration
// Kueche 2026-05-16). Phase 1: alle 11 Endpoints mit echten Bodies, plus
// Account-Sources und Preset-Sync zwischen Speaker und ESP-Store.

#include "bose_endpoints.h"
#include "tunein_resolver.h"
#include "preset_store.h"
#include "speaker_inventory.h"
#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

namespace {

void send200(AsyncWebServerRequest* r) { r->send(200, "text/plain", ""); }

// -----------------------------------------------------------------------------
// POST /v1/scmudc/{deviceId}  -- SoundTouch-Telemetrie verwerfen
// -----------------------------------------------------------------------------
void handleSCMUDC(AsyncWebServerRequest* req) {
    Serial.printf("[bmx] SCMUDC device=%s\n", req->pathArg(0).c_str());
    send200(req);
}

// -----------------------------------------------------------------------------
// GET /bmx/registry/v1/services  -- Service-Discovery
//   Liefert AfterTouch's vollstaendiges Service-Descriptor-JSON (8 KB),
//   mit Token-Replacement fuer {BMX_SERVER} und {MEDIA_SERVER}. Ohne diesen
//   Descriptor markiert der Speaker TUNEIN als UNAVAILABLE, Preset-Tasten
//   bleiben tot.
// -----------------------------------------------------------------------------
void handleBMXRegistry(AsyncWebServerRequest* req) {
    if (!LittleFS.exists("/bmx_services.json")) {
        req->send(500, "application/json", "{\"error\":\"bmx_services.json missing in LittleFS\"}");
        return;
    }
    File f = LittleFS.open("/bmx_services.json", "r");
    String body = f.readString();
    f.close();
    String base = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
    body.replace("{BMX_SERVER}",   base);
    body.replace("{MEDIA_SERVER}", base + "/media");
    req->send(200, "application/json", body);
}

void handleBMXServicesAvailability(AsyncWebServerRequest* req) {
    if (!LittleFS.exists("/bmx_services_availability.json")) {
        req->send(200, "application/json", "{\"services\":[]}");
        return;
    }
    req->send(LittleFS, "/bmx_services_availability.json", "application/json");
}

void handlePowerOn(AsyncWebServerRequest* req) {
    String body;
    if (req->hasParam("plain", true)) body = req->getParam("plain", true)->value();
    Serial.printf("[marge] power_on body-bytes=%u\n", body.length());
    send200(req);
}

// -----------------------------------------------------------------------------
// GET /streaming/account/{id}/full  -- Account-Mock mit Default-Sources
// -----------------------------------------------------------------------------
void handleAccountFull(AsyncWebServerRequest* req) {
    String acct = req->pathArg(0);
    String body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    body += "<account><accountID>" + acct + "</accountID>";
    body += "<sources>"
              "<source id=\"10002\" type=\"Audio\">"
                "<sourceproviderid>2</sourceproviderid>"
                "<name>INTERNET_RADIO</name>"
                "<username></username>"
                "<credential type=\"token\"></credential>"
                "<sourcename></sourcename>"
              "</source>"
              "<source id=\"10003\" type=\"Audio\">"
                "<sourceproviderid>11</sourceproviderid>"
                "<name>LOCAL_INTERNET_RADIO</name>"
                "<username></username>"
                "<credential type=\"token\">eyJzZXJpYWwiOiJsb2NhbC1pbnRlcm5ldC1yYWRpbyJ9</credential>"
                "<sourcename></sourcename>"
              "</source>"
              "<source id=\"10004\" type=\"Audio\">"
                "<sourceproviderid>3</sourceproviderid>"
                "<name>TUNEIN</name>"
                "<username>TuneIn</username>"
                "<credential type=\"token\"></credential>"
                "<sourcename>TuneIn Radio</sourcename>"
              "</source>"
            "</sources>";
    // Account-Presets: liefere die Presets des ersten Speakers mit diesem
    // Account zurueck. Damit re-sync't der Speaker nach Migration nicht
    // gegen leere Liste sondern bekommt sofort seine alten 6 Stations.
    String presetsXml;
    for (auto& s : bosefix::SpeakerInventory::instance().list()) {
        if (s.accountId == acct) {
            presetsXml = bosefix::PresetStore::instance().toBoseXml(s.deviceId);
            break;
        }
    }
    if (presetsXml.length() == 0) {
        body += "<presets/>";
    } else {
        // entferne XML-Header aus dem PresetStore-XML, behalte nur <presets>...
        int p = presetsXml.indexOf("<presets");
        if (p > 0) presetsXml = presetsXml.substring(p);
        body += presetsXml;
    }
    body += "</account>";
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
}

void handleAccountSources(AsyncWebServerRequest* req) {
    String body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                  "<sources>"
                  "<source id=\"10002\"><name>INTERNET_RADIO</name><sourceproviderid>2</sourceproviderid></source>"
                  "<source id=\"10003\"><name>LOCAL_INTERNET_RADIO</name><sourceproviderid>11</sourceproviderid></source>"
                  "<source id=\"10004\"><name>TUNEIN</name><sourceproviderid>3</sourceproviderid></source>"
                  "</sources>";
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
}

void handleProviderSettings(AsyncWebServerRequest* req) {
    req->send(200, "application/json",
              "{\"providers\":["
                "{\"id\":3,\"name\":\"TUNEIN\",\"enabled\":true},"
                "{\"id\":11,\"name\":\"LOCAL_INTERNET_RADIO\",\"enabled\":true}"
              "]}");
}

// -----------------------------------------------------------------------------
// Preset-Endpoints: Speaker fragt nach Account-/Device-Presets
// -----------------------------------------------------------------------------
void handleAccountPresets(AsyncWebServerRequest* req) {
    // Liefert PresetStore aller bekannten Speaker zusammengefuehrt - oder
    // einfach leer wenn keine Speaker bekannt.
    // Bose-XML mit max 6 Presets - wir liefern hier den ersten bekannten.
    auto speakers = bosefix::SpeakerInventory::instance().list();
    String body;
    if (!speakers.empty()) {
        body = bosefix::PresetStore::instance().toBoseXml(speakers[0].deviceId);
    } else {
        body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<presets/>";
    }
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
}

void handleDevicePresets(AsyncWebServerRequest* req) {
    String acct  = req->pathArg(0);
    String devId = req->pathArg(1);
    String body  = bosefix::PresetStore::instance().toBoseXml(devId);
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
}

// -----------------------------------------------------------------------------
// /bmx/tunein/* — Station-Resolver, Token-Mock, Report-Sink
// -----------------------------------------------------------------------------
void handleTuneInToken(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"access_token\":\"\",\"refresh_token\":\"\"}");
}

void handleTuneInStation(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
    String json = resolveTuneInStation(id);
    req->send(200, "application/json", json);
}

void handleTuneInReport(AsyncWebServerRequest* req)  { send200(req); }
void handleRecent(AsyncWebServerRequest* req)        { req->send(201, "text/plain", ""); }

// -----------------------------------------------------------------------------
// GET /updates/soundtouch  -- Replica der Bose-Firmware-Index-Liste
// -----------------------------------------------------------------------------
void handleSWUpdateIndex(AsyncWebServerRequest* req) {
    const char* body =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<INDEX REVISION=\"02.11.00\">\n"
"  <DEVICE ID=\"0x0939\" PRODUCTNAME=\"SoundTouch 10\">\n"
"    <HARDWARE REVISION=\"00.01.00\">\n"
"      <RELEASE REVISION=\"27.0.6.46330.5043500\"/>\n"
"    </HARDWARE>\n"
"  </DEVICE>\n"
"  <DEVICE ID=\"0x093B\" PRODUCTNAME=\"SoundTouch 20\">\n"
"    <HARDWARE REVISION=\"00.01.00\">\n"
"      <RELEASE REVISION=\"27.0.6.46330.5043500\"/>\n"
"    </HARDWARE>\n"
"  </DEVICE>\n"
"  <DEVICE ID=\"0x093C\" PRODUCTNAME=\"SoundTouch 30\">\n"
"    <HARDWARE REVISION=\"00.01.00\">\n"
"      <RELEASE REVISION=\"27.0.6.46330.5043500\"/>\n"
"    </HARDWARE>\n"
"  </DEVICE>\n"
"  <DEVICE ID=\"0x094A\" PRODUCTNAME=\"SoundTouch Wireless Link adapter\">\n"
"    <HARDWARE REVISION=\"00.01.00\">\n"
"      <RELEASE REVISION=\"27.0.6.46330.5043500\"/>\n"
"    </HARDWARE>\n"
"  </DEVICE>\n"
"  <DEVICE ID=\"0x0949\" PRODUCTNAME=\"SoundTouch 300\">\n"
"    <HARDWARE REVISION=\"00.01.00\">\n"
"      <RELEASE REVISION=\"27.0.6.46330.5043500\"/>\n"
"    </HARDWARE>\n"
"  </DEVICE>\n"
"</INDEX>\n";
    req->send(200, "application/xml", body);
}

// -----------------------------------------------------------------------------
// Catch-all - logged & 404. Hilft bei der Endpoint-Forensik.
// -----------------------------------------------------------------------------
void handleNotFound(AsyncWebServerRequest* req) {
    Serial.printf("[bmx][?] %s %s\n", req->methodToString(), req->url().c_str());
    req->send(404, "text/plain", "BoseFix32: endpoint not implemented");
}

} // anon

void registerBoseEndpoints(AsyncWebServer& server) {
    server.on("^/v1/scmudc/([^/]+)$",                                 HTTP_POST, handleSCMUDC);
    server.on("/bmx/registry/v1/services",                            HTTP_GET,  handleBMXRegistry);
    server.on("/bmx/registry/v1/servicesAvailability",                HTTP_GET,  handleBMXServicesAvailability);
    server.on("/streaming/support/power_on",                          HTTP_POST, handlePowerOn);
    server.on("^/streaming/account/([^/]+)/full$",                    HTTP_GET,  handleAccountFull);
    server.on("^/streaming/account/([^/]+)/sources$",                 HTTP_GET,  handleAccountSources);
    server.on("^/streaming/account/([^/]+)/provider_settings$",       HTTP_GET,  handleProviderSettings);
    server.on("^/streaming/account/([^/]+)/presets$",                 HTTP_GET,  handleAccountPresets);
    server.on("^/streaming/account/([^/]+)/presets/all$",             HTTP_GET,  handleAccountPresets);
    server.on("^/streaming/account/([^/]+)/device/([^/]+)/presets$",  HTTP_GET,  handleDevicePresets);
    server.on("/bmx/tunein/v1/token",                                 HTTP_POST, handleTuneInToken);
    server.on("^/bmx/tunein/v1/playback/station/([^/]+)$",            HTTP_GET,  handleTuneInStation);
    server.on("/bmx/tunein/v1/report",                                HTTP_POST, handleTuneInReport);
    server.on("^/streaming/account/([^/]+)/device/([^/]+)/recent$",   HTTP_POST, handleRecent);
    server.on("/updates/soundtouch",                                  HTTP_GET,  handleSWUpdateIndex);

    server.onNotFound(handleNotFound);
}
