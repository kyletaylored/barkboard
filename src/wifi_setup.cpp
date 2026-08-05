#include "wifi_setup.h"
#include "config.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>
#include <algorithm>

// Self-contained captive portal: AP + DNS hijack + WebServer.
// Stores creds in NVS namespace "wifi_cfg".

static String g_apSsid;
static netcfg::PortalEnterCb s_onPortal;
static netcfg::StatusCb      s_onStatus;
static volatile bool g_apActive  = false;
static volatile bool g_connected = false;

static const uint16_t DNS_PORT = 53;
static DNSServer  dns;
static WebServer  http(80);
static String     g_lastError;
static String     g_lastSsidTried;

static String makeApSsid() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[8];
    snprintf(buf, sizeof(buf), "%04X", (uint16_t)(mac & 0xFFFF));
    return String(AP_SSID_PREFIX) + buf;
}

static String htmlEscape(const String& s) {
    String o; o.reserve(s.length()+8);
    for (size_t i=0;i<s.length();++i){char c=s[i];
        switch(c){case '<':o+="&lt;";break;case '>':o+="&gt;";break;
        case '&':o+="&amp;";break;case '"':o+="&quot;";break;default:o+=c;}}
    return o;
}

// Escapes for embedding inside a single-quoted JS string literal (the list
// row onclick handlers) — distinct from htmlEscape, which is for text nodes.
static String jsEscape(const String& s) {
    String o; o.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '\'' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

struct ScannedNetwork { String ssid; int32_t rssi; bool secured; };

// ESP32 doesn't speak 5GHz at all, but dual-band routers broadcast the same
// SSID on both bands — without filtering, every such router shows up twice
// in the list with no way to tell which entry is actually joinable.
// Channels 1-14 are 2.4GHz; 5GHz starts at channel 36. Also dedupes repeat
// SSIDs (mesh nodes, multiple APs) down to the strongest signal.
static std::vector<ScannedNetwork> get24GhzNetworks(int scanCount) {
    std::vector<ScannedNetwork> out;
    for (int i = 0; i < scanCount && i < 40; ++i) {
        if (WiFi.channel(i) > 14) continue;   // 5GHz — ESP32 can't join it anyway
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;      // hidden network — nothing to click
        int32_t rssi = WiFi.RSSI(i);
        bool secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

        bool merged = false;
        for (ScannedNetwork& existing : out) {
            if (existing.ssid == ssid) {
                if (rssi > existing.rssi) existing.rssi = rssi;
                merged = true;
                break;
            }
        }
        if (!merged) out.push_back({ssid, rssi, secured});
        if (out.size() >= 20) break;
    }
    // Strongest signal first, same convention as the reference WiFiManager-style UI.
    std::sort(out.begin(), out.end(), [](const ScannedNetwork& a, const ScannedNetwork& b) {
        return a.rssi > b.rssi;
    });
    return out;
}

// Coarse 3-bar signal icon from RSSI, CSS-drawn (no image asset needed).
static String signalIconHtml(int32_t rssi) {
    int bars = (rssi > -60) ? 3 : (rssi > -75) ? 2 : 1;
    String h = "<span class=\"sig\">";
    for (int b = 1; b <= 3; ++b) {
        h += "<i class=\"" + String(b <= bars ? "on" : "") + "\"></i>";
    }
    h += "</span>";
    return h;
}

static String formPage(const String& msg, bool err) {
    int n = WiFi.scanComplete();
    // If never triggered or last attempt failed, kick a fresh scan.
    if (n == WIFI_SCAN_FAILED || n == -2) {
        WiFi.scanNetworks(true, false);
        n = WIFI_SCAN_RUNNING;
    }
    std::vector<ScannedNetwork> networks = (n > 0) ? get24GhzNetworks(n) : std::vector<ScannedNetwork>();

    String h;
    h.reserve(4096);
    h += F("<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>BarkBoard setup</title><style>"
           ":root{--bg:#0B0B10;--panel:#16151C;--ink:#F2F0F5;--mut:#B5B2C0;--accent:#8000FF;--accent2:#632CA6}"
           "body{font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;"
           "background:radial-gradient(ellipse at top,#1a0e2a,var(--bg));color:var(--ink);margin:0;padding:24px}"
           ".w{max-width:520px;margin:0 auto}"
           "h1{color:var(--accent2);font-size:22px;margin:6px 0}"
           "h2{color:var(--ink);font-size:15px;margin:22px 0 8px}"
           ".sub{color:var(--mut);font-size:13px;margin-bottom:18px}"
           ".card{background:var(--panel);border:1px solid #26242E;border-radius:14px;padding:6px;margin-bottom:14px}"
           ".card.pad{padding:18px}"
           "label{display:block;font-size:12px;letter-spacing:.12em;text-transform:uppercase;color:var(--mut);margin:12px 0 6px}"
           "input{width:100%;padding:11px 12px;border-radius:10px;border:1px solid #26242E;"
           "background:#0a0a0f;color:var(--ink);font-size:15px;outline:none;box-sizing:border-box}"
           "input:focus{border-color:var(--accent2)}"
           "button{appearance:none;border:0;background:linear-gradient(180deg,var(--accent),var(--accent2));"
           "color:#fff;font-weight:800;padding:12px;border-radius:10px;width:100%;font-size:15px;"
           "letter-spacing:.06em;text-transform:uppercase;cursor:pointer;margin-top:16px}"
           "button.secondary{background:#26242E;color:var(--ink);font-weight:600;margin-top:10px}"
           ".banner{padding:10px 12px;border-radius:10px;margin-bottom:12px;font-size:13px}"
           ".ok{background:#173523;color:#aef0c0;border:1px solid #235a38}"
           ".err{background:#3a1620;color:#ffb6bb;border:1px solid #5a1d2a}"
           ".small{color:var(--mut);font-size:12px;margin-top:8px}"
           ".scan-state{color:var(--mut);font-size:12px;margin:6px 0 0}"
           // Clickable network row — matches the classic WiFiManager-style
           // list (icon + name, lock if secured) rather than a <select>.
           ".net{display:flex;align-items:center;gap:10px;padding:11px 12px;cursor:pointer;"
           "border-radius:8px;background:none;border:0;width:100%;text-align:left;color:var(--ink);font-size:15px}"
           ".net:active{background:#1e1c28}"
           ".net+.net{border-top:1px solid #26242E}"
           ".net .ssid{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
           ".net .lock{color:var(--mut);font-size:13px}"
           ".sig{display:inline-flex;align-items:flex-end;gap:2px;height:12px}"
           ".sig i{width:3px;background:#26242E;border-radius:1px;display:block}"
           ".sig i:nth-child(1){height:5px}.sig i:nth-child(2){height:8px}.sig i:nth-child(3){height:12px}"
           ".sig i.on{background:var(--accent2)}"
           ".chk{display:flex;align-items:center;gap:8px;text-transform:none;letter-spacing:0;"
           "font-size:13px;color:var(--mut);margin:10px 0 0}"
           ".chk input{width:auto;margin:0}"
           "</style></head><body><div class=\"w\">"
           "<h1>WiFi Networks</h1>"
           "<div class=\"sub\">BarkBoard &middot; first-time setup</div>");

    if (msg.length()) {
        h += "<div class=\"banner ";
        h += err ? "err" : "ok";
        h += "\">";
        h += htmlEscape(msg);
        h += "</div>";
    }

    h += "<div class=\"card\">";
    if (n == WIFI_SCAN_RUNNING) {
        h += "<p class=\"scan-state\" style=\"margin:12px\">Scanning networks&hellip; refresh in a few seconds.</p>";
    } else if (networks.empty()) {
        h += "<p class=\"scan-state\" style=\"margin:12px\">No 2.4GHz networks found. Rescan, or type your network's name below.</p>";
    } else {
        for (const ScannedNetwork& net : networks) {
            h += "<button type=\"button\" class=\"net\" onclick=\"selectNet('" + jsEscape(net.ssid) + "')\">";
            h += signalIconHtml(net.rssi);
            h += "<span class=\"ssid\">" + htmlEscape(net.ssid) + "</span>";
            if (net.secured) h += "<span class=\"lock\">&#128274;</span>";
            h += "</button>";
        }
    }
    h += "</div>";

    h += F("<h2>WiFi Settings</h2>"
           "<form method=\"POST\" action=\"/wifi\" class=\"card pad\">"
           "<input type=\"text\" id=\"ssid\" name=\"ssid\" placeholder=\"SSID\" autocomplete=\"off\" required>"
           "<input type=\"password\" id=\"pass\" name=\"pass\" placeholder=\"Password\" autocomplete=\"new-password\" style=\"margin-top:10px\">"
           "<label class=\"chk\"><input type=\"checkbox\" onclick=\""
           "document.getElementById('pass').type=this.checked?'text':'password'\"> Show password</label>"
           "<button type=\"submit\">Save</button>"
           "<div class=\"small\">2.4 GHz networks only — ESP32 doesn't speak 5 GHz.</div>"
           "</form>"
           "<form method=\"POST\" action=\"/rescan\" class=\"card pad\">"
           "<button type=\"submit\" class=\"secondary\">Rescan networks</button>"
           "</form>"
           "<script>function selectNet(s){"
           "document.getElementById('ssid').value=s;"
           "document.getElementById('pass').focus();"
           "}</script>"
           "</div></body></html>");
    return h;
}

static void handleRoot()  { http.send(200, "text/html", formPage(g_lastError, g_lastError.length() > 0)); g_lastError = ""; }
static void redirectRoot(){ http.sendHeader("Location", "http://192.168.4.1/"); http.send(302, "text/plain", ""); }
static void handleRescan(){
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false);
    http.sendHeader("Location", "/"); http.send(302, "text/plain", "");
}

static void saveCreds(const String& ssid, const String& pass) {
    Preferences p; p.begin("wifi_cfg", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
}
static bool loadCreds(String& ssid, String& pass) {
    Preferences p; p.begin("wifi_cfg", true);
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
    return ssid.length() > 0;
}

static bool tryConnect(const String& ssid, const String& pass, uint32_t timeoutMs) {
    Serial.printf("[wifi] connecting to '%s' ...\n", ssid.c_str());
    WiFi.disconnect(false, false);
    delay(50);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    wl_status_t st = WL_IDLE_STATUS;
    while (millis() - start < timeoutMs) {
        st = WiFi.status();
        if (st == WL_CONNECTED) {
            Serial.printf("[wifi] connected ip=%s\n", WiFi.localIP().toString().c_str());
            return true;
        }
        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            Serial.printf("[wifi] connect failed status=%d\n", (int)st);
            break;
        }
        delay(150);
    }
    Serial.printf("[wifi] connect timed out, last status=%d\n", (int)WiFi.status());
    return false;
}

static void handleSave() {
    String ssid = http.arg("ssid");
    String pass = http.arg("pass");
    ssid.trim();
    g_lastSsidTried = ssid;

    if (ssid.length() == 0) {
        g_lastError = "SSID is empty.";
        http.sendHeader("Location", "/"); http.send(302, "text/plain", "");
        return;
    }

    // Reply IMMEDIATELY before we tear down AP, so the phone gets the page.
    String pre = String("Connecting to ") + ssid + "...";
    String pageMsg = "Saved. Trying to connect to '" + ssid + "'. The device will switch off this WiFi.";
    http.send(200, "text/html",
        "<html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"4;url=/\"></head>"
        "<body style=\"background:#0B0B10;color:#F2F0F5;font-family:sans-serif;padding:24px\">"
        "<h1 style=\"color:#632CA6\">" + htmlEscape(pre) + "</h1>"
        "<p>" + htmlEscape(pageMsg) + "</p></body></html>");
    delay(200);

    saveCreds(ssid, pass);

    // Stop AP/portal so STA can take the radio.
    dns.stop();
    http.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_apActive = false;

    if (tryConnect(ssid, pass, 25000)) {
        g_connected = true;
        if (s_onStatus) s_onStatus(String("Connected to ") + ssid);
        return;  // task ends; main loop sees connected=true
    }

    // Failed — reboot to start a fresh portal session with new attempt.
    Serial.println("[wifi] connect failed, rebooting to retry portal");
    delay(800);
    ESP.restart();
}

static void portalLoop() {
    g_apActive = true;
    Serial.printf("[wifi] AP up: %s ip=%s\n",
                  g_apSsid.c_str(), WiFi.softAPIP().toString().c_str());
    if (s_onPortal) s_onPortal(g_apSsid, "");   // "" signals an open AP — no password to relay

    // Async scan; passive=false so the radio actively probes. show_hidden=false,
    // 300ms/channel keeps the AP responsive while still finding most networks.
    WiFi.scanNetworks(true, false, false, 300);

    dns.start(DNS_PORT, "*", WiFi.softAPIP());

    http.on("/",                HTTP_GET,  handleRoot);
    http.on("/wifi",            HTTP_POST, handleSave);
    http.on("/rescan",          HTTP_POST, handleRescan);
    // Captive-portal redirect endpoints used by phones to detect login walls
    http.on("/generate_204",    HTTP_GET,  redirectRoot);
    http.on("/gen_204",         HTTP_GET,  redirectRoot);
    http.on("/hotspot-detect.html", HTTP_GET, handleRoot);
    http.on("/library/test/success.html", HTTP_GET, handleRoot);
    http.on("/connecttest.txt", HTTP_GET, redirectRoot);
    http.on("/ncsi.txt",        HTTP_GET, redirectRoot);
    http.on("/redirect",        HTTP_GET, redirectRoot);
    http.onNotFound([](){ http.sendHeader("Location", "http://192.168.4.1/"); http.send(302, "text/plain", ""); });
    http.begin();

    uint32_t lastScanKick = millis();
    while (!g_connected) {
        dns.processNextRequest();
        http.handleClient();
        // Self-heal the scan: if it failed/finished-empty, kick it again every 8s.
        if (millis() - lastScanKick > 8000) {
            int n = WiFi.scanComplete();
            if (n == WIFI_SCAN_FAILED || n == -2 || n == 0) {
                WiFi.scanDelete();
                WiFi.scanNetworks(true, false, false, 300);
            }
            lastScanKick = millis();
        }
        delay(2);
    }
}

static void wifiTask(void*) {
    if (s_onStatus) s_onStatus("Connecting WiFi...");

    // 1. Try saved creds (with retry — first attempt often times out).
    String ssid, pass;
    if (loadCreds(ssid, pass)) {
        if (s_onStatus) s_onStatus(String("Connecting to ") + ssid);
        WiFi.mode(WIFI_STA);
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (attempt > 0) {
                Serial.printf("[wifi] retry %d\n", attempt);
                delay(1500);
            }
            if (tryConnect(ssid, pass, 25000)) {
                g_connected = true;
                if (s_onStatus) s_onStatus(String("Connected to ") + ssid);
                vTaskDelete(NULL);
                return;
            }
        }
        Serial.println("[wifi] saved creds failed after retries, opening portal");
    } else {
        Serial.println("[wifi] no saved creds, opening portal");
    }

    // 2. Open portal. No password — see config.h's AP_SSID_PREFIX comment.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(g_apSsid.c_str());
    delay(200);
    portalLoop();   // blocks until g_connected

    if (s_onStatus) s_onStatus("Connected");
    vTaskDelete(NULL);
}

void netcfg::begin(PortalEnterCb onPortal, StatusCb onStatus) {
    g_apSsid   = makeApSsid();
    s_onPortal = onPortal;
    s_onStatus = onStatus;
    g_connected = false;
    g_apActive  = false;
    xTaskCreatePinnedToCore(wifiTask, "wifi", 8192, nullptr, 1, nullptr, 0);
}

void netcfg::process() {
    if (!g_connected && WiFi.status() == WL_CONNECTED) g_connected = true;
}

String netcfg::apSsid()         { return g_apSsid.length() ? g_apSsid : makeApSsid(); }
bool   netcfg::isConnected()    { return g_connected || WiFi.status() == WL_CONNECTED; }
bool   netcfg::isPortalActive() { return g_apActive; }
