#pragma once

// Injected at build time by tools/get_version.py (a PlatformIO pre: extra
// script; see platformio.ini) via `git describe --tags --always --dirty` —
// "v1.2.0" right after a tag, "v1.2.0-3-gabc1234" for commits since, "-dirty"
// appended for uncommitted changes, or the bare short SHA if no tag exists
// yet at all. This fallback only matters for builds that don't run through
// that script (the sim/ host build, or an IDE indexer) — real device builds
// always get the real one.
#ifndef BARKBOARD_VERSION
#define BARKBOARD_VERSION "dev"
#endif

// Display: 320x240 landscape (rotation 1)
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// Touch (XPT2046) — separate SPI bus
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_SCLK 25
#define TOUCH_CS   33
#define TOUCH_IRQ  36

// Touch calibration (raw -> pixel mapping); refined later if needed.
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

// Onboard RGB LED (active-low)
#define LED_R 4
#define LED_G 16
#define LED_B 17

// Onboard speaker (SC8002B amp) — NOT in the pagerduty-cyd reference project
// (it never used the piezo). GPIO26 is corroborated by multiple independent
// community pinout writeups (espboards.dev, mischianti.org,
// randomnerdtutorials.com) for this exact board, not from an official
// datasheet — per CLAUDE.md, treat as unverified until a human confirms it
// chirps on real hardware.
#define SPEAKER_PIN 26

// Captive portal (WiFi join AP) — deliberately open (no password): this is a
// one-time setup network with no sensitive traffic, and skipping the
// password removes a step from the flow a phone has to complete just to
// reach the setup page.
#define AP_SSID_PREFIX "BarkBoard-"

// Datadog — fallback default only; the real, user-configurable value lives
// in NVS (NVS_KEY_POLL_INTERVAL_SEC / storage::getPollIntervalSec()), set
// via the web Settings page. 60s, not the previous hardcoded 30s.
#define DD_POLL_INTERVAL_SEC_DEFAULT 60

// HTTP web portal (key-entry setup page)
#define PORTAL_HTTP_PORT 80

// NVS keys
#define NVS_NS          "bbcyd"
#define NVS_KEY_API_KEY "dd_api_key"
#define NVS_KEY_APP_KEY "dd_app_key"
#define NVS_KEY_SITE    "dd_site"     // detected Datadog site host, e.g. "datadoghq.com"
// A single bare team value (e.g. "my-team", no "team:"/"teams:" prefix) —
// one Settings-page field for all screens. Each screen's fetcher builds its
// own query fragment from this, since monitors/SLOs (tag convention
// "team:x") and incidents (custom field literally named "teams", plural)
// don't share a filter syntax; splitting this into two raw-query fields
// fixed correctness but pushed the team/teams distinction onto the user
// instead of the code, so it's collapsed back to one value here.
#define NVS_KEY_SCOPE_TEAM      "dd_scope_team"
#define NVS_KEY_TIME_FORMAT_24H "time_24h"    // bool; default true (24h) if unset
#define NVS_KEY_LED_BREATHE     "led_breathe" // bool; default true — purple breathing LED when healthy
#define NVS_KEY_POLL_INTERVAL_SEC "poll_sec"  // int; default DD_POLL_INTERVAL_SEC_DEFAULT (60s)
// On-Call's own team selection — deliberately separate from
// NVS_KEY_SCOPE_TEAM above. That field answers "what should Monitors/
// Incidents/SLOs show"; this one answers "which team's on-call roster",
// auto-detected from the API-key-owning user's own team memberships
// (GET /api/v2/team?filter[me]=true) rather than typed in. "" means not
// yet resolved — set automatically when filter[me] returns exactly one
// team, or via the web Settings page's picker when it returns more than one.
#define NVS_KEY_ONCALL_TEAM_ID "dd_oncall_team"
