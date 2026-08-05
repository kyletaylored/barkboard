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
};

struct SloStatus {
    double sliValue = 0;             // current SLI, percentage
    double target = 0;
    double errorBudgetRemaining = 0; // Datadog's own metric; can go negative once breached
    String state;                    // e.g. "breached", "ok"
};

struct OnCallEntry {
    String user;
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

// Bits AI Investigation — list response only exposes title/status (confirmed
// against docs.datadoghq.com; no monitor/team/timestamp field is returned),
// which is also why "filtered to our team" has to be done by calling
// filter[monitor_id] once per team monitor rather than one broad fetch.
struct BitsInvestigation {
    String id;
    String title;
    String status;
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

bool fetchIncidents(std::vector<Incident>& out, String& err, int limit = 14);
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

// No team/monitor-list filter exists server-side for this list endpoint
// (confirmed against docs.datadoghq.com — only filter[monitor_id] is
// supported, no team facet) — this fans out filter[monitor_id] across the
// monitor ids already visible to the Monitors screen (i.e. already scoped
// to storage::getTeamScope()) and merges the results, same fan-out pattern
// fetchOnCallAll()/fetchMyTeams() used to use before On-Call moved to a
// single resolved team id.
bool fetchBitsInvestigationsForMonitors(const std::vector<long>& monitorIds,
                                         std::vector<BitsInvestigation>& out, String& err);

}
