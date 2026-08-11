#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <math.h>
#include <vector>

#include "config.h"
#include "storage.h"
#include "display.h"
#include "ui.h"
#include "wifi_setup.h"
#include "portal.h"
#include "datadog.h"

static bool portalServerStarted = false;
static bool wasConnected = false;
static uint32_t lastClockTickMs = 0;

// dd::isConfigured() reads two NVS keys; ESP-IDF logs an error-level line
// every time a key isn't found yet (benign — our code handles the "" default
// fine, but calling this every ~5ms loop tick floods the serial monitor with
// "dd_api_key NOT_FOUND" before setup). Cache it and only refresh once a
// second, or immediately after a key-save so the transition isn't delayed.
// No cache here — DRAM on this device is razor-thin (see CLAUDE.md/earlier
// build history) and every static global added has tipped some build over
// the limit. dd::isConfigured() is just two cheap NVS string-length reads
// (storage::hasKeys()/hasSite()), so calling it directly every loop() tick
// is a fine trade for staying in budget; it's a read, not a write, so
// there's no flash-wear concern, just a small, acceptable CPU cost.
static bool isConfiguredThrottled() {
    return dd::isConfigured();
}

// Same reasoning as isConfiguredThrottled() above — no cache, just a direct
// NVS read every tick.
static int pollIntervalSecThrottled() {
    return storage::getPollIntervalSec();
}

// Mood-ring LED + piezo chirp (BARKBOARD_PLAN.md §5.1/§5.2) — the reference
// project only ever blinked the LED blue during a poll; this repurposes it
// as an ambient health indicator, same GPIOs, no new hardware. Edge-trigger
// state (prevAlert/prevIncidents) now lives in netTask() below, the only
// place that still does the ambient poll.

enum MoodLevel { MOOD_OK, MOOD_WARN, MOOD_CRITICAL };

static MoodLevel currentMood() {
    // dd::moodInputsSnapshot(), not raw lastMonitorCounts()/lastIncidents() —
    // once fetchMonitorCounts()/fetchIncidents() run on the core-0 net task
    // (see netTask() below), this read happening unconditionally on every
    // core-1 loop() tick would otherwise be a genuine unsynchronized
    // cross-core race; that accessor carries its own tiny critical section.
    int alert, warn; bool hasCritical;
    dd::moodInputsSnapshot(alert, warn, hasCritical);
    if (hasCritical) return MOOD_CRITICAL;
    if (alert > 0 || warn > 0) return MOOD_WARN;
    return MOOD_OK;
}

// LEDC (PWM) has now failed to reliably drive this LED across two different
// channel assignments — 0-2 (died after the first tone() chirp stole channel
// 0) and 13-15/1-3 (still wrong/dark, no tone() involved, so that wasn't a
// channel-stealing repeat — root cause on that one is still unconfirmed).
// Rather than keep guessing channel numbers, only use LEDC for the one
// thing that actually needs an intermediate duty value — the breathing
// purple fade — and drive every solid color (CRITICAL/WARN/plain OK) with
// plain digitalWrite(), the one control path already confirmed working on
// this exact board before PWM was ever introduced. ledcDetachPin() before
// digitalWrite and ledcAttachPin() before ledcWrite because mixing the two
// on an attached pin is documented as unreliable; there's no per-mode
// static to track which mode a pin is currently in (DRAM is razor-thin —
// see CLAUDE.md build history), so every call just (re-)asserts the mode
// it needs, which is redundant but harmless.
// Red (GPIO4) is confirmed dead on real hardware — reproducible on 3
// separate boards even with a bare digitalWrite/PWM sanity sketch
// (tools/ledtest), independent of anything in this codebase. Until a
// hardware fix is found, every mood color below is built only from
// green+blue, the two channels confirmed working.
#define LEDC_CH_G 1
#define LEDC_CH_B 3
#define LED_BREATHE_PERIOD_MS 3000

static void setSolidRGB(uint8_t r, uint8_t g, uint8_t b) {
    (void)r;   // unused — see the LED note above; kept in the signature so callers don't need rewriting if red is ever fixed
    ledcDetachPin(LED_G);
    ledcDetachPin(LED_B);
    pinMode(LED_G, OUTPUT);
    pinMode(LED_B, OUTPUT);
    digitalWrite(LED_G, g ? LOW : HIGH);   // active-low
    digitalWrite(LED_B, b ? LOW : HIGH);
}

// g/b are 0..255 brightness (255 = brightest).
static void setBreathingGB(uint8_t g, uint8_t b) {
    // ledcSetup() configures the channel's timer (freq/resolution) — has to
    // run before ledcAttachPin()/ledcWrite() actually do anything, and there's
    // no static here to call it only once, so it's just re-asserted every
    // time (cheap register writes, idempotent).
    ledcSetup(LEDC_CH_G, 5000, 8); ledcAttachPin(LED_G, LEDC_CH_G);
    ledcSetup(LEDC_CH_B, 5000, 8); ledcAttachPin(LED_B, LEDC_CH_B);
    ledcWrite(LEDC_CH_G, 255 - g);
    ledcWrite(LEDC_CH_B, 255 - b);
}

static void updateMoodLed() {
    MoodLevel mood = currentMood();
    bool ledBreathe = storage::getLedBreatheEnabled();

    // Solid, not pulsing, for WARN/CRITICAL — this used to blink, which
    // reads as "constantly flashing" the moment an org has *any* standing
    // warn/alert (i.e. always, for a busy org) rather than genuinely
    // ambient. Only the healthy state breathes — Warn/Alert stay solid so
    // they read as "something's wrong" at a glance, not blend into the effect.
    if (mood == MOOD_CRITICAL) {
        setSolidRGB(0, 255, 255);     // solid cyan (green+blue, both full — no red available, see LED note above)
    } else if (mood == MOOD_WARN) {
        setSolidRGB(0, 0, 255);       // solid blue
    } else if (ledBreathe) {
        float phase = (millis() % LED_BREATHE_PERIOD_MS) / (float)LED_BREATHE_PERIOD_MS;
        float bri = (sinf(phase * 2.0f * (float)M_PI - (float)M_PI / 2.0f) + 1.0f) / 2.0f;  // 0..1
        uint8_t b = (uint8_t)(bri * 255);
        setBreathingGB(b, b);         // breathing cyan
    } else {
        setSolidRGB(0, 255, 0);       // solid green (breathing disabled in Settings)
    }
}

// Short two-tone chirp — edge-triggered only (new alert/incident, not every
// poll) so it doesn't nag on every 30s refresh. No Settings screen yet to
// expose a mute toggle (BARKBOARD_PLAN.md §5.2 calls for one) — future work.
static void chirpNewAlert() {
    if (ui::chirpMuted()) return;
    tone(SPEAKER_PIN, 880, 80);
    delay(90);
    tone(SPEAKER_PIN, 660, 120);
    delay(120);
    noTone(SPEAKER_PIN);
}

// WiFi itself carries no timezone info, but the network's public IP does
// imply a rough location. Originally used worldtimeapi.org — confirmed live
// (from a dev machine, not the device) that it's currently unreachable over
// both HTTP and HTTPS, which is why this silently fell back to UTC with no
// visible error. Switched to ip-api.com's free JSON endpoint instead,
// explicitly requesting the "offset" field — confirmed live it returns a
// ready-to-use signed integer in seconds that already accounts for DST (no
// separate raw+dst summing, no IANA-name-to-POSIX-TZ mapping needed). Plain
// HTTP: public, non-sensitive lookup, skips TLS overhead for a call this
// small and this early in boot. Falls back to UTC on any failure.
static void detectAndConfigureTime() {
    HTTPClient http;
    http.setTimeout(6000);
    http.setConnectTimeout(4000);
    if (!http.begin("http://ip-api.com/json/?fields=status,timezone,offset")) {
        Serial.println("[ntp] timezone lookup failed to start, defaulting to UTC");
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        return;
    }
    int code = http.GET();
    if (code == 200) {
        JsonDocument doc;
        DeserializationError je = deserializeJson(doc, http.getStream());
        http.end();
        if (!je && String(doc["status"] | "") == "success") {
            long offsetSec = doc["offset"] | 0;
            const char* tz = doc["timezone"] | "UTC";
            configTime(offsetSec, 0, "pool.ntp.org", "time.google.com");
            Serial.printf("[ntp] timezone detected: %s (UTC%+ld:%02ld)\n",
                          tz, offsetSec / 3600, labs(offsetSec / 60) % 60);
            return;
        }
        Serial.printf("[ntp] timezone lookup bad response (status=%s je=%s), defaulting to UTC\n",
                      (const char*)(doc["status"] | "?"), je.c_str());
    } else {
        Serial.printf("[ntp] timezone lookup HTTP %d, defaulting to UTC\n", code);
        http.end();
    }
    configTime(0, 0, "pool.ntp.org", "time.google.com");
}

// Shared with portal.cpp — key validation handoff so the HTTP request
// returns instantly. Wired to datadog::validateKeysAndDetectSite() (the
// nine-site validate_keys probe, BARKBOARD_PLAN.md §2) once datadog.cpp
// exists in a later build phase; for now the main loop just trusts that
// both keys were entered.
volatile bool g_ddKeysJustSaved = false;
volatile bool g_ddKeysValidated = false;
String        g_ddValidationError;

// Same handoff shape as above, for portal.cpp's /oncall-team picker page —
// GET /api/v2/team?filter[me]=true is a single quick call, not one of the
// core-split's "worst offenders", so this stays on core 1 like every other
// portal-triggered action rather than routing through netTask().
volatile bool g_oncallTeamsFetchRequested = false;
volatile bool g_oncallTeamsFetchDone = false;

// Callbacks fire on the WiFi task (core 0). We must NOT touch LVGL there;
// instead we stash state and the main loop (core 1) applies it.
static volatile bool   g_uiPortalDirty = false;
static volatile bool   g_uiStatusDirty = false;
static String          g_uiPortalSsid, g_uiPortalPwd;
static String          g_uiStatusMsg;
static portMUX_TYPE    g_uiMux = portMUX_INITIALIZER_UNLOCKED;

static void onPortalEnter(const String& ssid, const String& pwd) {
    Serial.printf("[wifi] AP up: %s (open, no password)\n", ssid.c_str());
    portENTER_CRITICAL(&g_uiMux);
    g_uiPortalSsid = ssid; g_uiPortalPwd = pwd;
    g_uiPortalDirty = true;
    portEXIT_CRITICAL(&g_uiMux);
}
static void onWiFiStatus(const String& msg) {
    Serial.printf("[wifi] %s\n", msg.c_str());
    portENTER_CRITICAL(&g_uiMux);
    g_uiStatusMsg = msg;
    g_uiStatusDirty = true;
    portEXIT_CRITICAL(&g_uiMux);
}

// ---- Core split: netTask() (core 0) <-> loop() (core 1) ----
// Everything that blocks on a Datadog HTTPS call used to run inline in
// loop() on core 1 (the same core LVGL renders on), which is exactly what
// made the UI feel frozen during any fetch — confirmed live comparing
// against Marauder's firmware on the same hardware. Only the worst
// offenders move here: the always-on ambient poll, Monitor Detail's 2-3
// chained fetches, and On-Call's per-team fan-out. Every other action
// (mute, incident-advance, declare case/incident, bits-trigger, SLO detail,
// case-projects fetch) is a single quick HTTP call with its own busy-
// spinner already and stays on core 1 as-is — moving everything would have
// needed cross-core caches this device's DRAM budget doesn't have (see
// CLAUDE.md build history; verified precisely via compiler size probes
// while building this).
enum class NetJobType { None, PollAmbient, MonitorDetail, FetchOnCall, FetchMonitors, FetchIncidents, FetchSlos, FetchBitsInvestigations };

struct NetJobStatus {
    NetJobType type = NetJobType::None;
    bool running = false;   // core0->core1: show the busy overlay (PollAmbient never sets this — silent background poll, matching pre-split behavior)
    bool done = false;      // core0->core1: fetch finished; core1 clears after dispatching
    bool ok = false;
    String err;
};
static portMUX_TYPE  g_netMux = portMUX_INITIALIZER_UNLOCKED;
static NetJobStatus  g_netJob;
// chirpNewAlert() drives the piezo via LEDC channel 0 (tone()); updateMoodLed()
// drives the LED via LEDC channels 1/3 on core 1 — different channels, but
// the Arduino-ESP32 LEDC driver's internal channel bookkeeping isn't
// documented as safe to touch concurrently from two cores. Cheaper and
// safer to just signal the request and let core 1 make the actual call,
// same core updateMoodLed() already runs on, than to trust that driver's
// internals across cores.
static volatile bool g_chirpPending = false;

// Captured once in setup() (core 1) via xTaskGetCurrentTaskHandle() — a task
// can't look up another task's handle by name, so this is passed into
// dd::submitDeviceMetrics() to report the Arduino loop task's stack
// headroom alongside dd-net's own (see that function's doc comment in
// datadog.h). A plain FreeRTOS handle, safe to read cross-core.
static TaskHandle_t g_loopTaskHandle = nullptr;

// loop()'s (core 1) coarse "busy vs. asleep in delay(5)" running average —
// see dd::submitDeviceMetrics()'s doc comment in datadog.h for exactly what
// this is and isn't. Written every loop() iteration, read + reset once per
// metrics submission by netTask() (core 0) via getLoopBusyPctAndReset() —
// a real cross-core access, hence the mutex (netTask()'s own busy% doesn't
// need one; it's tracked as plain locals inside netTask() itself, read on
// the same task/core that writes it).
static portMUX_TYPE g_busyMux = portMUX_INITIALIZER_UNLOCKED;
static float        g_loopBusyAccum = 0;
static uint32_t     g_loopBusyCount = 0;

static bool getLoopBusyPctAndReset(float& outPct) {
    portENTER_CRITICAL(&g_busyMux);
    bool has = g_loopBusyCount > 0;
    if (has) outPct = g_loopBusyAccum / g_loopBusyCount;
    g_loopBusyAccum = 0;
    g_loopBusyCount = 0;
    portEXIT_CRITICAL(&g_busyMux);
    return has;
}

// g_netJob has room for exactly one outstanding signal — if two of
// netTask()'s three jobs (ambient poll / Monitor Detail / On-Call) finish
// within the same core-0 pass before core 1's loop() drains between them,
// the second netJobRunning()/netJobDone() call would silently overwrite the
// first's type/done before it was ever seen, dropping or misattributing a
// UI update. Waiting here for any earlier undrained done signal to clear
// first closes that gap — normally a no-op since loop() drains within a
// few ms, so this almost never actually waits.
static void waitForJobSlotFree() {
    for (;;) {
        portENTER_CRITICAL(&g_netMux);
        bool free = !g_netJob.done;
        portEXIT_CRITICAL(&g_netMux);
        if (free) return;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
static void netJobRunning(NetJobType type) {
    waitForJobSlotFree();
    portENTER_CRITICAL(&g_netMux);
    g_netJob.type = type;
    g_netJob.running = true;
    portEXIT_CRITICAL(&g_netMux);
}
static void netJobDone(NetJobType type, bool ok, const String& err = String()) {
    waitForJobSlotFree();
    portENTER_CRITICAL(&g_netMux);
    g_netJob.type = type;
    g_netJob.done = true;
    g_netJob.ok = ok;
    g_netJob.err = err;
    portEXIT_CRITICAL(&g_netMux);
}

// Runs entirely on core 0 — never touch lv_*/ui::showX()/ui::applyX()/
// ui::notifyX() from here, only netJobRunning()/netJobDone() and the
// ui::xxxPending() getters (made cross-core-safe for exactly this).
static void netTask(void*) {
    uint32_t lastPollMs = 0;
    bool firstPollDone = false;
    int prevAlert = -1;
    size_t prevIncidents = (size_t)-1;
    bool myTeamsAutoFetchDone = false;
    uint32_t lastMetricsMs = 0;
    bool bootReportDone = false;

    // netTask()'s own busy-vs-asleep tracking — see g_loopBusyAccum's doc
    // comment above for what this is. No mutex needed here: written and
    // read entirely within this one task/core (unlike loop()'s, which
    // netTask() reads cross-core), so these are plain locals.
    uint32_t lastNetIterStart = 0;
    uint32_t lastNetWorkMs = 0;
    float    netBusyAccum = 0;
    uint32_t netBusyCount = 0;

    for (;;) {
        uint32_t netIterStart = millis();
        if (lastNetIterStart) {
            uint32_t period = netIterStart - lastNetIterStart;
            if (period > 0) {
                float busyPct = (float)lastNetWorkMs / period * 100.0f;
                if (busyPct > 100) busyPct = 100;
                netBusyAccum += busyPct;
                netBusyCount++;
            }
        }
        lastNetIterStart = netIterStart;

        if (netcfg::isConnected() && isConfiguredThrottled()) {
            // One-shot per boot: reports the previous boot's reset reason
            // as a Datadog Event on every boot, not just abnormal ones —
            // see dd::reportBootEvent()'s doc comment for why (alert_type
            // still separates a real crash from a routine reboot). Gated on
            // its own opt-in toggle, separate from the metrics gauges below
            // — custom metrics and Events are both billable Datadog usage,
            // but distinct products a user may want on/off independently.
            if (!bootReportDone && storage::getEventsEnabled()) {
                bootReportDone = true;
                String berr;
                dd::reportBootEvent(berr);
            }

            // Auto-detect the team scope fallback (dd::bareTeamScope()) once
            // per boot via filter[me] — always attempted now that there's no
            // manual Team field to opt out of it with. Silent, no
            // netJobRunning()/busy overlay, same as the ambient poll below.
            if (!myTeamsAutoFetchDone) {
                myTeamsAutoFetchDone = true;
                std::vector<dd::Team> myTeams;
                String terr;
                dd::fetchMyTeams(myTeams, terr);
            }

            // Ambient poll — same cadence check main.cpp's loop() used to do
            // inline. No netJobRunning() here: silent background refresh,
            // matching pre-split behavior of never showing a spinner for it.
            if (!firstPollDone || (millis() - lastPollMs) > (uint32_t)pollIntervalSecThrottled() * 1000UL) {
                lastPollMs = millis();
                firstPollDone = true;
                dd::MonitorCounts counts;
                dd::fetchMonitorCounts(counts);
                std::vector<dd::Incident> incidents;
                String ierr;
                dd::fetchIncidents(incidents, ierr);
                // Signal only — see g_chirpPending's doc comment above for
                // why the actual tone()/ledcWrite() calls stay on core 1.
                if (prevAlert >= 0 && counts.alert > prevAlert) g_chirpPending = true;
                if (prevIncidents != (size_t)-1 && incidents.size() > prevIncidents) g_chirpPending = true;
                prevAlert = counts.alert;
                prevIncidents = incidents.size();
                netJobDone(NetJobType::PollAmbient, true);
            }

            // Opt-in device self-monitoring (Settings page toggle,
            // storage::getMetricsEnabled(), default off) — its own interval,
            // independent of the dashboard's own data-poll cadence above,
            // since these gauges don't need to be nearly as fresh. Silent,
            // no netJobRunning()/busy overlay, same as the ambient poll.
            if (storage::getMetricsEnabled() &&
                (!lastMetricsMs || (millis() - lastMetricsMs) > (uint32_t)METRICS_INTERVAL_SEC * 1000UL)) {
                lastMetricsMs = millis();
                float loopBusyPct = -1, thisNetBusyPct = -1;
                getLoopBusyPctAndReset(loopBusyPct);
                if (netBusyCount > 0) thisNetBusyPct = netBusyAccum / netBusyCount;
                netBusyAccum = 0;
                netBusyCount = 0;
                String merr;
                dd::submitDeviceMetrics(g_loopTaskHandle, loopBusyPct, thisNetBusyPct, merr);

                // Cheap to call every interval — it's a no-op string compare
                // once storage::getMetricMetadataVersion() already matches
                // this build, so a brand-new install only pays the real
                // 10-PUT-request cost once, not every 60s forever.
                String metaErr;
                dd::pushMetricMetadataIfNeeded(metaErr);
            }

            long monitorId;
            if (ui::monitorDetailRequestPending(monitorId)) {
                // No netJobRunning() — ui::showMonitorDetailLoading() already
                // put up a dedicated "Loading chart..." screen synchronously
                // on tap, before this task even sees the request.
                bool ok = dd::fetchMonitorDetailAndChart(monitorId);
                netJobDone(NetJobType::MonitorDetail, ok);
            }

            if (ui::oncallFetchPending()) {
                netJobRunning(NetJobType::FetchOnCall);
                dd::fetchOnCallAll();
                netJobDone(NetJobType::FetchOnCall, true);
            }

            // Monitors/Incidents/SLOs list fetches — moved here from loop()
            // for the same reason as the three above: every dashboard tab
            // should behave consistently (tap -> busy spinner -> view),
            // not just the ones that happened to chain multiple calls.
            // Each already has its own dd::lastXxx() cache (lastMonitors()/
            // lastIncidents()/lastSlos()), so no new cross-core cache is
            // needed here — same "read the cache after done==true" pattern.
            String filter;
            if (ui::monitorsFetchPending(filter)) {
                netJobRunning(NetJobType::FetchMonitors);
                std::vector<dd::Monitor> monitors;
                String merr;
                dd::fetchMonitors(filter, monitors, merr);
                netJobDone(NetJobType::FetchMonitors, true);
            }

            if (ui::incidentsFetchPending()) {
                netJobRunning(NetJobType::FetchIncidents);
                std::vector<dd::Incident> incidentsList;
                String ierr2;
                dd::fetchIncidents(incidentsList, ierr2);
                netJobDone(NetJobType::FetchIncidents, true);
            }

            if (ui::sloFetchPending()) {
                netJobRunning(NetJobType::FetchSlos);
                std::vector<dd::SloSummary> slos;
                String serr;
                dd::fetchSlos(slos, serr);
                netJobDone(NetJobType::FetchSlos, true);
            }

            if (ui::bitsInvestigationsFetchPending()) {
                netJobRunning(NetJobType::FetchBitsInvestigations);
                std::vector<dd::BitsInvestigation> investigations;
                String bierr;
                dd::fetchBitsInvestigations(investigations, bierr);
                netJobDone(NetJobType::FetchBitsInvestigations, true);
            }
        }
        lastNetWorkMs = millis() - netIterStart;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// Shared by every factory-reset trigger — the boot-time touch gesture
// below, the Settings screen's two-tap confirm, and the BOOT-button hold
// gesture (see checkBootButtonReset()) — wipes WiFi + Datadog credentials,
// drops the saved AP from NVS, and reboots back into fresh AP setup mode.
static void performFactoryReset(const char* reason) {
    Serial.printf("[reset] factory reset triggered (%s) — wiping creds\n", reason);
    storage::clearAll();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);   // erase saved AP from NVS
    delay(200);
    ESP.restart();
}

void setup() {
    // Must happen first thing in setup(), which runs on Arduino's own
    // "loopTask" (core 1) — this is the one place that task's own handle is
    // ever available to capture. Stashed for dd::submitDeviceMetrics() to
    // read its stack headroom from netTask() (core 0); see g_loopTaskHandle's
    // doc comment above.
    g_loopTaskHandle = xTaskGetCurrentTaskHandle();

    Serial.begin(115200);
    delay(150);
    Serial.println("\n[boot] BarkBoard");

    // Solid colors go through plain digitalWrite (setSolidRGB) — LEDC/PWM is
    // reserved for the one case that actually needs an intermediate duty
    // value, the breathing cyan fade (setBreathingGB), which attaches
    // these channels itself on demand. Just start dark via the digital path;
    // no ledcSetup()/ledcAttachPin() needed up front.
    setSolidRGB(0, 0, 0);
    pinMode(SPEAKER_PIN, OUTPUT);
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

    storage::begin();

    // One-time: clear stale WiFi creds from earlier dev sessions.
    {
        Preferences p; p.begin("bbcyd_meta", false);
        if (!p.getBool("wifi_wiped_v1", false)) {
            Serial.println("[boot] wiping stale WiFi creds (one-time)");
            WiFi.mode(WIFI_STA);
            WiFi.disconnect(true, true);
            p.putBool("wifi_wiped_v1", true);
        }
        p.end();
    }

    display::begin();

    // Factory-reset gesture: hold the touchscreen during boot for ~2s to wipe
    // WiFi creds and Datadog keys, then reboot back into AP setup mode.
    if (display::factoryResetPrompt(2000)) {
        performFactoryReset("touch gesture at boot");
    }

    ui::begin();
    display::tick();

    // Diagnostic only (see logLvglMemTrend() above) — the true baseline,
    // right after all six dashboard screens are built but before any user
    // navigation, distinguishes "the resident screens alone already eat
    // most of LV_MEM_SIZE" from "it only gets bad as you accumulate list
    // rows/animations across screens" — two very different fixes.
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        Serial.printf("[lvgl] mem BASELINE (post ui::begin, pre-navigation) free=%u biggest=%u used_pct=%u%% frag_pct=%u%%\n",
                      mon.free_size, mon.free_biggest_size, mon.used_pct, mon.frag_pct);
    }

    netcfg::begin(onPortalEnter, onWiFiStatus);

    // Stack comes from FreeRTOS's task-stack heap allocation (pvPortMalloc
    // at creation time) — a different pool from the .bss/.data static
    // segment (dram0_0_seg) that's overflowed repeatedly in this project's
    // history; sized generously above the transient WiFi-join task's 8192
    // (_reference/pagerduty-cyd/src/wifi_setup.cpp) since this one also does
    // ArduinoJson parsing and WiFiClientSecure TLS.
    xTaskCreatePinnedToCore(netTask, "dd-net", 10240, nullptr, 1, nullptr, 0);
}

// Diagnostic only — chasing a reported freeze on the SLO/On-Call screens
// (UI fully unresponsive, netTask() on core 0 still running fine). Safe to
// call lv_mem_monitor() here since it only ever runs on core 1, the one
// core that owns LVGL's pool. Prints LV_MEM_SIZE's free bytes/fragmentation
// every ~3s so the Serial log around the next freeze shows whether the pool
// was trending toward exhaustion beforehand — see lv_conf.h's LV_USE_LOG
// comment for why that's the leading theory.
// Non-blocking counterpart to display::factoryResetPrompt()'s boot-time
// touch-hold gesture, checked every loop() iteration instead of once at
// boot — recovers a device whose touchscreen has gone unresponsive or
// miscalibrated sometime *after* boot, not just at the boot instant. Safe
// to read GPIO0 here regardless of its boot-strapping role; see
// config.h's BOOT_BTN_PIN comment.
static uint32_t s_bootBtnHoldStartMs = 0;
static bool     s_bootBtnBusyShown = false;

static void checkBootButtonReset() {
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (s_bootBtnHoldStartMs == 0) {
            s_bootBtnHoldStartMs = millis();
        } else if (millis() - s_bootBtnHoldStartMs >= BOOT_BTN_RESET_HOLD_MS) {
            performFactoryReset("BOOT button hold");
        } else if (!s_bootBtnBusyShown) {
            s_bootBtnBusyShown = true;
            ui::showBusy("Hold BOOT to factory reset...");
        }
    } else {
        if (s_bootBtnBusyShown) ui::hideBusy();
        s_bootBtnHoldStartMs = 0;
        s_bootBtnBusyShown = false;
    }
}

static void logLvglMemTrend() {
    static uint32_t lastMs = 0;
    if (millis() - lastMs < 3000) return;
    lastMs = millis();
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("[lvgl] mem free=%u biggest=%u used_pct=%u%% frag_pct=%u%%\n",
                  mon.free_size, mon.free_biggest_size, mon.used_pct, mon.frag_pct);
}

void loop() {
    // Busy-vs-asleep tracking (see g_loopBusyAccum's doc comment) — must be
    // the very first thing measured, before anything else this iteration
    // does. lastLoopWorkMs/lastLoopIterStart are function-scoped statics
    // (not block-scoped) since they're written here at the top but also
    // need writing again at the bottom, right before delay(5).
    static uint32_t lastLoopIterStart = 0;
    static uint32_t lastLoopWorkMs = 0;
    uint32_t loopIterStart = millis();
    if (lastLoopIterStart) {
        uint32_t period = loopIterStart - lastLoopIterStart;
        if (period > 0) {
            float busyPct = (float)lastLoopWorkMs / period * 100.0f;
            if (busyPct > 100) busyPct = 100;
            portENTER_CRITICAL(&g_busyMux);
            g_loopBusyAccum += busyPct;
            g_loopBusyCount++;
            portEXIT_CRITICAL(&g_busyMux);
        }
    }
    lastLoopIterStart = loopIterStart;

    logLvglMemTrend();
    checkBootButtonReset();
    netcfg::process();

    // Drain UI updates queued by the WiFi task (core 0).
    if (g_uiPortalDirty || g_uiStatusDirty) {
        String ssid, pwd, msg;
        bool portalDirty = false, statusDirty = false;
        portENTER_CRITICAL(&g_uiMux);
        if (g_uiPortalDirty) { ssid = g_uiPortalSsid; pwd = g_uiPortalPwd; portalDirty = true; g_uiPortalDirty = false; }
        if (g_uiStatusDirty) { msg = g_uiStatusMsg; statusDirty = true; g_uiStatusDirty = false; }
        portEXIT_CRITICAL(&g_uiMux);
        if (portalDirty) {
            ui::showSetupHint(ssid, pwd,
                "Join this network from your phone. The captive portal will open automatically.");
        } else if (statusDirty && !netcfg::isConnected() && !netcfg::isPortalActive()) {
            ui::showConnecting(msg);
        }
    }

    display::tick();

    if (netcfg::isConnected()) {
        if (!wasConnected) {
            wasConnected = true;
            Serial.printf("[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
            if (!portalServerStarted) {
                portal::begin();
                portalServerStarted = true;
                String mdnsHostname = ui::deviceHostname();
                if (MDNS.begin(mdnsHostname.c_str())) {
                    MDNS.addService("http", "tcp", 80);
                    Serial.printf("[mdns] http://%s.local/\n", mdnsHostname.c_str());
                } else {
                    Serial.println("[mdns] failed to start");
                }
                detectAndConfigureTime();
                Serial.println("[ntp] sync started");
            }
        }

        // State-driven screen selection — corrects any race where the
        // *initial* transition into the dashboard was missed or overwritten.
        // Must only fire while still on a modal/setup screen: this used to
        // check just "now != Overview", which meant tapping anything that
        // navigated away from Overview (Monitors, Settings, any detail
        // screen) got forcibly snapped back to Overview on the very next
        // loop tick — calling lv_scr_load_anim again while the user's own
        // tap-triggered screen animation was potentially still in flight,
        // which is exactly the kind of LVGL re-entrancy that corrupts
        // internal state and crashes.
        ui::Screen want = isConfiguredThrottled() ? ui::Screen::Overview : ui::Screen::Waiting;
        ui::Screen now = ui::currentScreen();
        bool onModalScreen = (now == ui::Screen::None || now == ui::Screen::Connecting ||
                              now == ui::Screen::Setup || now == ui::Screen::Waiting);
        if (onModalScreen && want == ui::Screen::Waiting && now != ui::Screen::Waiting) {
            ui::showWaitingForKeys(String("http://") + ui::deviceHostname() + ".local\n" + WiFi.localIP().toString());
        } else if (onModalScreen && want == ui::Screen::Overview && now != ui::Screen::Overview) {
            ui::showOverview();
            ui::setStatusOnline(true);
        }
        portal::loop();

        // Clock tick — once per 30s update HH:MM. Format (12h/24h) is a
        // Settings-page preference; still UTC (no timezone picker yet —
        // Timezone (auto-detected via IP geolocation, see detectAndConfigureTime()).
        if (millis() - lastClockTickMs > 30000 || lastClockTickMs == 0) {
            lastClockTickMs = millis();
            time_t now = time(nullptr);
            if (now > 1700000000) {
                // localtime_r, not gmtime_r — gmtime_r always renders UTC
                // regardless of configTime()'s offset; only localtime_r
                // actually applies it.
                struct tm tmu; localtime_r(&now, &tmu);
                char buf[12];
                strftime(buf, sizeof(buf), storage::getTimeFormat24h() ? "%H:%M" : "%I:%M %p", &tmu);
                ui::setClockText(buf);
            }
        }

        // Drain netTask()'s (core 0) job status — ambient poll, Monitor
        // Detail, and On-Call all land here now instead of blocking this
        // loop directly (see the NetJobStatus/netTask() comment above
        // onWiFiStatus()). Same copy-out-then-release-then-apply idiom as
        // the g_uiPortalDirty/g_uiStatusDirty drain above.
        if (g_netJob.running || g_netJob.done) {
            NetJobStatus job;
            portENTER_CRITICAL(&g_netMux);
            job = g_netJob;
            g_netJob.running = false;
            g_netJob.done = false;
            portEXIT_CRITICAL(&g_netMux);

            if (job.running) {
                // PollAmbient never sets running=true (silent background
                // refresh) and MonitorDetail already has its own dedicated
                // "Loading chart..." screen from the tap handler — every
                // other type here does need the generic overlay.
                switch (job.type) {
                    case NetJobType::FetchOnCall:    ui::showBusy("Loading on-call...");   break;
                    case NetJobType::FetchMonitors:  ui::showBusy("Loading monitors...");  break;
                    case NetJobType::FetchIncidents: ui::showBusy("Loading incidents..."); break;
                    case NetJobType::FetchSlos:      ui::showBusy("Loading SLOs...");      break;
                    case NetJobType::FetchBitsInvestigations: ui::showBusy("Loading investigations..."); break;
                    default: break;
                }
            }
            if (job.done) {
                switch (job.type) {
                    case NetJobType::PollAmbient:
                        ui::notifyMonitorCountsRefreshed();
                        if (ui::currentScreen() == ui::Screen::Incidents) ui::notifyIncidentsRefreshed();
                        break;
                    case NetJobType::MonitorDetail: {
                        const dd::MonitorDetailResult& r = dd::lastMonitorDetailResult();
                        dd::Monitor m;
                        m.id = r.id;
                        m.status = r.status;
                        m.muted = r.muted;
                        // name/query aren't cached in MonitorDetailResult (DRAM
                        // budget — see its doc comment in datadog.h); re-derive
                        // from the already-cached list by id, same data the
                        // tap handler's instant showMonitorDetailLoading()
                        // used to get this screen on-screen in the first place.
                        for (const dd::Monitor& lm : dd::lastMonitors()) {
                            if (lm.id == r.id) { m.name = lm.name; m.query = lm.query; break; }
                        }
                        m.criticalThreshold = r.criticalThreshold;
                        m.warningThreshold = r.warningThreshold;
                        m.thresholdsApplicable = r.thresholdsApplicable;
                        ui::showMonitorDetail(m, r.chart, r.chartOk, r.err);
                        break;
                    }
                    case NetJobType::FetchOnCall: {
                        const dd::OnCallResult& r = dd::lastOnCallResult();
                        ui::hideBusy();
                        ui::notifyOnCallRefreshed(r.entries, r.hasTeams, r.needsTeamPick);
                        logMemCheckpoint("notifyOnCallRefreshed");
                        break;
                    }
                    case NetJobType::FetchMonitors:
                        ui::hideBusy();
                        ui::notifyMonitorsListRefreshed();
                        logMemCheckpoint("notifyMonitorsListRefreshed");
                        break;
                    case NetJobType::FetchIncidents:
                        ui::hideBusy();
                        ui::notifyIncidentsRefreshed();
                        logMemCheckpoint("notifyIncidentsRefreshed");
                        break;
                    case NetJobType::FetchSlos:
                        ui::hideBusy();
                        ui::notifySlosRefreshed();
                        logMemCheckpoint("notifySlosRefreshed");
                        break;
                    case NetJobType::FetchBitsInvestigations:
                        ui::hideBusy();
                        ui::notifyBitsInvestigationsRefreshed();
                        logMemCheckpoint("notifyBitsInvestigationsRefreshed");
                        break;
                    default: break;
                }
            }
        }

        // Smooth pulse — updated every iteration, not just on poll (BARKBOARD_PLAN.md §5.1).
        // Throttled, not dd::isConfigured() directly — same NVS-log-flood
        // reason as the screen-selection check above.
        if (isConfiguredThrottled()) updateMoodLed();
        if (g_chirpPending) { g_chirpPending = false; chirpNewAlert(); }

        // Monitors/Incidents/SLOs/On-Call list fetches all now run on
        // netTask() (core 0) — see the g_netJob drain above. No dd:: calls
        // for any of them remain here; only SLO detail (single quick call,
        // its own busy spinner already) stays on core 1 below.
        {
            String sloId;
            if (ui::sloDetailRequestPending(sloId)) {
                ui::showBusy("Loading SLO...");
                display::tick();
                dd::SloSummary summary;
                for (const dd::SloSummary& s : dd::lastSlos()) if (s.id == sloId) { summary = s; break; }
                dd::SloStatus status;
                String serr;
                bool ok = dd::fetchSloStatus(sloId, status, serr);
                status.target = summary.target;
                ui::hideBusy();
                ui::showSloDetail(summary, status, ok);
            }
        }

        // Bits Investigation Detail — same "single quick call" pattern as
        // SLO detail just above.
        {
            String invId;
            if (ui::bitsInvestigationDetailRequestPending(invId)) {
                ui::showBusy("Loading investigation...");
                display::tick();
                dd::BitsInvestigationDetail detail;
                String ierr;
                dd::fetchBitsInvestigationDetail(invId, detail, ierr);
                ui::hideBusy();
                ui::showBitsInvestigationDetail(detail, ierr);
            }
        }

        // Settings: re-detect site (same probe as first-time setup, just
        // reusing the already-stored keys) and factory reset confirm.
        if (ui::redetectSitePending()) {
            ui::showBusy("Detecting site...");
            display::tick();
            String site, verr;
            bool ok = dd::validateKeysAndDetectSite(storage::getApiKey(), storage::getAppKey(), site, verr);
            ui::hideBusy();
            ui::applyRedetectResult(ok, site, verr);
        }
        if (ui::factoryResetPending()) {
            performFactoryReset("Settings confirm");
        }

        // Incident state advance (tap on Incident Detail) — default
        // Active -> Stable -> Resolved fallback chain (BARKBOARD_PLAN.md §3.1).
        {
            String incId, newState;
            if (ui::incidentAdvancePending(incId, newState)) {
                ui::showBusy("Updating...");
                display::tick();
                String aerr;
                bool ok = dd::setIncidentState(incId, newState, aerr);
                ui::hideBusy();
                ui::applyIncidentAdvanceResult(ok, newState, ok ? ("Now: " + newState) : ("Fail: " + aerr));
                if (ok) { std::vector<dd::Incident> incidents; dd::fetchIncidents(incidents, aerr); ui::notifyIncidentsRefreshed(); }
            }
        }

        // Monitor Detail drill-in (BARKBOARD_PLAN.md §4) now runs on
        // netTask() (core 0) via dd::fetchMonitorDetailAndChart() — see the
        // g_netJob drain above. No dd:: calls for it remain here.

        // Mute — tapping the action bar's Mute button opens a "1 Hour" /
        // "Today" options screen; long-pressing Unmute (shown once muted)
        // fires straight through. untilEpochSec==0 is the unmute sentinel
        // (see ui::monitorMutePending's doc comment).
        {
            long muteId;
            uint32_t until;
            if (ui::monitorMutePending(muteId, until)) {
                ui::showBusy("Updating...");
                display::tick();
                String merr;
                bool ok, unmuting = (until == 0);
                if (unmuting) ok = dd::unmuteMonitor(muteId, merr);
                else          ok = dd::muteMonitor(muteId, until, merr);
                ui::hideBusy();
                String okMsg = unmuting ? "Unmuted" : (until - (uint32_t)time(nullptr) > 3600 ? "Muted for today" : "Muted for 1h");
                ui::applyMuteResult(ok, ok ? okMsg : ("Fail: " + merr));
            }
        }

        // Declare > Case — Declare > Case > pick-a-project screen creates
        // immediately on project tap.
        {
            String projectId, title;
            if (ui::caseCreatePending(projectId, title)) {
                ui::showBusy("Creating case...");
                display::tick();
                String caseKey, cerr;
                bool ok = dd::createCase(title, projectId, caseKey, cerr);
                ui::hideBusy();
                ui::applyDeclareResult(ok, ok ? ("Created case " + caseKey) : ("Fail: " + cerr));
            }
        }

        // Declare > Incident — single-tap confirm, no severity/project
        // picker yet (see datadog.h's createIncident() doc comment).
        {
            String title;
            if (ui::incidentCreatePending(title)) {
                ui::showBusy("Declaring incident...");
                display::tick();
                String incidentId, ierr2;
                bool ok = dd::createIncident(title, incidentId, ierr2);
                ui::hideBusy();
                ui::applyDeclareResult(ok, ok ? "Incident declared" : ("Fail: " + ierr2));
            }
        }

        // Bits — looks up the monitor's latest alert event, then triggers an
        // investigation against it (see dd::triggerBitsInvestigation()'s doc
        // comment for why it needs the event, not just the monitor id).
        {
            long bitsMonitorId;
            if (ui::bitsTriggerPending(bitsMonitorId)) {
                ui::showBusy("Triggering investigation...");
                display::tick();
                String investigationId, berr;
                bool ok = dd::triggerBitsInvestigation(bitsMonitorId, investigationId, berr);
                ui::hideBusy();
                ui::applyBitsResult(ok, ok ? "Investigation triggered" : ("Fail: " + berr));
            }
        }

        // Declare > Case's project picker — fetched live (org-specific),
        // not cached like the dashboard lists, since it's only needed the
        // moment the user taps "Case".
        if (ui::caseProjectsFetchPending()) {
            ui::showBusy("Loading projects...");
            display::tick();
            std::vector<dd::CaseProject> projects;
            String perr;
            dd::fetchCaseProjects(projects, perr);
            ui::hideBusy();
            ui::notifyCaseProjectsRefreshed(projects);
        }

        // Validate freshly-saved keys from the web portal (off the request thread)
        // and auto-detect the site via the nine-site validate_keys probe
        // (BARKBOARD_PLAN.md §2). This blocks loop() for up to ~25s worst case,
        // same tradeoff the reference project accepts for pd::validateTokenVerbose —
        // the per-probe progress callback keeps the screen from looking hung.
        if (g_ddKeysJustSaved) {
            g_ddKeysJustSaved = false;
            ui::showConnecting("Detecting your Datadog site...");
            display::tick();

            String site, verr;
            bool ok = dd::validateKeysAndDetectSite(
                storage::getApiKey(), storage::getAppKey(), site, verr,
                [](int i, int total, const String& host) {
                    ui::showConnecting("Checking Datadog site " + String(i) + " of " + String(total) + "...");
                    display::tick();
                });

            g_ddKeysValidated   = ok;
            g_ddValidationError = ok ? String() : verr;
            if (ok) {
                ui::showOverview();
                ui::setStatusOnline(true);
            } else {
                ui::showConnecting(String("Couldn't detect your Datadog site.\n") + verr +
                                    "\nRe-enter keys at http://" + ui::deviceHostname() + ".local");
            }
        }

        // Web Settings' /oncall-team picker page — see the doc comment on
        // g_oncallTeamsFetchRequested above for why this stays on core 1.
        if (g_oncallTeamsFetchRequested) {
            g_oncallTeamsFetchRequested = false;
            std::vector<dd::Team> myTeams;
            String terr;
            dd::fetchMyTeams(myTeams, terr);
            g_oncallTeamsFetchDone = true;
        }
    } else {
        wasConnected = false;
        ui::setStatusOnline(false);
    }

    lastLoopWorkMs = millis() - loopIterStart;
    delay(5);
}
