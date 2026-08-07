#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>

// Datadog REST client. Two static keys (DD-API-KEY / DD-APPLICATION-KEY) plus
// a detected site host — see BARKBOARD_PLAN.md §2 for the auth model and the
// nine-site validate_keys probe this replaces pagerduty.cpp's single-token
// validateToken() with.
namespace dd {

struct MonitorCounts {
    int alert = 0;
    int warn = 0;
    int ok = 0;
    int noData = 0;
    bool fetchOk = false;
    String error;
    uint32_t lastFetchMs = 0;
};

struct Monitor {
    long   id = 0;
    String name;
    String status;          // "Alert" | "Warn" | "OK" | "No Data"
    String query;
    String type;             // e.g. "metric alert", "log alert", "synthetics alert" — see fetchMonitorChartSeries
    long   lastTriggeredTs = 0;   // overall_state_modified, unix seconds
    std::vector<String> tags;     // only populated by fetchMonitorDetail()
    bool   muted = false;         // options.silenced non-empty; only populated by fetchMonitorDetail()
    // options.thresholds.{critical,warning}; NAN = not set. Only meaningful as
    // a literal reference line on the raw-metric chart when the query isn't
    // wrapped in a detection function (anomalies/outliers/forecast/change) —
    // those compare a *deviation band*, not the plotted value, against this
    // threshold, so drawing it as a horizontal line would misrepresent what
    // it means. See thresholdsApplicable (set by fetchMonitorChartSeries).
    double criticalThreshold = NAN;
    double warningThreshold  = NAN;
    bool   thresholdsApplicable = false;
    // Only populated by fetchMonitorDetail(), and only for monitors using
    // the newer "formula(...) + options.variables[]" query style (confirmed
    // live: a real log-alert monitor's `query` was literally
    // formula("moving_rollup(query, 300, 'avg')").last("5m") > 100, with no
    // logs("...")-embedded string to extract at all — the actual search
    // string lives here instead). Empty for monitors that don't use this
    // style; fetchNonMetricChartSeries() falls back to the legacy embedded-
    // string extraction (extractWrappedQuery()) when this is blank.
    String variableSearchQuery;
};

struct Incident {
    String id;
    String title;
    String severity;        // "SEV-1".."SEV-5" | "UNKNOWN"
    String state;            // org-configured state name, e.g. "active" | "resolved"
    String createdAt;        // ISO8601
    String commander;        // resolved from the "users" include, "" if unassigned
    std::vector<String> services;
};

struct Team {
    String id;      // UUID — what the on-call API endpoints key off
    String name;    // display name, e.g. "ESE TOLA" — NOT the tag value
    String handle;  // e.g. "ese-tola" — matches the team:/teams: tag convention Monitors/Incidents/SLOs filter on
};

struct SloSummary {
    String id;
    String name;
    String type;             // "monitor" | "time_slice" | "metric"
    double target = 0;       // target_threshold, e.g. 99.9
    String timeframe;        // e.g. "7d", "30d"
    // Populated by fetchSlos() via GET /api/v1/slo/search (which returns
    // current status inline per SLO, unlike GET /api/v1/slo — see its doc
    // comment) so the list can show a health-state dot without an N+1
    // fetchSloStatus() call per row. "breached" | "warning" | "ok" |
    // "no_data" (SLOState); sliValue is the current SLI percentage.
    String state;
    double sliValue = 0;
};

struct SloStatus {
    double sliValue = 0;             // current SLI, percentage
    double target = 0;
    double errorBudgetRemaining = 0; // Datadog's own metric; can go negative once breached
    String state;                    // e.g. "breached", "ok"
};

struct OnCallEntry {
    String user;
    String email;   // "" when the user resource has no email or wasn't found —
                     // shown as secondary metadata on the Overview hero card,
                     // not currently used for the plain escalation rows.
    String schedule;
    int    escalationLevel = 0;
};

struct MetricPoint {
    uint32_t tsSec = 0;
    double   value = 0;
};

struct CaseProject {
    String id;
    String key;   // e.g. "SANDBOX" — short, org-defined
    String name;  // e.g. "Sandbox"
};

// Bits AI Investigation — via GET /api/unstable/bits-ai/investigation/search
// (see fetchBitsInvestigations()'s doc comment for why an /api/unstable/
// endpoint is in use here at all). id is the investigation uuid, usable
// directly with the documented GET /api/v2/bits-ai/investigations/{id} for
// full detail (fetchBitsInvestigationDetail()) — confirmed live these are
// the same identifier, not a different id space.
struct BitsInvestigation {
    String id;
    String title;
    String status;         // real values seen live: "conclusive"; API also
                            // documents "inconclusive"/"failed"/"pending"/"in progress"
    String entitySource;    // e.g. "MONITOR_ENTITY" — best-effort, only one
                             // source type exists in the org this was verified against
    uint32_t modifiedTs = 0; // epoch sec, parsed from modified_timestamp
};

// Fuller detail for one investigation, via the documented
// GET /api/v2/bits-ai/investigations/{id} — deliberately doesn't cache the
// full conclusion `description` (a multi-KB markdown wall of text per the
// live response this was verified against), just title/summary, to keep
// this cheap on the LVGL pool.
struct BitsInvestigationDetail {
    String title;
    String status;
    String conclusionTitle;
    String conclusionSummary;
    bool detailOk = false;
};

using ProgressCb = std::function<void(int index, int total, const String& host)>;

// Probes the known Datadog site hosts (most-common-first) with both headers
// until one returns 200 from /api/v2/validate_keys. On success, persists the
// winning site via storage::setSite() and returns true. onProgress fires
// before each probe so the UI can show "Checking Datadog site N of 9...".
bool validateKeysAndDetectSite(const String& apiKey, const String& appKey,
                                String& outSite, String& err,
                                ProgressCb onProgress = nullptr);

bool isConfigured();   // storage::hasKeys() && storage::hasSite()

// LED/mood-ring inputs only (main.cpp's currentMood()) — reads on EVERY
// core-1 loop() tick, unlike every other dd::lastXxx() above which is only
// read once right after a job-done signal says fresh data is ready. Once
// fetchMonitorCounts()/fetchIncidents() run on the core-0 net task, that
// unconditional continuous read would otherwise be a genuine unsynchronized
// cross-core race (vector reallocation mid-read) — this is guarded by its
// own tiny critical section instead, carrying just the 3 scalars mood needs,
// not the full incidents vector.
void moodInputsSnapshot(int& outAlertCount, int& outWarnCount, bool& outHasCriticalIncident);

// Every fetcher narrows server-side (query params / search syntax) rather
// than pulling everything and filtering on-device — see CLAUDE.md.
bool fetchMonitorCounts(MonitorCounts& out);
const MonitorCounts& lastMonitorCounts();   // cached result of the last fetchMonitorCounts()

// statusFilter: "" for all non-OK (alert+warn+no data), or "alert"/"warn"/"no data"/"ok".
bool fetchMonitors(const String& statusFilter, std::vector<Monitor>& out, String& err, int limit = 14);
const std::vector<Monitor>& lastMonitors();       // cached result of the last fetchMonitors()
void setMonitorFilter(const String& f);           // mirrors pagerduty.cpp's ListFilter — main loop polls with this
const String& getMonitorFilter();

// Single-monitor GET (BARKBOARD_PLAN.md §4 Monitor Detail) — note this
// endpoint uses "overall_state", not monitor/search's "status" field name;
// confirmed against a live org, they're the same value under different keys.
bool fetchMonitorDetail(long monitorId, Monitor& out, String& err);

// ---- Core-split cache accessors ----
// The core-0 net task (main.cpp) does every blocking dd:: call; core 1's
// loop() only ever learns "job <type> finished, ok=<bool>" through a small
// cross-core struct (see main.cpp's NetJobStatus) and then reads the actual
// payload back out through one of these — avoids threading large vectors/
// structs through a portMUX_TYPE critical section, and keeps every list/
// detail render on the same "read once, right after being told data is
// fresh" discipline the pre-split code already used via lastMonitors() etc.

// Fetches monitor detail + its chart series in one call (mirrors what the
// Monitor Detail drill-in already did as two sequential calls) and caches
// the combined result — one "done" signal covers both fetches. Doesn't cache
// name/query at all: Monitor Detail is only ever reached by tapping a row in
// the Monitors list, so that data is already sitting in lastMonitors() (the
// existing showMonitorDetailLoading() screen already relies on exactly this
// for its instant pre-fetch render) — the dispatcher re-derives it by id at
// no extra permanent cost, instead of a second copy living here permanently.
// Only `status`/`muted` are cached, since fetchMonitorDetail()'s live
// "overall_state"/mute-state can be fresher than what the list last saw.
// DRAM here is razor-thin (see CLAUDE.md build history), so every cross-
// core cache added by this refactor needs to earn its bytes.
struct MonitorDetailResult {
    long id = 0;
    String status;
    bool muted = false;
    bool detailOk = false;
    std::vector<MetricPoint> chart;
    bool chartOk = false;
    String err;   // whichever of detail/chart failed; detailOk==false means it's the detail fetch's error
    double criticalThreshold = NAN;
    double warningThreshold  = NAN;
    bool   thresholdsApplicable = false;
};
bool fetchMonitorDetailAndChart(long monitorId);
const MonitorDetailResult& lastMonitorDetailResult();

// Duration-bound snooze — the Datadog analog of pagerduty-cyd's ack/snooze
// (BARKBOARD_PLAN.md §4). NOT verified against a live call (muting a real
// monitor has a real effect on the org), only against the documented shape.
bool muteMonitor(long monitorId, uint32_t untilEpochSec, String& err);
// Clears any active mute (POST .../unmute, documented alongside .../mute).
// NOT verified against a live call, same caveat as muteMonitor().
bool unmuteMonitor(long monitorId, String& err);

// Default capped well below the usual 14-row LVGL-pool-protecting limit —
// confirmed live that individual incident objects here run ~10-12KB each
// (customer_impact_scope, notification_handles, a nested last_modified_by
// user object, etc. — none of which this app reads), so page_size=14 (or
// even 10) can push the raw response into the ~120-130KB range, which
// overflows HTTPClient's buffered String growth (see the "HTTPClient
// buffered-fetch size limits" note in BARKBOARD_PLAN.md §11 — same failure
// class, `json: IncompleteInput`, confirmed live again here). Tried a
// sparse-fieldset query param (fields[incidents]=...) hoping the server
// would send less — confirmed live it doesn't change the response size at
// all, so page_size is the only real lever. 5 items keeps the confirmed-
// live response size (~68KB) with real margin below the ~131KB that just
// failed, not just barely under it.
bool fetchIncidents(std::vector<Incident>& out, String& err, int limit = 5);
const std::vector<Incident>& lastIncidents();

// Cycle to the next state in the default Active -> Stable -> Resolved chain
// (BARKBOARD_PLAN.md §3.1's documented fallback — org-configured state names
// aren't fetched yet since there's no Settings screen to surface an override).
String nextIncidentState(const String& current);
bool setIncidentState(const String& incidentId, const String& newState, String& err);

bool fetchSlos(std::vector<SloSummary>& out, String& err, int limit = 14);
const std::vector<SloSummary>& lastSlos();
bool fetchSloStatus(const String& sloId, SloStatus& out, String& err);

// The teams belonging to whichever user created the API/App key pair —
// GET /api/v2/team?filter[me]=true, confirmed live it resolves correctly
// (every Application Key is tied to the specific user who created it).
// Used to auto-detect On-Call's team instead of the free-text team-scope
// Settings field (that field answers "what should Monitors/Incidents show",
// a different question from "which team's on-call roster").
bool fetchMyTeams(std::vector<Team>& out, String& err);
const std::vector<Team>& lastMyTeams();   // cached for the web Settings picker to render

// Confirmed live against a team with a real active rotation: GET /api/v2/
// on-call/teams/{id}/on-call is a SINGLE JSON:API object, not an array.
// Current responders live in data.relationships.responders.data[] (bare
// {id,type} refs); a broader escalation-policy roster in
// data.relationships.escalations.data[] (each a further ref to an
// escalation_policy_steps object, itself carrying its own
// relationships.responders.data[]); actual names only appear in a top-level
// `included` array, and only when the request passes
// ?include=responders,escalations,escalations.responders at all. This
// endpoint has no concept of a named "schedule" the way the old (unverified,
// wrong) version of this code assumed — OnCallEntry.schedule is populated
// with "Current" / "Escalation step N" instead.
bool fetchOnCallForTeamId(const String& teamId, std::vector<OnCallEntry>& out, String& err);

// Reads storage::getOnCallTeamId(): if set, fetches that team's on-call
// directly. If unset, tries fetchMyTeams() — exactly one team auto-persists
// via storage::setOnCallTeamId() and proceeds; zero or multiple teams can't
// be resolved on-device, so this caches the team list (lastMyTeams(), for
// the web picker) and sets needsTeamPick/hasTeams instead of guessing.
struct OnCallResult {
    std::vector<OnCallEntry> entries;
    bool hasTeams = false;
    bool needsTeamPick = false;
};
bool fetchOnCallAll();
const OnCallResult& lastOnCallResult();
// Last hour by default; caller decimates pointlist for lv_chart. `query`
// must already be a plain metrics-query expression (e.g. "avg:system.cpu.
// idle{*}") — confirmed live that this shape charts fine, but a Monitor's
// raw `query` field is NOT this shape (see fetchMonitorChartSeries).
bool fetchMetricSeries(const String& query, uint32_t fromEpochSec, uint32_t toEpochSec,
                        std::vector<MetricPoint>& out, String& err);

// Monitor Detail's chart entry point — do not call fetchMetricSeries()
// directly with a Monitor's `query` field. Confirmed live: a monitor's
// stored query is an *evaluation* expression like "avg(last_5m):avg:system.
// cpu.user{*} > 80" or "avg(last_4h):anomalies(avg:trace.http.request{env:
// prod} by {service}, 'agile', 2, ...) > 1" — both the "<aggr>(<window>):"
// prefix and the trailing "<op> <threshold>" comparison are evaluation
// syntax /api/v1/query's classic metrics-query parser rejects outright, and
// detection-function wrappers (anomalies/outliers/forecast/...) aren't valid
// there either. This strips the evaluation envelope and, for detection-
// function queries, extracts the underlying metric expression from the
// wrapper's first argument. It also fails closed with a clear message for
// monitor types that have no metrics query at all (log/synthetics/event/
// process/etc. alerts use a different query language or none) instead of
// sending a doomed request.
//
// No from/to params (there used to be) — it looks up the monitor's most
// recent alert event itself and anchors the window there instead of "now"
// (confirmed live: a recovered monitor's "last 1h from now" window is often
// just a flat, uneventful line; anchoring on when it actually fired shows
// the real spike), falling back to a live "now" window if there's no
// recent event. It also scopes a "by {tag}" query down to the specific
// group that alerted (from the event's monitor_groups) instead of leaving
// fetchMetricSeries() to grab an arbitrary one of however many distinct
// tag values exist — confirmed live that's not necessarily the one that
// alerted at all.
bool fetchMonitorChartSeries(const Monitor& monitor, std::vector<MetricPoint>& out, String& err);

// Equivalent chart sources for "log alert" / "trace-analytics alert" /
// "rum alert" monitors, none of which have a metrics query at all
// (fetchMonitorChartSeries() correctly refuses all three). Each pulls a
// count timeseries from its respective Aggregate API instead, over the same
// alert-anchored window; see the shared fetchNonMetricChartSeries() in
// datadog.cpp. Dispatched by monitor.type in fetchMonitorDetailAndChart().
bool fetchLogMonitorChartSeries(const Monitor& monitor, std::vector<MetricPoint>& out, String& err);
bool fetchTraceMonitorChartSeries(const Monitor& monitor, std::vector<MetricPoint>& out, String& err);
bool fetchRumMonitorChartSeries(const Monitor& monitor, std::vector<MetricPoint>& out, String& err);

// ---- Declare: Case / Incident (Monitor Detail action bar) ----

// Cases need a project (org-specific, so fetched live) — everything else
// about a case (type, priority) is a fixed v1 default below, since there's
// no on-panel keyboard to fill in more than that. Confirmed live: this
// org has 2 projects ("Sandbox", "Error Tracking").
bool fetchCaseProjects(std::vector<CaseProject>& out, String& err);

// title is auto-generated from the monitor (no keyboard on this device —
// see BARKBOARD_PLAN.md's Monitor Detail action bar). type_id is hardcoded
// to Datadog's built-in "Standard" case type (00000000-0000-0000-0000-
// 000000000001, confirmed live via GET /api/v2/cases/types — this ID looks
// like a product-wide constant, not org-specific, but that's inferred, not
// documented) and priority to "P3" — a type/priority picker is a reasonable
// follow-up, not done here. Returns the created case's human-readable key
// (e.g. "SANDBOX-1"), which is what you'd actually go look for in Datadog.
bool createCase(const String& title, const String& projectId, String& outCaseKey, String& err);

// Only `title` is required (confirmed live — an otherwise-empty attributes
// object fails solely on "title cannot be empty"); severity/customer-impact
// are left at the org's own defaults for the same no-keyboard reason as
// createCase(). Returns the created incident's id (same id shape used by
// setIncidentState()/lastIncidents()).
bool createIncident(const String& title, String& outIncidentId, String& err);

// ---- Declare: Bits AI Investigation (Monitor Detail action bar) ----

// Triggering an investigation needs the monitor's *latest alert event*
// (event_id + event_ts), not just the monitor id — confirmed live via
// POST /api/v2/bits-ai/investigations' validation errors, and the actual
// event_id/event_ts values were confirmed against a real event found via
// GET /api/v2/events/search?query=source:alert @monitor.id:<id>. Wraps
// both calls: looks up the latest event, then triggers. Returns the new
// investigation's id.
bool triggerBitsInvestigation(long monitorId, String& outInvestigationId, String& err);

// GET /api/unstable/bits-ai/investigation/search — an internal/undocumented
// endpoint (the web UI's own investigations page uses it), NOT the
// documented /api/v2/bits-ai/investigations list endpoint, which only
// supports filter[monitor_id] (one monitor at a time, no team/broad facet —
// confirmed against docs.datadoghq.com). Returns richer per-item data
// (status, entity source, modified timestamp) in one request instead of a
// fan-out of one GET per monitor. Confirmed live it accepts the standard
// DD-API-KEY/DD-APPLICATION-KEY headers despite the /api/unstable/ path —
// but that also means Datadog can change or remove it without the
// compatibility guarantees /api/v2/ has; if this starts failing after a
// Datadog platform change, that's the likely cause.
//
// One call, server-side team-scoped (same "team:x" convention as Monitors/
// Incidents/SLOs) — see the implementation's comment for why scoping also
// matters for keeping the response small enough to fetch reliably, not just
// for correctness.
bool fetchBitsInvestigations(std::vector<BitsInvestigation>& out, String& err);
const std::vector<BitsInvestigation>& lastBitsInvestigations();

// The documented, stable GET /api/v2/bits-ai/investigations/{id} — used for
// the investigation detail screen once a row from fetchBitsInvestigations()
// is tapped.
bool fetchBitsInvestigationDetail(const String& investigationId, BitsInvestigationDetail& out, String& err);

// ---- Device self-monitoring (opt-in, storage::getMetricsEnabled()) ----

// Submits a handful of device-health gauges to Datadog's own Metrics API
// (POST /api/v2/series) — auth is DD-API-KEY only, no app key needed
// (confirmed: application keys are for read/query endpoints, not metric
// submission).
//
// Deliberately doesn't include LVGL's own memory pool stats (lv_mem_monitor())
// even though they'd be a genuinely interesting gauge — that call touches
// LVGL's shared, non-thread-safe internal state, and this function runs on
// netTask() (core 0), not the LVGL-owning core 1. This project's own
// CLAUDE.md rule ("never touch LVGL objects from the network task") and an
// entire earlier debugging session (the lv_chart_remove_series() TLSF hang)
// exist specifically because that boundary matters here more than most
// Arduino projects. Everything sent below (ESP.getFreeHeap() and friends,
// WiFi.RSSI(), millis()) is either already read from netTask() elsewhere in
// this file or is core-0-native (WiFi state), so none of it crosses that
// line.
//
// Tagged service:barkboard (so this shows up under the normal Datadog
// service convention, not just as an untagged custom metric), device:<AP
// SSID> (the same "BarkBoard-XXXX" identifier already shown during setup,
// via netcfg::apSsid()), and firmware_version:<the BARKBOARD_VERSION build
// stamp>, plus static hardware tags (chip model/revision, CPU freq, flash
// size, SDK version) and boot_reason:<the last esp_reset_reason(), e.g.
// "panic"/"task_wdt"/"poweron"> — enough to tell multiple devices apart and
// to slice metrics by "was this boot preceded by a crash" without a
// separate query.
//
// Per-task stack headroom (uxTaskGetStackHighWaterMark()) is included for
// the two tasks this project creates — the Arduino "loopTask" (core 1, LVGL)
// and "dd-net" (core 0, this file) — as barkboard.task.stack_free, tagged
// task:loop / task:dd-net. That call is a plain FreeRTOS kernel query (its
// own internal locking makes it safe to call cross-core for another task's
// handle) — not an LVGL call, so it doesn't cross the boundary described
// above. loopTaskHandle must be captured once in setup() via
// xTaskGetCurrentTaskHandle() (before FreeRTOS reassigns "current task" to
// something else) and passed in here; this function calls
// xTaskGetCurrentTaskHandle() itself to get dd-net's own handle, since it
// always runs on that task.
//
// Real per-task CPU% (what sysmon and ESP32-Task-Manager report) is NOT
// available — confirmed against this project's pinned framework
// (framework-arduinoespressif32@3.20016.0)'s prebuilt esp32 sdkconfig that
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS and CONFIG_FREERTOS_USE_TRACE_FACILITY
// are both off, so vTaskGetRunTimeStats()/uxTaskGetSystemState() aren't
// available — and the FreeRTOS/IDF component here ships as a prebuilt
// static lib for this target, not something a plain PlatformIO build
// recompiles from source with different config. PSRAM stats are also
// skipped — this specific board has none (see platformio.ini's
// BOARD_HAS_PSRAM=0 build flag).
//
// loopBusyPct/netBusyPct are a coarse, honest stand-in instead: each of
// main.cpp's two tasks times how much of its own loop cycle is spent
// working versus asleep in delay(5)/vTaskDelay(5), and passes the running
// average since the last report here — sent as barkboard.task.busy_pct,
// tagged task:loop/task:dd-net alongside stack_free above. This is NOT
// true CPU utilization (no ISR time, no WiFi driver internals, nothing
// outside these two tasks' own accounting), just "is this task's own work
// starting to crowd out its sleep" — but it's cheap enough (a couple of
// millis() reads per iteration, no new tasks) to always compute. Pass -1
// for either when no full cycle has completed yet since the last report
// (e.g. right after boot) — skipped rather than sending a meaningless value.
//
// Also sends barkboard.api.calls (type count, tagged endpoint:<name>) — a
// sum-since-last-report tally of real HTTP requests this device made to
// Datadog, one series per endpoint (monitor_search, incident_search,
// oncall_get, metrics_submit, etc. — see datadog.cpp's recordApiCall() call
// sites for the full tag list). Retries count as separate calls since
// they're separate real requests against Datadog's API. The tally is
// swapped out (not just copied) each call, so a slow/failed report doesn't
// double-count the same calls into the next interval.
bool submitDeviceMetrics(TaskHandle_t loopTaskHandle, float loopBusyPct, float netBusyPct, String& err);

// One-shot per boot (call once from netTask() after the first successful
// connection, guarded by a local "already ran" bool — same pattern as this
// file's team-scope auto-fetch), only while storage::getEventsEnabled() is
// on — Events are billable Datadog usage just like custom metrics, but a
// distinct product, so this has its own opt-in toggle rather than sharing
// submitDeviceMetrics()'s. Reads
// esp_reset_reason() and, only when it indicates the previous boot did NOT
// end cleanly (panic, either watchdog, brownout — not a plain
// esp_restart()/power-on), fires a Datadog Event (POST /api/v1/events,
// still DD-API-KEY only) so a crash shows up as a discrete, alertable
// occurrence rather than something you'd only notice by staring at a
// reboot-count graph. Always returns true on a clean boot without making
// any network call — nothing to report.
bool reportBootEvent(String& err);

// One-shot per firmware version (not per boot) — pushes short_name/unit/
// description metadata (PUT /api/v1/metrics/<name>) for every barkboard.*
// metric, so a brand-new device reporting metrics for the first time shows
// up labeled in Datadog's UI (metrics explorer, dashboards, monitor
// creation) instead of as bare unlabeled gauges — without requiring anyone
// to separately find and run tools/push_metric_metadata.py by hand. Skips
// entirely (returns true, no network call) once storage::getMetricMetadataVersion()
// already matches BARKBOARD_VERSION; only advances that stored version once
// every metric's PUT succeeds, so a transient failure just retries whole on
// the next metrics-report interval rather than leaving some metrics
// permanently unlabeled. Gate this call on storage::getMetricsEnabled()
// (opt-in) at the call site, same as submitDeviceMetrics() — metric
// metadata is meaningless if you've never opted into sending the metrics
// it describes.
bool pushMetricMetadataIfNeeded(String& err);

}
