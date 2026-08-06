#include "portal.h"
#include "config.h"
#include "storage.h"
#include "datadog.h"
#include "wifi_setup.h"   // netcfg::apSsid() — shown in the device-metrics note

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
<title>BarkBoard &middot; setup</title>
<link rel="icon" type="image/png" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABIAAAAUCAYAAACAl21KAAACZ0lEQVR4nHWUS4iPURjGf2dmiEbuksskSVmwmo2FskCTkpJGURY2NqJkMwu5lJKNLORSrCgLIRaiZKNkipIsFLlNM+6XpnEbM/PTO/P+xzfDvPX1nfN95zzned/neQ9qAVCnqRMYI9Qj6nt1e87r/rdon9qp3lLnq1PUPep5dbd6zKG4rvapLbmvvgrS5t94rnapb/w3etS96ok8tG4EK7Vb7VX71Xvqo9z4K999OY7/7cm2VR1fzSoQbwLjctwEtAc+EAu7c12Mo5Z3geWllIvA79FA24D48RE4CjzJTWeB6cDlGnngQ4CqDWMpUq8eVHepHZnSSXWD+lAdyBQjfqgLRytXUv444Tkwfyz5GWLUmcyD9VZgIL6XUhyWUL2WJ0bhazFQEeKLuijXPlWbMpMhVjkJZqvVTxWQrpS8Gq/VB+rLUaUpNVfXlVLi9OXAbWAi8Am4CjQCa4Be4DgwA/gJdAHLgGbg3GD1E6ShlBI+egCsAD4DT4HXwCXgO7AJWAXMScXJA2cNAmWeUYeYz8zCvgPmAoeBXylIrSX6KkA7gaVh8xKMBisPp4El6aOmNF1/BaQ3lVoJtAFX8hlmNAm4kAtuJFiw+ZYnB/BXYCpwqpQSDr+bmQy2Sm3RGWBdfmxOV3cAk/P//SzsemDvsAmHMonC94TsUZO3FTXr03ABPhvYCLSUUp5V5I57ax4QvgqXt0Zqoc4hYH8WMZ5I7XEyixQb1c3AgpQ/mjyYhCCvgB0BFMU+oC4GtlS6elwp5av6AlibNriTG9+UUkKEEa4M5cLdYfn7efdEu0z873U6stEbam3yB+i4DQ1sJV6WAAAAAElFTkSuQmCC">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Press+Start+2P&family=JetBrains+Mono:wght@400;500;600;700&family=Inter:wght@400;500;600;700;800&display=swap" rel="stylesheet">
<script>
(function () {
  var stored = null;
  try { stored = localStorage.getItem('bb-theme'); } catch (e) {}
  var mode = stored || 'auto';
  var dark = mode === 'auto' ? window.matchMedia('(prefers-color-scheme: dark)').matches : mode === 'dark';
  document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light');
  document.documentElement.setAttribute('data-theme-mode', mode);
})();
</script>
<style>
  :root{
    --purple: oklch(0.62 0.23 302);
    --purple-soft: oklch(0.62 0.23 302 / 0.14);
    --purple-border: oklch(0.62 0.23 302 / 0.3);
    --ok: oklch(0.72 0.17 150);
    --ok-soft: oklch(0.72 0.17 150 / 0.14);
    --ok-border: oklch(0.72 0.17 150 / 0.35);
    --warn: oklch(0.78 0.15 85);
    --alert: oklch(0.63 0.21 25);
    --alert-soft: oklch(0.63 0.21 25 / 0.14);
    --alert-border: oklch(0.5 0.15 25 / 0.5);

    --bg: oklch(0.13 0.012 285);
    --bg-dot: oklch(0.28 0.02 285);
    --surface: oklch(0.17 0.015 285);
    --border: oklch(0.28 0.02 285);
    --chip-bg: oklch(0.19 0.016 285);
    --ink: oklch(0.85 0.006 285);
    --ink-strong: oklch(0.97 0.004 285);
    --ink-muted: oklch(0.68 0.02 285);
    --ink-dim: oklch(0.5 0.015 285);
  }
  :root[data-theme="light"]{
    --bg: oklch(0.95 0.006 285);
    --bg-dot: oklch(0.86 0.012 285);
    --surface: oklch(0.99 0.003 285);
    --border: oklch(0.87 0.01 285);
    --chip-bg: oklch(0.93 0.006 285);
    --ink: oklch(0.3 0.015 285);
    --ink-strong: oklch(0.15 0.015 285);
    --ink-muted: oklch(0.42 0.018 285);
    --ink-dim: oklch(0.58 0.015 285);
  }
  *{box-sizing:border-box}
  body{
    margin:0;min-height:100vh;display:flex;flex-direction:column;
    font-family:'Inter',-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
    color:var(--ink);background-color:var(--bg);
    background-image:radial-gradient(var(--bg-dot) 1px, transparent 1px), radial-gradient(ellipse 900px 500px at 50% -10%, oklch(0.3 0.09 302 / 0.28), transparent);
    background-size:22px 22px, 100% 100%;
  }
  a{color:var(--purple)}
  button{font-family:inherit}
  code{font-family:'JetBrains Mono',monospace}

  .bb-header{
    display:flex;align-items:center;gap:12px;flex-wrap:wrap;
    padding:18px 28px;border-bottom:1px solid var(--border);background:var(--surface);
  }
  .bb-wordmark{font-family:'Press Start 2P',monospace;font-size:16px;letter-spacing:1px;color:var(--ink-strong)}
  .bb-wordmark span{color:var(--purple)}
  .bb-chip{
    display:flex;align-items:center;gap:6px;font-size:11px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;
    color:var(--ink-muted);background:var(--chip-bg);border:1px solid var(--border);border-radius:6px;padding:5px 9px 5px 8px;
  }
  .bb-chip .dot{width:6px;height:6px;border-radius:1px;background:var(--purple);display:inline-block}

  .bb-main{flex:1;padding:32px 18px}
  .bb-main-inner{max-width:960px;margin:0 auto;display:grid;grid-template-columns:1fr 1fr;gap:20px;align-items:stretch}
  .bb-card{background:var(--surface);border:1px solid var(--border);border-radius:16px;padding:20px}
  .bb-card-wide{grid-column:1 / -1}
  .bb-card-danger{border-color:var(--alert-border)}
  @media (max-width: 700px){
    .bb-main-inner{grid-template-columns:1fr}
  }

  .bb-section{
    font-size:12px;letter-spacing:.1em;text-transform:uppercase;color:var(--ink-muted);
    margin:0 0 14px;display:flex;align-items:center;justify-content:space-between;
  }
  .bb-pill{
    font-size:11px;letter-spacing:.04em;text-transform:none;padding:3px 10px;border-radius:999px;
    background:var(--ok-soft);border:1px solid var(--ok-border);color:var(--ok);
  }
  .bb-sub{color:var(--ink-muted);font-size:13px;line-height:1.5}
  .bb-note{margin-top:10px;color:var(--ink-dim);font-size:12.5px;line-height:1.55}

  label{display:block;font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--ink-dim);margin:16px 0 6px}
  label:first-of-type{margin-top:0}
  input[type=text],input[type=password]{
    width:100%;padding:11px 14px;border-radius:9px;border:1px solid var(--border);background:var(--bg);
    color:var(--ink);font-size:14px;outline:none;font-family:'JetBrains Mono',monospace;
  }
  input:focus{border-color:var(--purple);box-shadow:0 0 0 3px var(--purple-soft)}
  select{
    width:100%;padding:10px 14px;border-radius:9px;border:1px solid var(--border);background:var(--bg);
    color:var(--ink);font-size:14px;font-family:inherit;
  }
  input[type=checkbox],input[type=radio]{accent-color:var(--purple)}
  .bb-radio-list{display:grid;gap:8px;margin-top:2px}
  .bb-radio{
    display:flex;align-items:center;gap:10px;font-size:14px;font-weight:500;text-transform:none;letter-spacing:0;
    color:var(--ink);background:var(--bg);border:1px solid var(--border);border-radius:9px;padding:10px 14px;cursor:pointer;
  }
  .bb-radio:has(input:checked){border-color:var(--purple);background:var(--purple-soft)}
  .bb-radio input{width:auto;margin:0}
  .bb-radio-handle{color:var(--ink-dim);font-family:'JetBrains Mono',monospace;font-size:12px;margin-left:2px}

  button{
    appearance:none;border:none;cursor:pointer;font-size:13px;font-weight:700;letter-spacing:.02em;
    color:oklch(0.99 0 0);background:var(--purple);padding:11px 16px;border-radius:9px;
    width:100%;margin-top:18px;transition:filter .15s ease;
  }
  button:hover{filter:brightness(1.12)}
  button.bb-btn-outline{background:transparent;color:var(--ink-muted);border:1px solid var(--border)}
  button.bb-btn-danger{background:transparent;color:var(--alert);border:1px solid var(--alert-border)}
  button.bb-linklike{
    appearance:none;background:none;border:0;color:var(--purple);font-size:13px;font-weight:700;
    padding:0;margin:0;width:auto;cursor:pointer;
  }

  .bb-banner{grid-column:1 / -1;margin:0;padding:10px 14px;border-radius:10px;background:var(--ok-soft);color:var(--ok);border:1px solid var(--ok-border);font-size:13px}
  .bb-banner.err{background:var(--alert-soft);color:var(--alert);border-color:var(--alert-border)}
  .bb-kvs{display:grid;grid-template-columns:auto 1fr;gap:8px 16px;font-size:13px;color:var(--ink)}
  .bb-kvs b{color:var(--ink-muted);font-weight:500}
  .bb-chk{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--ink-muted);margin:10px 0 0}
  .bb-chk input{width:auto;margin:0}

  .bb-footer{
    display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:14px;
    padding:16px 28px;border-top:1px solid var(--border);background:var(--surface);
  }
  .bb-theme-switch{display:flex;gap:3px;background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:3px}
  .bb-theme-btn{
    appearance:none;border:none;cursor:pointer;font-size:12px;font-weight:600;padding:6px 11px;border-radius:6px;
    color:var(--ink-muted);background:transparent;width:auto;margin:0;
  }
  .bb-theme-btn.is-active{background:var(--purple);color:#fff}
  .bb-copyright{font-size:12.5px;color:var(--ink-dim)}
  .bb-github{
    display:flex;align-items:center;gap:6px;font-size:12px;font-weight:600;color:var(--ink-muted);text-decoration:none;
    background:var(--chip-bg);border:1px solid var(--border);border-radius:999px;padding:6px 12px;
  }
  .bb-github .dot{width:6px;height:6px;border-radius:50%;background:var(--purple);display:inline-block}
</style></head><body>
<div class="bb-header">
  <span class="bb-wordmark">BARK<span>BOARD</span></span>
  <span class="bb-chip"><span class="dot"></span>Powered by Datadog</span>
</div>
<div class="bb-main"><div class="bb-main-inner">
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
</div></div>
<div class="bb-footer">
  <div class="bb-theme-switch">
    <button type="button" class="bb-theme-btn" onclick="bbSetTheme('auto')">Auto</button>
    <button type="button" class="bb-theme-btn" onclick="bbSetTheme('light')">Light</button>
    <button type="button" class="bb-theme-btn" onclick="bbSetTheme('dark')">Dark</button>
  </div>
  <div class="bb-copyright">Open source, made with &#128156; by Kyle Taylor and Datadog</div>
  <a class="bb-github" href="https://github.com/kyletaylored/barkboard" target="_blank" rel="noopener"><span class="dot"></span>GitHub</a>
</div>
<script>
function bbSetTheme(mode) {
  var dark = mode === 'auto' ? window.matchMedia('(prefers-color-scheme: dark)').matches : mode === 'dark';
  document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light');
  document.documentElement.setAttribute('data-theme-mode', mode);
  try { localStorage.setItem('bb-theme', mode); } catch (e) {}
  bbSyncThemeButtons();
}
function bbSyncThemeButtons() {
  var mode = document.documentElement.getAttribute('data-theme-mode') || 'auto';
  var modes = ['auto', 'light', 'dark'];
  document.querySelectorAll('.bb-theme-btn').forEach(function (btn, i) {
    btn.classList.toggle('is-active', modes[i] === mode);
  });
}
bbSyncThemeButtons();
</script>
</body></html>
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
        html += String("<div class=\"bb-banner ") + (err?"err":"") + "\">" + htmlEscape(banner) + "</div>";
    }

    // Status first — the thing you actually look at this page to check.
    html += "<div class=\"bb-card\"><div class=\"bb-section\">Device Info</div><div class=\"bb-kvs\">";
    html += "<b>SSID</b><span>" + htmlEscape(WiFi.SSID()) + "</span>";
    html += "<b>IP</b><span>" + WiFi.localIP().toString() + "</span>";
    html += "<b>RSSI</b><span>" + String(WiFi.RSSI()) + " dBm</span>";
    String site = storage::getSite();
    html += "<b>Site</b><span>" + (site.length() ? htmlEscape(site) : String("not detected yet")) + "</span>";
    html += "</div></div>";

    html += "<div class=\"bb-card\">";
    html += "<div class=\"bb-section\">Datadog Keys<span class=\"bb-pill\">" +
            String(keysSet ? "&check; set" : "not set") + "</span></div>";
    if (showKeyForm) {
        html += "<form method=\"POST\" action=\"/save\">";
        html += "<label>API Key</label><input type=\"password\" id=\"api_key\" name=\"api_key\" placeholder=\"32-char hex string\" autocomplete=\"off\" required>";
        html += "<label>Application Key</label><input type=\"password\" id=\"app_key\" name=\"app_key\" placeholder=\"40-char hex string\" autocomplete=\"off\" required>";
        html += "<label class=\"bb-chk\"><input type=\"checkbox\" onclick=\""
                "document.getElementById('api_key').type=this.checked?'text':'password';"
                "document.getElementById('app_key').type=this.checked?'text':'password'\"> Show keys</label>";
        html += "<div class=\"bb-note\">Both keys come from <i>Organization Settings &rarr; API Keys / Application Keys</i>. "
                "The Application Key needs read access to Monitors, Incidents, On-Call, SLOs, and Hosts.</div>";
        html += "<button type=\"submit\">Save</button>";
        html += "</form>";
    } else {
        html += "<div class=\"bb-sub\">Both keys are saved on this device.</div>";
        html += "<button type=\"button\" class=\"bb-btn-outline\" onclick=\"location.href='/?edit=keys'\">Change keys</button>";
    }
    html += "</div>";

    html += "<div class=\"bb-card bb-card-wide\">";
    html += "<div class=\"bb-section\">Preferences</div>";
    html += "<form method=\"POST\" action=\"/save-prefs\">";
    html += "<label>Team <small>(optional)</small></label>";
    html += "<input type=\"text\" name=\"team\" placeholder=\"my-team\" value=\"" + htmlEscape(storage::getTeamScope()) + "\" style=\"font-family:'JetBrains Mono',monospace\">";
    html += "<div class=\"bb-note\">Applied to Monitors, Incidents, and SLOs (each uses "
            "Datadog's own field naming under the hood, e.g. monitors' <code>team:</code> tag vs. incidents' "
            "<code>teams:</code> field, so you don't have to know which is which) — On-Call has its own separate "
            "team below, since it's about your account, not a filter. Leave this blank and the device "
            "auto-detects your team from your API key, same as On-Call; type one here to override it.</div>";
    html += "<label>Poll interval</label>";
    int pollSec = storage::getPollIntervalSec();
    html += "<select name=\"poll_sec\">";
    for (int secs : {30, 60, 120, 300}) {
        String labelText = secs < 60 ? (String(secs) + "s")
                          : (String(secs / 60) + (secs == 60 ? " minute" : " minutes"));
        html += String("<option value=\"") + secs + "\"" + (pollSec == secs ? " selected" : "") + ">" + labelText + "</option>";
    }
    html += "</select>";
    html += "<div class=\"bb-note\">How often the dashboard re-fetches monitors/incidents/on-call/SLOs. Shorter means fresher data but more API calls.</div>";
    html += "<label>Clock format</label>";
    bool is24h = storage::getTimeFormat24h();
    html += "<select name=\"time_format\">";
    html += String("<option value=\"24\"") + (is24h ? " selected" : "") + ">24-hour (14:30)</option>";
    html += String("<option value=\"12\"") + (!is24h ? " selected" : "") + ">12-hour (2:30 PM)</option>";
    html += "</select>";
    html += "<label>Status LED</label>";
    bool ledBreathe = storage::getLedBreatheEnabled();
    html += "<select name=\"led_style\">";
    html += String("<option value=\"breathe\"") + (ledBreathe ? " selected" : "") + ">Purple breathing when healthy</option>";
    html += String("<option value=\"solid\"") + (!ledBreathe ? " selected" : "") + ">Solid green when healthy</option>";
    html += "</select>";
    html += "<div class=\"bb-note\">Either way, Warn/Alert still show as solid yellow/red — this only changes the all-clear look.</div>";
    html += "<label>Device metrics</label>";
    bool metricsOn = storage::getMetricsEnabled();
    html += "<select name=\"metrics_enabled\">";
    html += String("<option value=\"off\"") + (!metricsOn ? " selected" : "") + ">Off</option>";
    html += String("<option value=\"on\"")  + (metricsOn  ? " selected" : "") + ">On — send to this Datadog org</option>";
    html += "</select>";
    html += "<div class=\"bb-note\">Sends device-health gauges (heap, task stack headroom, WiFi signal, uptime) back "
            "to your own Datadog org every " + String(METRICS_INTERVAL_SEC) + "s, tagged <code>device:" +
            htmlEscape(netcfg::apSsid()) + "</code>, and reports abnormal reboots (panic/watchdog/brownout) as a "
            "Datadog Event — because what's a Datadog dashboard without also monitoring itself? Custom metrics and "
            "Events are both billable Datadog usage, so both stay off unless you turn this on. Off by default.</div>";
    html += "<button type=\"submit\">Save preferences</button>";
    html += "</form></div>";

    html += "<div class=\"bb-card\">";
    html += "<div class=\"bb-section\">On-Call Team<button type=\"button\" class=\"bb-linklike\" "
            "onclick=\"location.href='/oncall-team'\">Refresh list</button></div>";
    String ocTeamId = storage::getOnCallTeamId();
    // dd::lastMyTeams() is kept warm by the device's own periodic On-Call
    // poll (see fetchOnCallAll()'s doc comment in datadog.h) — no extra
    // fetch needed here just to render the picker.
    const std::vector<dd::Team>& ocTeams = dd::lastMyTeams();
    if (ocTeams.empty()) {
        html += "<div class=\"bb-sub\">";
        html += ocTeamId.length()
                    ? ("Auto-detected from your API key. <b>Team id:</b> " + htmlEscape(ocTeamId))
                    : String("Not yet detected — the device will auto-detect it from your API key the next time "
                             "the On-Call screen refreshes, or you can detect it here now.");
        html += " <button type=\"button\" class=\"bb-linklike\" onclick=\"location.href='/oncall-team'\">"
                + String(ocTeamId.length() ? "Change" : "Detect now") + "</button></div>";
    } else {
        html += "<form method=\"POST\" action=\"/save-oncall-team\">";
        html += "<div class=\"bb-radio-list\">";
        for (const dd::Team& t : ocTeams) {
            bool checked = t.id == ocTeamId || (ocTeamId.length() == 0 && &t == &ocTeams.front());
            html += "<label class=\"bb-radio\"><input type=\"radio\" name=\"team_id\" value=\"" +
                    htmlEscape(t.id) + "\"" + (checked ? " checked" : "") + "> " + htmlEscape(t.name) +
                    " <span class=\"bb-radio-handle\">" + htmlEscape(t.handle) + "</span></label>";
        }
        html += "</div>";
        html += "<button type=\"submit\">Save</button></form>";
    }
    html += "</div>";

    html += "<div class=\"bb-card bb-card-danger\">";
    html += "<div class=\"bb-section\">Danger Zone</div>";
    html += "<div class=\"bb-sub\">Erases WiFi credentials and Datadog keys from this device — you'll need to go through setup again.</div>";
    html += "<form method=\"POST\" action=\"/forget\">";
    html += "<button type=\"submit\" class=\"bb-btn-danger\">Forget WiFi &amp; keys</button></form>";
    html += "</div>";

    html += "<p class=\"bb-note\">Keys are stored in NVS on the device and never logged.</p>";
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
    storage::setMetricsEnabled(server.arg("metrics_enabled") == "on");

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
    Serial.printf("[portal] save-prefs: team=%s time_format=%s led_style=%s -> ledBreatheEnabled=%s metrics_enabled=%s\n",
                  team.c_str(), server.arg("time_format").c_str(), server.arg("led_style").c_str(),
                  ledBreathe ? "true" : "false", server.arg("metrics_enabled").c_str());
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
        html += "<div class=\"bb-card bb-card-wide\"><div class=\"bb-sub\">Detecting your teams&hellip;</div></div>";
        html += FPSTR(PAGE_FOOT);
        server.send(200, "text/html", html);
        return;
    }
    g_oncallTeamsFetchDone = false;   // one-shot — the next visit re-detects fresh

    const std::vector<dd::Team>& teams = dd::lastMyTeams();
    String html = FPSTR(PAGE_HEAD);
    html += "<div class=\"bb-card bb-card-wide\">";
    html += "<div class=\"bb-section\">On-Call Team</div>";
    if (teams.empty()) {
        html += "<div class=\"bb-sub\">No teams found for this API key's user. "
                "Set up a team in Datadog first, then come back and detect again.</div>";
        html += "<button type=\"button\" class=\"bb-btn-outline\" "
                "onclick=\"location.href='/oncall-team'\">Detect again</button>";
    } else {
        // You can belong to more than one Datadog team — list every team this
        // API key's user belongs to (fetched moments ago above) as radio
        // options rather than forcing a single guess into a dropdown.
        String currentId = storage::getOnCallTeamId();
        html += "<form method=\"POST\" action=\"/save-oncall-team\">";
        html += "<label>Team</label><div class=\"bb-radio-list\">";
        for (const dd::Team& t : teams) {
            bool checked = t.id == currentId || (currentId.length() == 0 && &t == &teams.front());
            html += "<label class=\"bb-radio\"><input type=\"radio\" name=\"team_id\" value=\"" +
                    htmlEscape(t.id) + "\"" + (checked ? " checked" : "") + "> " + htmlEscape(t.name) + "</label>";
        }
        html += "</div>";
        html += "<div class=\"bb-note\">Teams belonging to whoever created this device's "
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
    html += "<div class=\"bb-card bb-card-wide\"><div class=\"bb-section\">Cleared</div><div class=\"bb-sub\">Restarting and re-opening captive portal&hellip;</div></div>";
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
