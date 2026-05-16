// SPDX-License-Identifier: GPL-3.0-or-later
#include "captive_portal.h"
#include "wifi_provisioning.h"
#include "config.h"
#include "version.h"

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

namespace bosefix {

namespace {

constexpr uint32_t  CAPTIVE_WINDOW_MS = 5 * 60 * 1000;   // 5 min idle
constexpr uint16_t  CAPTIVE_PORT      = 80;
constexpr uint16_t  DNS_PORT          = 53;
const     IPAddress AP_IP(192, 168, 4, 1);
const     IPAddress AP_NETMASK(255, 255, 255, 0);

DNSServer       dnsServer;
AsyncWebServer* captiveServer = nullptr;
bool            active        = false;
uint32_t        startMs       = 0;
String          apSsid;
String          provisionedSta;   // empty = pending, "x.x.x.x" once STA up

// Minimal-HTML Form (~1.2 KB), keine externen Assets — funktioniert ohne
// Internet, was im Captive-Portal Pflicht ist.
String formHtml() {
    return F(
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<title>BoseFix32 \xe2\x80\x94 WiFi Setup</title>"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<style>body{font:-apple-system,Segoe UI,sans-serif;max-width:28em;margin:1.5em auto;padding:0 1em;color:#222;background:#f7f6f3}"
"h1{color:#7a3e00;margin:0 0 .2em}label{display:block;margin:.9em 0 .25em;font-weight:600;font-size:.9em}"
"input,select{width:100%;padding:.45em;font:inherit;border:1px solid #e1ddd2;border-radius:4px;background:#fff}"
"button{margin-top:1.2em;width:100%;padding:.7em;background:#7a3e00;color:#fff;border:0;border-radius:5px;font:inherit;font-weight:600;cursor:pointer}"
"button:hover{background:#5a2d00}p{color:#777;font-size:.9em}</style></head><body>"
"<h1>BoseFix32 \xe2\x80\x94 WiFi Setup</h1>"
"<p>Wähle dein WLAN und gib das Passwort ein.</p>"
"<form method=\"post\" action=\"/save\">"
"<label>Network</label>"
"<select id=\"pick\" onchange=\"document.getElementById('ssid').value=this.value\">"
"<option value=\"\">\xe2\x80\x94 scanning \xe2\x80\xa6 \xe2\x80\x94</option></select>"
"<label>SSID</label><input type=\"text\" id=\"ssid\" name=\"ssid\" required>"
"<label>Password</label><input type=\"password\" name=\"psk\" placeholder=\"(empty for open networks)\">"
"<button type=\"submit\">Save &amp; connect</button></form>"
"<script>fetch('/scan').then(r=>r.json()).then(d=>{"
"const s=document.getElementById('pick');"
"s.innerHTML='<option value=\"\">\xe2\x80\x94 pick one \xe2\x80\x94</option>'+"
"(d.networks||[]).map(n=>`<option value=\"${n.ssid}\">${n.ssid} (${n.rssi} dBm${n.open?', open':''})</option>`).join('');"
"});</script></body></html>");
}

String successHtml(const String& staIp) {
    String h = F(
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<title>BoseFix32 \xe2\x80\x94 Connected</title>"
"<meta http-equiv=\"refresh\" content=\"20;url=http://");
    h += staIp;
    h += F("/\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<style>body{font:-apple-system,Segoe UI,sans-serif;max-width:28em;margin:1.5em auto;padding:0 1em;color:#222;background:#f7f6f3}"
"h1{color:#1f7a3a;margin:0 0 .3em}p{color:#444}code{background:#fee;padding:.1em .35em;border-radius:3px}"
"a{color:#7a3e00}</style></head><body><h1>Connected!</h1>"
"<p>BoseFix32 ist jetzt im LAN unter <a href=\"http://");
    h += staIp;
    h += F("/\">http://");
    h += staIp;
    h += F("/</a> erreichbar (oder per <code>http://bosefix.local/</code>).</p>"
"<p><b>Wechsle dein Handy zurueck in dein normales WLAN.</b> Diese Seite "
"leitet in 20 Sekunden automatisch dorthin weiter \xe2\x80\x94 das funktioniert "
"natuerlich nur wenn du im richtigen Netz bist.</p>"
"<p style=\"color:#777;font-size:.85em\">BoseFix32 " FW_VERSION_STRING "</p>"
"</body></html>");
    return h;
}

void handleRoot(AsyncWebServerRequest* req) {
    if (provisionedSta.length() > 0) {
        req->send(200, "text/html; charset=utf-8", successHtml(provisionedSta));
        return;
    }
    req->send(200, "text/html; charset=utf-8", formHtml());
}

void handleScan(AsyncWebServerRequest* req) {
    // Synchroner Scan — ~2 s. AsyncWebServer toleriert das, weil handler in
    // eigener Task laeuft. Im AP-Modus geht's nur eingeschraenkt; AP_STA ist
    // notwendig damit Scan ueberhaupt fuktioniert.
    int n = WiFi.scanNetworks(false, true);
    String body = "{\"networks\":[";
    for (int i = 0; i < n; ++i) {
        if (i) body += ",";
        String ssid = WiFi.SSID(i);
        ssid.replace("\"", "\\\"");
        body += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i))
              + ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false")
              + "}";
    }
    body += "]}";
    WiFi.scanDelete();
    req->send(200, "application/json", body);
}

void handleSave(AsyncWebServerRequest* req) {
    if (!req->hasParam("ssid", true)) {
        req->send(400, "text/plain", "Missing ssid");
        return;
    }
    String ssid = req->getParam("ssid", true)->value();
    String psk  = req->hasParam("psk", true) ? req->getParam("psk", true)->value() : "";
    Serial.printf("[captive] save ssid=%s\n", ssid.c_str());

    WiFi.begin(ssid.c_str(), psk.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 18000) {
        delay(150);
    }
    if (WiFi.status() == WL_CONNECTED) {
        provisionedSta = WiFi.localIP().toString();
        Serial.printf("[captive] connect OK, sta-IP=%s\n", provisionedSta.c_str());
        persistCreds(ssid, psk);
        req->send(200, "text/html; charset=utf-8", successHtml(provisionedSta));
    } else {
        Serial.println("[captive] connect failed");
        req->send(200, "text/html; charset=utf-8",
            "<html><body style='font-family:sans-serif;max-width:28em;margin:2em auto;padding:0 1em'>"
            "<h1 style='color:#a32525'>Connect failed</h1>"
            "<p>SSID oder Passwort falsch? <a href='/'>Zurueck</a></p></body></html>");
    }
}

void handleCaptiveRedirect(AsyncWebServerRequest* req) {
    req->redirect("http://" + AP_IP.toString() + "/");
}

}  // namespace

void captiveStart() {
    if (active) return;

    uint8_t mac[6]; WiFi.macAddress(mac);
    char buf[24];
    snprintf(buf, sizeof(buf), "BoseFix32-%02X%02X%02X", mac[3], mac[4], mac[5]);
    apSsid = buf;

    Serial.printf("[captive] starting AP '%s' (open) + DNS + HTTP on %s\n",
                  apSsid.c_str(), AP_IP.toString().c_str());

    WiFi.mode(WIFI_AP_STA);   // STA bleibt parallel, damit WPS/improv weiter
    WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
    WiFi.softAP(apSsid.c_str());

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", AP_IP);

    captiveServer = new AsyncWebServer(CAPTIVE_PORT);
    captiveServer->on("^/$",                       HTTP_GET,  handleRoot);
    captiveServer->on("/scan",                     HTTP_GET,  handleScan);
    captiveServer->on("/save",                     HTTP_POST, handleSave);
    // Captive-detection endpoints von Apple/Google/Microsoft — alle auf
    // unsere Form umleiten damit der Popup aufgeht.
    captiveServer->on("/hotspot-detect.html",      HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/library/test/success.html",HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/generate_204",             HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/gen_204",                  HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/connecttest.txt",          HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/ncsi.txt",                 HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/redirect",                 HTTP_GET,  handleCaptiveRedirect);
    captiveServer->onNotFound(handleCaptiveRedirect);
    captiveServer->begin();

    active  = true;
    startMs = millis();
    provisionedSta = "";
}

void captiveStop() {
    if (!active) return;
    Serial.println("[captive] stopping");
    if (captiveServer) {
        captiveServer->end();
        delete captiveServer;
        captiveServer = nullptr;
    }
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    active = false;
}

void captiveTick() {
    if (!active) return;
    dnsServer.processNextRequest();
    if (millis() - startMs > CAPTIVE_WINDOW_MS) {
        Serial.println("[captive] window expired");
        captiveStop();
    }
}

bool captiveIsActive() { return active; }

uint32_t captiveWindowRemainingS() {
    if (!active) return 0;
    uint32_t e = millis() - startMs;
    return e >= CAPTIVE_WINDOW_MS ? 0 : (CAPTIVE_WINDOW_MS - e) / 1000;
}

}  // namespace bosefix
