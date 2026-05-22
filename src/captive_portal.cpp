// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "captive_portal.h"
#include "wifi_provisioning.h"
#include "config.h"
#include "version.h"

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

namespace sixback {

namespace {

constexpr uint32_t  CAPTIVE_WINDOW_MS = 5 * 60 * 1000;   // 5 min idle
constexpr uint16_t  CAPTIVE_PORT      = 80;
constexpr uint16_t  DNS_PORT          = 53;
const     IPAddress AP_IP(192, 168, 4, 1);
const     IPAddress AP_NETMASK(255, 255, 255, 0);
// Offener AP — kein PSK. Schwester-Pattern aus ip4knx / TUL-KNX-Gateway:
// dort laeuft der Captive-AP auf ESP32-C3 und C6 ohne PSK zuverlaessig.
// Vorteil fuer User: AP-Beitritt ist Ein-Klick, Captive-Popup geht auf
// allen Handys automatisch auf (iOS/Android erkennen Open-Netze als
// "Hotspot ohne Internet" und triggern den Detect-Endpoint).

DNSServer       dnsServer;
AsyncWebServer* captiveServer = nullptr;
bool            active        = false;
uint32_t        startMs       = 0;
String          apSsid;
String          provisionedSta;   // empty = pending, "x.x.x.x" once STA up

// Async save-state-machine. handleSave kickt einen non-blocking
// WiFi.begin() an und kehrt sofort mit einer Progress-HTML zurueck.
// captiveTick() pollt WiFi.status() und transitioniert.
// /save_status liefert JSON fuer das JS-Polling im Browser.
enum class SaveState : uint8_t { Idle, Connecting, Success, Failed };
SaveState   saveState      = SaveState::Idle;
String      saveSsid;
String      savePsk;
uint32_t    saveStartMs    = 0;
constexpr uint32_t SAVE_TIMEOUT_MS = 20 * 1000;

// Minimal-HTML Form (~1.2 KB), keine externen Assets — funktioniert ohne
// Internet, was im Captive-Portal Pflicht ist.
String formHtml() {
    return F(
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<title>SixBack \xe2\x80\x94 WiFi Setup</title>"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<style>body{font:-apple-system,Segoe UI,sans-serif;max-width:28em;margin:1.5em auto;padding:0 1em;color:#222;background:#f7f6f3}"
"h1{color:#7a3e00;margin:0 0 .2em}label{display:block;margin:.9em 0 .25em;font-weight:600;font-size:.9em}"
"input,select{width:100%;padding:.45em;font:inherit;border:1px solid #e1ddd2;border-radius:4px;background:#fff}"
"button{margin-top:1.2em;width:100%;padding:.7em;background:#7a3e00;color:#fff;border:0;border-radius:5px;font:inherit;font-weight:600;cursor:pointer}"
"button:hover{background:#5a2d00}p{color:#777;font-size:.9em}</style></head><body>"
"<h1>SixBack \xe2\x80\x94 WiFi Setup</h1>"
"<p>Pick your home network and enter its password.</p>"
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
"<title>SixBack \xe2\x80\x94 Connected</title>"
"<meta http-equiv=\"refresh\" content=\"20;url=http://");
    h += staIp;
    h += F("/\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<style>body{font:-apple-system,Segoe UI,sans-serif;max-width:28em;margin:1.5em auto;padding:0 1em;color:#222;background:#f7f6f3}"
"h1{color:#1f7a3a;margin:0 0 .3em}p{color:#444}code{background:#fee;padding:.1em .35em;border-radius:3px}"
"a{color:#7a3e00}</style></head><body><h1>Connected!</h1>"
"<p>SixBack is now on your LAN at <a href=\"http://");
    h += staIp;
    h += F("/\">http://");
    h += staIp;
    h += F("/</a> (or <code>http://sixback.local/</code>).</p>"
"<p><b>Switch your phone back to your normal Wi-Fi.</b> This page will "
"redirect there automatically in 20 seconds \xe2\x80\x94 which of course "
"only works once you are on the right network.</p>"
"<p style=\"color:#777;font-size:.85em\">SixBack " FW_VERSION_STRING "</p>"
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
    // ASYNC scan + Polling im Handler.  Sync-scanNetworks() blockiert auf
    // arduino-esp32 im AP_STA- (Captive aktiv) oder STA-connected-Mode
    // unbestimmt lange — wir haben live verifiziert dass er > 60 s nicht
    // zurueckkehrt.  Async + scanComplete()-Polling liefert die Liste in
    // ~4 s zuverlaessig.
    WiFi.scanDelete();
    delay(50);
    WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
    uint32_t t0 = millis();
    int n = WIFI_SCAN_RUNNING;
    while (millis() - t0 < 15000) {
        n = WiFi.scanComplete();
        if (n >= 0 || n == WIFI_SCAN_FAILED) break;
        delay(200);
    }
    String body = "{\"networks\":[";
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            if (i) body += ",";
            String ssid = WiFi.SSID(i);
            ssid.replace("\"", "\\\"");
            body += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i))
                  + ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false")
                  + "}";
        }
    }
    body += "],\"rc\":" + String(n) + "}";
    WiFi.scanDelete();
    req->send(200, "application/json", body);
}

String progressHtml(const String& ssid) {
    String h = F(
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<title>SixBack \xe2\x80\x94 Connecting</title>"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<style>body{font:-apple-system,Segoe UI,sans-serif;max-width:28em;margin:1.5em auto;padding:0 1em;color:#222;background:#f7f6f3}"
"h1{color:#7a3e00;margin:0 0 .3em}p{color:#444}"
"#status{font-size:1.05em;margin:1em 0;padding:.7em;border-radius:5px;background:#fff;border:1px solid #e1ddd2}"
".ok{color:#1f7a3a}.err{color:#a32525}.busy{color:#7a3e00}"
"a{color:#7a3e00}</style></head><body>"
"<h1>Connecting\xe2\x80\xa6</h1>"
"<p>SixBack is associating with <b>");
    h += ssid;
    h += F("</b>. This may take up to 20 seconds.</p>"
"<div id=\"status\" class=\"busy\">\xe2\x8f\xb3 connecting\xe2\x80\xa6</div>"
"<p><a href=\"/\">Back to setup form</a></p>"
"<script>"
"async function poll(){"
" try{const r=await fetch('/save_status',{cache:'no-store'});const d=await r.json();"
"   const s=document.getElementById('status');"
"   if(d.state==='success'){s.className='ok';s.textContent='\xe2\x9c\x93 connected as '+d.sta_ip+' \xe2\x80\x94 loading\xe2\x80\xa6';setTimeout(()=>location.href='/',800);return;}"
"   if(d.state==='failed'){s.className='err';s.textContent='\xe2\x9c\x97 connect failed \xe2\x80\x94 wrong password? Use Back to retry.';return;}"
"   s.textContent='\xe2\x8f\xb3 connecting ('+d.elapsed_s+'s)\xe2\x80\xa6';"
" }catch(e){/* AP-channel-hop \xe2\x80\x94 retry */}"
" setTimeout(poll,1500);"
"}"
"poll();"
"</script></body></html>");
    return h;
}

void handleSave(AsyncWebServerRequest* req) {
    if (!req->hasParam("ssid", true)) {
        req->send(400, "text/plain", "Missing ssid");
        return;
    }
    String ssid = req->getParam("ssid", true)->value();
    String psk  = req->hasParam("psk", true) ? req->getParam("psk", true)->value() : "";

    // Re-Submit waehrend bereits laufender Connect-Versuch: nur Progress-Page.
    // Bei Success/Failed/Idle starten wir einen neuen Versuch.
    if (saveState != SaveState::Connecting) {
        Serial.printf("[captive] async save ssid=%s\n", ssid.c_str());
        saveSsid    = ssid;
        savePsk     = psk;
        saveStartMs = millis();
        saveState   = SaveState::Connecting;
        // Non-blocking — captiveTick pollt WiFi.status() ab jetzt.
        WiFi.begin(ssid.c_str(), psk.c_str());
    } else {
        Serial.printf("[captive] save while already connecting (existing ssid=%s, new=%s)\n",
                      saveSsid.c_str(), ssid.c_str());
    }
    req->send(200, "text/html; charset=utf-8", progressHtml(saveSsid));
}

void handleSaveStatus(AsyncWebServerRequest* req) {
    const char* st = "idle";
    switch (saveState) {
        case SaveState::Idle:       st = "idle";       break;
        case SaveState::Connecting: st = "connecting"; break;
        case SaveState::Success:    st = "success";    break;
        case SaveState::Failed:     st = "failed";     break;
    }
    String body = "{\"state\":\"";
    body += st;
    body += "\",\"ssid\":\"";
    body += saveSsid;
    body += "\",\"sta_ip\":\"";
    body += provisionedSta;
    body += "\",\"elapsed_s\":";
    body += String(saveState == SaveState::Connecting ? (millis() - saveStartMs) / 1000 : 0);
    body += "}";
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", body);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
}

void handleCaptiveRedirect(AsyncWebServerRequest* req) {
    req->redirect("http://" + AP_IP.toString() + "/");
}

}  // namespace

void captiveStart() {
    if (active) return;

    uint8_t mac[6]; WiFi.macAddress(mac);
    char buf[24];
    snprintf(buf, sizeof(buf), "SixBack-%02X%02X%02X", mac[3], mac[4], mac[5]);
    apSsid = buf;

    Serial.printf("[captive] starting open AP '%s' + DNS + HTTP on %s\n",
                  apSsid.c_str(), AP_IP.toString().c_str());

    WiFi.mode(WIFI_AP_STA);   // STA bleibt parallel, damit improv weiter
    WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
    WiFi.softAP(apSsid.c_str());   // kein PSK — offener AP

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", AP_IP);

    captiveServer = new AsyncWebServer(CAPTIVE_PORT);
    captiveServer->on("^/$",                       HTTP_GET,  handleRoot);
    captiveServer->on("/scan",                     HTTP_GET,  handleScan);
    captiveServer->on("/save",                     HTTP_POST, handleSave);
    captiveServer->on("/save_status",              HTTP_GET,  handleSaveStatus);
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
    saveState = SaveState::Idle;
    saveSsid  = "";
    savePsk   = "";
    saveStartMs = 0;
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

    // Async-Save-State-Machine antreiben.
    if (saveState == SaveState::Connecting) {
        if (WiFi.status() == WL_CONNECTED) {
            provisionedSta = WiFi.localIP().toString();
            persistCreds(saveSsid, savePsk);
            wifiOptimizeForReliability();
            saveState = SaveState::Success;
            Serial.printf("[captive] async connect OK, sta-IP=%s\n", provisionedSta.c_str());
        } else if (millis() - saveStartMs > SAVE_TIMEOUT_MS) {
            Serial.println("[captive] async connect timeout");
            WiFi.disconnect(true);
            saveState = SaveState::Failed;
        }
    }

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

}  // namespace sixback
