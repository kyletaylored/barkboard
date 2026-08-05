#include "portal.h"
#include "config.h"
#include "storage.h"
#include "datadog.h"

#include <WiFi.h>
#include <WebServer.h>

static WebServer server(PORTAL_HTTP_PORT);

// Same 18x20 Bits mark used on-device (assets/bits_icon_small.png) —
// embedded as a data URI so the tab icon shows up without an extra request,
// and also served as real bytes at /favicon.ico below for browsers that
// fetch that directly regardless of the <link> tag.
static const char PAGE_HEAD[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BarkBoard</title>
<link rel="icon" type="image/png" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABIAAAAUCAYAAACAl21KAAACZ0lEQVR4nHWUS4iPURjGf2dmiEbuksskSVmwmo2FskCTkpJGURY2NqJkMwu5lJKNLORSrCgLIRaiZKNkipIsFLlNM+6XpnEbM/PTO/P+xzfDvPX1nfN95zzned/neQ9qAVCnqRMYI9Qj6nt1e87r/rdon9qp3lLnq1PUPep5dbd6zKG4rvapLbmvvgrS5t94rnapb/w3etS96ok8tG4EK7Vb7VX71Xvqo9z4K999OY7/7cm2VR1fzSoQbwLjctwEtAc+EAu7c12Mo5Z3geWllIvA79FA24D48RE4CjzJTWeB6cDlGnngQ4CqDWMpUq8eVHepHZnSSXWD+lAdyBQjfqgLRytXUv444Tkwfyz5GWLUmcyD9VZgIL6XUhyWUL2WJ0bhazFQEeKLuijXPlWbMpMhVjkJZqvVTxWQrpS8Gq/VB+rLUaUpNVfXlVLi9OXAbWAi8Am4CjQCa4Be4DgwA/gJdAHLgGbg3GD1E6ShlBI+egCsAD4DT4HXwCXgO7AJWAXMScXJA2cNAmWeUYeYz8zCvgPmAoeBXylIrSX6KkA7gaVh8xKMBisPp4El6aOmNF1/BaQ3lVoJtAFX8hlmNAm4kAtuJFiw+ZYnB/BXYCpwqpQSDr+bmQy2Sm3RGWBdfmxOV3cAk/P//SzsemDvsAmHMonC94TsUZO3FTXr03ABPhvYCLSUUp5V5I57ax4QvgqXt0Zqoc4hYH8WMZ5I7XEyixQb1c3AgpQ/mjyYhCCvgB0BFMU+oC4GtlS6elwp5av6AlibNriTG9+UUkKEEa4M5cLdYfn7efdEu0z873U6stEbam3yB+i4DQ1sJV6WAAAAAElFTkSuQmCC">
<style>
:root{--bg:#0B0B10;--panel:#16151C;--ink:#F2F0F5;--mut:#B5B2C0;--ok:#3FB950;--err:#F0506E;--accent:#632CA6}
*{box-sizing:border-box}
body{margin:0;font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:radial-gradient(ellipse at top,#1a0e2a,#0B0B10);color:var(--ink);min-height:100vh}
.wrap{max-width:520px;margin:0 auto;padding:28px 18px}
.brand{display:flex;align-items:center;gap:10px;margin-bottom:6px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--ok);box-shadow:0 0 12px var(--ok)}
.brand h1{font-size:14px;letter-spacing:.18em;text-transform:uppercase;color:var(--mut);margin:0}
.title{font-weight:800;font-size:30px;letter-spacing:-.5px;color:var(--accent);margin:6px 0 4px}
.sub{color:var(--mut);font-size:13px;margin-bottom:22px}
.card{background:var(--panel);border:1px solid #26242E;border-radius:14px;padding:18px}
label{display:block;font-size:12px;letter-spacing:.12em;text-transform:uppercase;color:var(--mut);margin:14px 0 6px}
input[type=text],input[type=password]{
 width:100%;padding:12px 14px;border-radius:10px;border:1px solid #26242E;background:#0a0a0f;color:var(--ink);font-size:15px;outline:none;font-family:monospace}
input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(99,44,166,.25)}
button{appearance:none;border:0;background:linear-gradient(180deg,#8000FF,#632CA6);color:#fff;font-weight:800;
 padding:12px 16px;border-radius:10px;cursor:pointer;width:100%;margin-top:18px;font-size:15px;letter-spacing:.06em;text-transform:uppercase}
button.secondary{background:#26242E;color:var(--ink);font-weight:600}
.note{margin-top:16px;color:var(--mut);font-size:12px;line-height:1.5}
.kvs{display:grid;grid-template-columns:auto 1fr;gap:6px 14px;font-size:13px;color:var(--ink)}
.kvs b{color:var(--mut);font-weight:500}
.banner{margin:12px 0;padding:10px 12px;border-radius:10px;background:#173523;color:#aef0c0;border:1px solid #235a38;font-size:13px}
.banner.err{background:#3a1620;color:#ffb6bb;border-color:#5a1d2a}
hr{border:0;border-top:1px solid #26242E;margin:18px 0}
small{color:var(--mut)}
.chk{display:flex;align-items:center;gap:8px;text-transform:none;letter-spacing:0;font-size:13px;color:var(--mut);margin:10px 0 0}
.chk input{width:auto;margin:0}
select{width:100%;padding:11px 12px;border-radius:10px;border:1px solid #26242E;background:#0a0a0f;color:var(--ink);font-size:15px}
.section{font-size:12px;letter-spacing:.12em;text-transform:uppercase;color:var(--mut);margin:0 0 10px;display:flex;align-items:center;justify-content:space-between}
.pill{font-size:11px;letter-spacing:.04em;text-transform:none;padding:3px 9px;border-radius:100px;background:#173523;color:#aef0c0}
.danger{border-color:#3a1620}
.danger button.secondary{background:#3a1620;color:#ffb6bb}
.linklike{background:none;border:0;color:var(--accent);font-size:13px;font-weight:700;text-transform:none;letter-spacing:0;padding:0;margin:0;width:auto;cursor:pointer}
</style></head><body><div class="wrap">
<div class="brand"><span class="dot"></span><h1>BarkBoard &middot; setup</h1></div>
)HTML";

// Raw bytes of the same favicon (assets/bits_icon_small.png) — served at
// /favicon.ico directly, for browsers that fetch that path regardless of
// the <link rel="icon"> data URI above.
static const uint8_t FAVICON_PNG[] PROGMEM = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x14, 0x08, 0x06, 0x00, 0x00, 0x00, 0x80, 0x97, 0x6d,
    0x4a, 0x00, 0x00, 0x02, 0x67, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x75, 0x94, 0x4b, 0x88, 0x8f,
    0x51, 0x18, 0xc6, 0x7f, 0x67, 0x66, 0x88, 0x46, 0xee, 0x92, 0xcb, 0x24, 0x49, 0x59, 0xb0, 0x9a,
    0x8d, 0x85, 0xb2, 0x40, 0x93, 0x92, 0x92, 0x46, 0x51, 0x16, 0x36, 0x36, 0xa2, 0x64, 0x33, 0x0b,
    0xb9, 0x94, 0x92, 0x8d, 0x2c, 0xe4, 0x52, 0xac, 0x28, 0x0b, 0x21, 0x16, 0xa2, 0x64, 0xa3, 0x64,
    0x8a, 0x92, 0x2c, 0x14, 0xb9, 0x4d, 0x33, 0xee, 0x97, 0xa6, 0x71, 0x1b, 0x33, 0xf3, 0xd3, 0x3b,
    0xf3, 0xfe, 0xc7, 0x37, 0xc3, 0xbc, 0xf5, 0xf5, 0x9d, 0xf3, 0x7d, 0xe7, 0x3c, 0xe7, 0x79, 0xdf,
    0xe7, 0x79, 0x0f, 0x6a, 0x01, 0x50, 0xa7, 0xa9, 0x13, 0x18, 0x23, 0xd4, 0x23, 0xea, 0x7b, 0x75,
    0x7b, 0xce, 0xeb, 0xfe, 0xb7, 0x68, 0x9f, 0xda, 0xa9, 0xde, 0x52, 0xe7, 0xab, 0x53, 0xd4, 0x3d,
    0xea, 0x79, 0x75, 0xb7, 0x7a, 0xcc, 0xa1, 0xb8, 0xae, 0xf6, 0xa9, 0x2d, 0xb9, 0xaf, 0xbe, 0x0a,
    0xd2, 0xe6, 0xdf, 0x78, 0xae, 0x76, 0xa9, 0x6f, 0xfc, 0x37, 0x7a, 0xd4, 0xbd, 0xea, 0x89, 0x3c,
    0xb4, 0x6e, 0x04, 0x2b, 0xb5, 0x5b, 0xed, 0x55, 0xfb, 0xd5, 0x7b, 0xea, 0xa3, 0xdc, 0xf8, 0x2b,
    0xdf, 0x7d, 0x39, 0x8e, 0xff, 0xed, 0xc9, 0xb6, 0x55, 0x1d, 0x5f, 0xcd, 0x2a, 0x10, 0x6f, 0x02,
    0xe3, 0x72, 0xdc, 0x04, 0xb4, 0x07, 0x3e, 0x10, 0x0b, 0xbb, 0x73, 0x5d, 0x8c, 0xa3, 0x96, 0x77,
    0x81, 0xe5, 0xa5, 0x94, 0x8b, 0xc0, 0xef, 0xd1, 0x40, 0xdb, 0x80, 0xf8, 0xf1, 0x11, 0x38, 0x0a,
    0x3c, 0xc9, 0x4d, 0x67, 0x81, 0xe9, 0xc0, 0xe5, 0x1a, 0x79, 0xe0, 0x43, 0x80, 0xaa, 0x0d, 0x63,
    0x29, 0x52, 0xaf, 0x1e, 0x54, 0x77, 0xa9, 0x1d, 0x99, 0xd2, 0x49, 0x75, 0x83, 0xfa, 0x50, 0x1d,
    0xc8, 0x14, 0x23, 0x7e, 0xa8, 0x0b, 0x47, 0x2b, 0x57, 0x52, 0xfe, 0x38, 0xe1, 0x39, 0x30, 0x7f,
    0x2c, 0xf9, 0x19, 0x62, 0xd4, 0x99, 0xcc, 0x83, 0xf5, 0x56, 0x60, 0x20, 0xbe, 0x97, 0x52, 0x1c,
    0x96, 0x50, 0xbd, 0x96, 0x27, 0x46, 0xe1, 0x6b, 0x31, 0x50, 0x11, 0xe2, 0x8b, 0xba, 0x28, 0xd7,
    0x3e, 0x55, 0x9b, 0x32, 0x93, 0x21, 0x56, 0x39, 0x09, 0x66, 0xab, 0xd5, 0x4f, 0x15, 0x90, 0xae,
    0x94, 0xbc, 0x1a, 0xaf, 0xd5, 0x07, 0xea, 0xcb, 0x51, 0xa5, 0x29, 0x35, 0x57, 0xd7, 0x95, 0x52,
    0xe2, 0xf4, 0xe5, 0xc0, 0x6d, 0x60, 0x22, 0xf0, 0x09, 0xb8, 0x0a, 0x34, 0x02, 0x6b, 0x80, 0x5e,
    0xe0, 0x38, 0x30, 0x03, 0xf8, 0x09, 0x74, 0x01, 0xcb, 0x80, 0x66, 0xe0, 0xdc, 0x60, 0xf5, 0x13,
    0xa4, 0xa1, 0x94, 0x12, 0x3e, 0x7a, 0x00, 0xac, 0x00, 0x3e, 0x03, 0x4f, 0x81, 0xd7, 0xc0, 0x25,
    0xe0, 0x3b, 0xb0, 0x09, 0x58, 0x05, 0xcc, 0x49, 0xc5, 0xc9, 0x03, 0x67, 0x0d, 0x02, 0x65, 0x9e,
    0x51, 0x87, 0x98, 0xcf, 0xcc, 0xc2, 0xbe, 0x03, 0xe6, 0x02, 0x87, 0x81, 0x5f, 0x29, 0x48, 0xad,
    0x25, 0xfa, 0x2a, 0x40, 0x3b, 0x81, 0xa5, 0x61, 0xf3, 0x12, 0x8c, 0x06, 0x2b, 0x0f, 0xa7, 0x81,
    0x25, 0xe9, 0xa3, 0xa6, 0x34, 0x5d, 0x7f, 0x05, 0xa4, 0x37, 0x95, 0x5a, 0x09, 0xb4, 0x01, 0x57,
    0xf2, 0x19, 0x66, 0x34, 0x09, 0xb8, 0x90, 0x0b, 0x6e, 0x24, 0x58, 0xb0, 0xf9, 0x96, 0x27, 0x07,
    0xf0, 0x57, 0x60, 0x2a, 0x70, 0xaa, 0x94, 0x12, 0x0e, 0xbf, 0x9b, 0x99, 0x0c, 0xb6, 0x4a, 0x6d,
    0xd1, 0x19, 0x60, 0x5d, 0x7e, 0x6c, 0x4e, 0x57, 0x77, 0x00, 0x93, 0xf3, 0xff, 0xfd, 0x2c, 0xec,
    0x7a, 0x60, 0xef, 0xb0, 0x09, 0x87, 0x32, 0x89, 0xc2, 0xf7, 0x84, 0xec, 0x51, 0x93, 0xb7, 0x15,
    0x35, 0xeb, 0xd3, 0x70, 0x01, 0x3e, 0x1b, 0xd8, 0x08, 0xb4, 0x94, 0x52, 0x9e, 0x55, 0xe4, 0x8e,
    0x7b, 0x6b, 0x1e, 0x10, 0xbe, 0x0a, 0x97, 0xb7, 0x46, 0x6a, 0xa1, 0xce, 0x21, 0x60, 0x7f, 0x16,
    0x31, 0x9e, 0x48, 0xed, 0x71, 0x32, 0x8b, 0x14, 0x1b, 0xd5, 0xcd, 0xc0, 0x82, 0x94, 0x3f, 0x9a,
    0x3c, 0x98, 0x84, 0x20, 0xaf, 0x80, 0x1d, 0x01, 0x14, 0xc5, 0x3e, 0xa0, 0x2e, 0x06, 0xb6, 0x54,
    0xba, 0x7a, 0x5c, 0x29, 0xe5, 0xab, 0xfa, 0x02, 0x58, 0x9b, 0x36, 0xb8, 0x93, 0x1b, 0xdf, 0x94,
    0x52, 0x42, 0x84, 0x11, 0xae, 0x0c, 0xe5, 0xc2, 0xdd, 0x61, 0xf9, 0xfb, 0x79, 0xf7, 0x44, 0xbb,
    0x4c, 0xfc, 0xef, 0x75, 0x3a, 0xb2, 0xd1, 0x1b, 0x6a, 0x6d, 0xf2, 0x07, 0xe8, 0xb8, 0x0d, 0x0d,
    0x6c, 0x25, 0x5e, 0x96, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
static const size_t FAVICON_PNG_LEN = sizeof(FAVICON_PNG);

static const char PAGE_FOOT[] PROGMEM = R"HTML(
</div></body></html>
)HTML";

static String htmlEscape(const String& s) {
    String o; o.reserve(s.length()+8);
    for (size_t i=0;i<s.length();++i){char c=s[i];
        switch(c){case '<':o+="&lt;";break;case '>':o+="&gt;";break;
        case '&':o+="&amp;";break;case '"':o+="&quot;";break;default:o+=c;}}
    return o;
}

static void sendStatus(const String& banner, bool err=false) {
    bool keysSet = storage::hasKeys();
    // Editing the keys is rare once they're set — a first-time visitor
    // needs the form front and center, but a returning visitor who just
    // wants to tweak Team/Clock/LED shouldn't have to scroll past a form
    // for keys that are already saved. Force it open with ?edit=keys.
    bool showKeyForm = !keysSet || server.hasArg("edit");

    String html = FPSTR(PAGE_HEAD);
    if (banner.length()) {
        html += String("<div class=\"banner ") + (err?"err":"") + "\">" + htmlEscape(banner) + "</div>";
    }

    // Status first — the thing you actually look at this page to check.
    html += "<div class=\"card\"><div class=\"kvs\">";
    html += "<b>SSID</b><span>" + htmlEscape(WiFi.SSID()) + "</span>";
    html += "<b>IP</b><span>" + WiFi.localIP().toString() + "</span>";
    html += "<b>RSSI</b><span>" + String(WiFi.RSSI()) + " dBm</span>";
    String site = storage::getSite();
    html += "<b>Site</b><span>" + (site.length() ? htmlEscape(site) : String("not detected yet")) + "</span>";
    html += "</div></div>";

    html += "<hr><div class=\"card\">";
    html += "<div class=\"section\">Datadog Keys<span class=\"pill\">" +
            String(keysSet ? "&check; set" : "not set") + "</span></div>";
    if (showKeyForm) {
        html += "<form method=\"POST\" action=\"/save\">";
        html += "<label>API Key</label><input type=\"password\" id=\"api_key\" name=\"api_key\" placeholder=\"32-char hex string\" autocomplete=\"off\" required>";
        html += "<label>Application Key</label><input type=\"password\" id=\"app_key\" name=\"app_key\" placeholder=\"40-char hex string\" autocomplete=\"off\" required>";
        html += "<label class=\"chk\"><input type=\"checkbox\" onclick=\""
                "document.getElementById('api_key').type=this.checked?'text':'password';"
                "document.getElementById('app_key').type=this.checked?'text':'password'\"> Show keys</label>";
        html += "<div class=\"note\" style=\"margin-top:6px\">Both keys come from <i>Organization Settings &rarr; API Keys / Application Keys</i>. "
                "The Application Key needs read access to Monitors, Incidents, On-Call, SLOs, and Hosts.</div>";
        html += "<button type=\"submit\">Save</button>";
        html += "</form>";
    } else {
        html += "<div class=\"sub\" style=\"margin-bottom:0\">Both keys are saved on this device. "
                "<button type=\"button\" class=\"linklike\" onclick=\"location.href='/?edit=keys'\">Change keys</button></div>";
    }
    html += "</div>";

    html += "<hr><div class=\"card\">";
    html += "<div class=\"section\">Preferences</div>";
    html += "<form method=\"POST\" action=\"/save-prefs\">";
    html += "<label>Team <small>(optional)</small></label>";
    html += "<input type=\"text\" name=\"team\" placeholder=\"my-team\" value=\"" + htmlEscape(storage::getTeamScope()) + "\" style=\"font-family:monospace\">";
    html += "<div class=\"note\" style=\"margin-top:6px\">Applied to Monitors, Incidents, and SLOs (each uses "
            "Datadog's own field naming under the hood, e.g. monitors' <code>team:</code> tag vs. incidents' "
            "<code>teams:</code> field, so you don't have to know which is which) — On-Call has its own separate "
            "team below, since it's about your account, not a filter. Leave this blank and the device "
            "auto-detects your team from your API key, same as On-Call; type one here to override it.</div>";
    html += "<label style=\"margin-top:16px\">Poll interval</label>";
    int pollSec = storage::getPollIntervalSec();
    html += "<select name=\"poll_sec\">";
    for (int secs : {30, 60, 120, 300}) {
        String labelText = secs < 60 ? (String(secs) + "s")
                          : (String(secs / 60) + (secs == 60 ? " minute" : " minutes"));
        html += String("<option value=\"") + secs + "\"" + (pollSec == secs ? " selected" : "") + ">" + labelText + "</option>";
    }
    html += "</select>";
    html += "<div class=\"note\" style=\"margin-top:6px\">How often the dashboard re-fetches monitors/incidents/on-call/SLOs. Shorter means fresher data but more API calls.</div>";
    html += "<label style=\"margin-top:16px\">Clock format</label>";
    bool is24h = storage::getTimeFormat24h();
    html += "<select name=\"time_format\">";
    html += String("<option value=\"24\"") + (is24h ? " selected" : "") + ">24-hour (14:30)</option>";
    html += String("<option value=\"12\"") + (!is24h ? " selected" : "") + ">12-hour (2:30 PM)</option>";
    html += "</select>";
    html += "<label style=\"margin-top:16px\">Status LED</label>";
    bool ledBreathe = storage::getLedBreatheEnabled();
    html += "<select name=\"led_style\">";
    html += String("<option value=\"breathe\"") + (ledBreathe ? " selected" : "") + ">Purple breathing when healthy</option>";
    html += String("<option value=\"solid\"") + (!ledBreathe ? " selected" : "") + ">Solid green when healthy</option>";
    html += "</select>";
    html += "<div class=\"note\" style=\"margin-top:6px\">Either way, Warn/Alert still show as solid yellow/red — this only changes the all-clear look.</div>";
    html += "<button type=\"submit\">Save preferences</button>";
    html += "</form></div>";

    html += "<hr><div class=\"card\">";
    html += "<div class=\"section\">On-Call Team</div>";
    String ocTeamId = storage::getOnCallTeamId();
    html += "<div class=\"sub\" style=\"margin-bottom:0\">";
    html += ocTeamId.length()
                ? ("Auto-detected from your API key. <b>Team id:</b> " + htmlEscape(ocTeamId))
                : String("Not yet detected — the device will auto-detect it from your API key the next time "
                         "the On-Call screen refreshes, or you can detect it here now.");
    html += " <button type=\"button\" class=\"linklike\" onclick=\"location.href='/oncall-team'\">"
            + String(ocTeamId.length() ? "Change" : "Detect now") + "</button></div>";
    html += "</div>";

    html += "<hr><div class=\"card danger\">";
    html += "<div class=\"section\">Danger Zone</div>";
    html += "<div class=\"sub\" style=\"margin-bottom:0\">Erases WiFi credentials and Datadog keys from this device — you'll need to go through setup again.</div>";
    html += "<form method=\"POST\" action=\"/forget\" style=\"margin-top:14px\">";
    html += "<button type=\"submit\" class=\"secondary\">Forget WiFi &amp; keys</button></form>";
    html += "</div>";

    html += "<p class=\"note\">Keys are stored in NVS on the device and never logged.</p>";
    html += FPSTR(PAGE_FOOT);
    server.send(200, "text/html", html);
}

static void handleRoot() {
    if (server.hasArg("prefs") && server.arg("prefs") == "saved") sendStatus("Preferences saved.", false);
    else sendStatus("", false);
}

// Keys to be validated (and site detected) by the main loop, not on this
// request thread — mirrors pagerduty-cyd's token-validation handoff pattern.
// Wired up to datadog::validateKeysAndDetectSite() once datadog.cpp exists
// (BARKBOARD_PLAN.md §2, §8 phase 2/3).
extern volatile bool   g_ddKeysJustSaved;
extern volatile bool   g_ddKeysValidated;
extern String          g_ddValidationError;

static void handleSave() {
    String apiKey = server.arg("api_key");
    String appKey = server.arg("app_key");
    apiKey.trim(); appKey.trim();
    if (apiKey.length() < 10 || appKey.length() < 10) {
        sendStatus("Both keys look too short — check for a copy/paste error.", true);
        return;
    }

    storage::setApiKey(apiKey);
    storage::setAppKey(appKey);
    g_ddKeysJustSaved   = true;
    g_ddKeysValidated   = false;
    g_ddValidationError = "";

    server.sendHeader("Location", "/saved");
    server.send(303, "text/plain", "");
}

static void handleSaved() {
    String banner;
    bool err = false;
    if (g_ddValidationError.length())      { banner = "Validation failed: " + g_ddValidationError; err = true; }
    else if (g_ddKeysValidated)            { banner = "Keys verified — site: " + storage::getSite() + ". Open the device."; }
    else                                    { banner = "Keys saved. Detecting your Datadog site... refresh in a few seconds."; }
    sendStatus(banner, err);
}

static void handleSavePrefs() {
    String team = server.arg("team");
    team.trim();
    storage::setTeamScope(team);
    storage::setTimeFormat24h(server.arg("time_format") != "12");
    bool ledBreathe = server.arg("led_style") != "solid";
    storage::setLedBreatheEnabled(ledBreathe);

    // Only accept one of the preset values the form actually offers — a
    // malformed/garbage value here (e.g. 0) would turn into a tight loop
    // hammering the Datadog API every main-loop tick instead of every N
    // seconds, so this fails closed to the default rather than trusting
    // whatever the request says.
    int pollSec = server.arg("poll_sec").toInt();
    bool validPoll = false;
    for (int allowed : {30, 60, 120, 300}) if (pollSec == allowed) validPoll = true;
    storage::setPollIntervalSec(validPoll ? pollSec : DD_POLL_INTERVAL_SEC_DEFAULT);
    // Confirms what the form actually submitted vs. what got saved — if
    // led_style ever comes back empty/unexpected here, that's a form
    // problem; if it's correct here but the LED still doesn't change,
    // that's downstream in updateMoodLed()/the LEDC hardware layer instead.
    Serial.printf("[portal] save-prefs: team=%s time_format=%s led_style=%s -> ledBreatheEnabled=%s\n",
                  team.c_str(), server.arg("time_format").c_str(), server.arg("led_style").c_str(),
                  ledBreathe ? "true" : "false");
    server.sendHeader("Location", "/?prefs=saved");
    server.send(303, "text/plain", "");
}

// See main.cpp's doc comment on these — same deferred handoff shape as
// g_ddKeysJustSaved above, for the "detect my teams" fetch below.
extern volatile bool g_oncallTeamsFetchRequested;
extern volatile bool g_oncallTeamsFetchDone;

static void handleOnCallTeam() {
    if (!g_oncallTeamsFetchDone) {
        g_oncallTeamsFetchRequested = true;
        String html = FPSTR(PAGE_HEAD);
        html += "<meta http-equiv=\"refresh\" content=\"1\">";
        html += "<div class=\"card\"><div class=\"sub\" style=\"margin-bottom:0\">Detecting your teams&hellip;</div></div>";
        html += FPSTR(PAGE_FOOT);
        server.send(200, "text/html", html);
        return;
    }
    g_oncallTeamsFetchDone = false;   // one-shot — the next visit re-detects fresh

    const std::vector<dd::Team>& teams = dd::lastMyTeams();
    String html = FPSTR(PAGE_HEAD);
    html += "<div class=\"card\">";
    html += "<div class=\"section\">On-Call Team</div>";
    if (teams.empty()) {
        html += "<div class=\"sub\" style=\"margin-bottom:0\">No teams found for this API key's user. "
                "Set up a team in Datadog first, then come back and detect again.</div>";
        html += "<button type=\"button\" class=\"secondary\" style=\"margin-top:14px\" "
                "onclick=\"location.href='/oncall-team'\">Detect again</button>";
    } else {
        // First runtime-populated <select> in this file — every other one
        // (poll_sec, time_format, led_style) iterates a fixed compile-time
        // list; this iterates dd::lastMyTeams(), fetched moments ago above.
        String currentId = storage::getOnCallTeamId();
        html += "<form method=\"POST\" action=\"/save-oncall-team\">";
        html += "<label>Team</label><select name=\"team_id\">";
        for (const dd::Team& t : teams) {
            html += "<option value=\"" + htmlEscape(t.id) + "\"" +
                    (t.id == currentId ? " selected" : "") + ">" + htmlEscape(t.name) + "</option>";
        }
        html += "</select>";
        html += "<div class=\"note\" style=\"margin-top:6px\">Teams belonging to whoever created this device's "
                "API/App key pair. On-Call shows this team's roster regardless of the general Team scope above.</div>";
        html += "<button type=\"submit\">Save</button></form>";
    }
    html += "</div>";
    html += FPSTR(PAGE_FOOT);
    server.send(200, "text/html", html);
}

static void handleSaveOnCallTeam() {
    String teamId = server.arg("team_id");
    teamId.trim();
    storage::setOnCallTeamId(teamId);
    server.sendHeader("Location", "/?prefs=saved");
    server.send(303, "text/plain", "");
}

static void handleForget() {
    storage::clearAll();
    String html = FPSTR(PAGE_HEAD);
    html += "<div class=\"title\">Cleared</div><div class=\"sub\">Restarting and re-opening captive portal&hellip;</div>";
    html += FPSTR(PAGE_FOOT);
    server.send(200, "text/html", html);
    delay(500);
    ESP.restart();
}

void portal::begin() {
    server.on("/",       HTTP_GET,  handleRoot);
    server.on("/save",   HTTP_POST, handleSave);
    server.on("/saved",  HTTP_GET,  handleSaved);
    server.on("/save-prefs", HTTP_POST, handleSavePrefs);
    server.on("/oncall-team", HTTP_GET, handleOnCallTeam);
    server.on("/save-oncall-team", HTTP_POST, handleSaveOnCallTeam);
    server.on("/forget", HTTP_POST, handleForget);
    server.on("/favicon.ico", HTTP_GET, [](){
        server.send_P(200, "image/png", (const char*)FAVICON_PNG, FAVICON_PNG_LEN);
    });
    server.onNotFound([](){ server.sendHeader("Location","/"); server.send(302,"text/plain",""); });
    server.begin();
    Serial.printf("[portal] http on http://%s/\n", WiFi.localIP().toString().c_str());
}

void portal::loop()        { server.handleClient(); }
String portal::currentIP() { return WiFi.localIP().toString(); }
