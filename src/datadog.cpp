#include "datadog.h"
#include "config.h"
#include "storage.h"
#include "wifi_setup.h"   // netcfg::apSsid() — submitDeviceMetrics()'s device: tag

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_system.h>   // esp_reset_reason() — reportBootEvent()
#include <map>

namespace dd {

// Per-endpoint call tally, drained into barkboard.api.calls each metrics
// report (see submitDeviceMetrics()) — small fixed set of string keys (one
// per endpoint tag below), never touched from core 1, so no mutex needed:
// every dd:: fetch/mutate call, and the report/reset itself, only ever runs
// on netTask (core 0).
static std::map<String, uint32_t> g_apiCallCounts;
static void recordApiCall(const char* endpoint) {
    if (endpoint) g_apiCallCounts[endpoint]++;
}

// Probe order per BARKBOARD_PLAN.md §2 — most-common-first so the typical
// case (US1 or EU) resolves in one or two hops rather than the ~25s worst case.
static const char* const SITE_HOSTS[] = {
    "datadoghq.com", "datadoghq.eu", "us3.datadoghq.com", "us5.datadoghq.com",
    "ap1.datadoghq.com", "ap2.datadoghq.com", "uk1.datadoghq.com",
    "ddog-gov.com", "us2.ddog-gov.com",
};
static const int SITE_HOST_COUNT = sizeof(SITE_HOSTS) / sizeof(SITE_HOSTS[0]);

// Declared up here (not next to fetchMyTeams() further down) so
// bareTeamScope() below can read it — the auto-detected scope fallback.
static std::vector<Team> g_lastMyTeams;
const std::vector<Team>& lastMyTeams() { return g_lastMyTeams; }

static String urlEncode(const String& s) {
    String out; out.reserve(s.length() * 3);
    const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.length(); ++i) {
        uint8_t c = (uint8_t)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// Monitors/Incidents/Bits Investigations no longer have their own separate
// "Team" field — it was just duplicating whatever team a user would also
// pick for On-Call, so this derives the scope from the same source On-Call
// uses instead (see NVS_KEY_ONCALL_TEAM_ID's doc comment in config.h).
static String bareTeamScope() {
    // Exactly one team auto-detected from filter[me] (see netTask()'s doc
    // comment in main.cpp for when this gets fetched): unambiguous, use it.
    if (g_lastMyTeams.size() == 1) return g_lastMyTeams[0].handle;

    // More than one team to choose from — if the user has already picked
    // one for On-Call, reuse that same team's handle here too, rather than
    // leaving these screens unscoped just because filter[me] alone couldn't
    // disambiguate. g_lastMyTeams still has the full list either way
    // (fetchMyTeams() populates it unconditionally), so this is a plain
    // lookup, not a second API call.
    String ocTeamId = storage::getOnCallTeamId();
    if (ocTeamId.length()) {
        for (const Team& t : g_lastMyTeams) {
            if (t.id == ocTeamId) return t.handle;
        }
    }
    return "";
}

// These translate the bare value into each screen's own filter syntax —
// monitors/SLOs key off the "team:x" tag convention, incidents key off a
// custom field literally named "teams" (plural). "" (no scope configured)
// passes through as "".
static String monitorTeamScope() {
    String team = bareTeamScope();
    return team.length() ? ("team:" + team) : "";
}
static String incidentTeamScope() {
    String team = bareTeamScope();
    return team.length() ? ("teams:" + team) : "";
}

static String apiBase() {
    String site = storage::getSite();
    if (site.length() == 0) site = SITE_HOSTS[0];
    return "https://api." + site;
}

bool isConfigured() { return storage::hasKeys() && storage::hasSite(); }

// Retry up to twice on transient TLS/connect errors (-1, -5, -11) — same
// backoff pattern as pagerduty.cpp's updateIncidentStatus, reused verbatim.
//
// Parses directly from the HTTPClient's stream rather than buffering the
// whole response into a String first — buffering doubles peak memory (raw
// text + parsed tree) which was enough to exhaust heap on a real org's
// monitor-facets response ("json: NoMemory", followed shortly by unrelated-
// looking LVGL crashes that were really just heap exhaustion surfacing
// somewhere else). Pass `filter` to skip parsing fields the caller doesn't
// use at all — see fetchMonitorCounts for why that matters more than it
// sounds like it should: the facets response's tag/type breakdowns scale
// with the whole org's monitor fleet, not with per_page.
// buffered=true parses from a fully-buffered String instead of streaming
// directly off http.getStream() — confirmed live this is necessary for
// /api/v1/query specifically: Datadog serves that endpoint's response
// chunked (Transfer-Encoding: chunked, no Content-Length — confirmed via
// curl -D-), and ArduinoJson's stream parser choked on it ("json:
// InvalidInput", reproducible on real hardware but never via curl/python,
// which both dechunk transparently before your code ever sees the bytes).
// HTTPClient::getString() dechunks correctly; getStream() being handed
// straight to a 3rd-party Stream consumer over a chunked response is a
// known-shaky combination on this platform. Only opt into this for
// responses with a known-bounded size (a metric series' point count is
// capped) — the facets-response memory blowup this function's streaming
// design originally existed to avoid is still a real risk for anything
// org-size-dependent.
static bool httpGetJsonRetrying(const char* endpoint, const String& url, JsonDocument& doc, String& err,
                                 const JsonDocument* filter = nullptr, bool buffered = false) {
    // The URL already contains the exact query/filter that went out —
    // logging it here (once, not per retry) covers every screen's fetch
    // automatically, since every GET-based one goes through this same
    // helper. No secrets in it: API/App keys are headers, never query params.
    // 2 attempts, not 3, and shorter timeouts than before — confirmed live
    // that a single stalled query (every attempt timing out on the same
    // slow/complex metric aggregation) can chain 3x ~20s timeouts into a
    // 70+ second stretch. That's not just a slow fetch: the core-split
    // architecture (main.cpp) puts this call on its own core so LVGL/touch
    // stay responsive regardless of how long it takes, but portal.cpp's web
    // server (running on core 1) shares the ESP32's underlying TCP/IP stack
    // with whatever this call is doing on core 0 — a long enough stall here
    // can still stall portal::loop()'s handleClient(), which runs before
    // updateMoodLed() in loop(), reading as a full freeze even though it
    // isn't LVGL itself blocking. Shortening the worst case here doesn't
    // fix that coupling, just shrinks how bad it can get until it does.
    // Logged once per call (not per retry, that's already noisy enough) —
    // the useHTTP10 fix for this endpoint's chunked-transfer freeze didn't
    // actually resolve it on real hardware, so the next real capture needs
    // to answer a different question: is this specifically the 3rd
    // sequential HTTPS/TLS call in Monitor Detail's drill-in (detail ->
    // events search -> chart) running into heap exhaustion/fragmentation
    // from back-to-back mbedTLS sessions, not a chunked-encoding issue at
    // all? Each TLS session's buffers are large relative to this device's
    // total heap.
    // getMaxAllocHeap(), not just getFreeHeap() — total free bytes can look
    // perfectly healthy while still being too fragmented for the one/few
    // large contiguous buffers mbedTLS's TLS session actually needs; this
    // is the metric that would actually reveal that specific failure mode.
    Serial.printf("[dd] GET %s (free heap: %u, largest block: %u)\n",
                  url.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    for (int attempt = 0; attempt < 2; ++attempt) {
        // The failure that triggered this retry is already logged immediately
        // below (at the point it happened), not repeated here.
        if (attempt > 0) delay(600);
        err = "";

        String apiKey = storage::getApiKey();
        String appKey = storage::getAppKey();

        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(10);
        client.setTimeout(10000);

        HTTPClient http;
        http.setReuse(false);
        http.setTimeout(10000);
        http.setConnectTimeout(8000);
        http.setUserAgent("barkboard/1.0");
        // buffered==true marks the one endpoint (/api/v1/query) confirmed
        // live to always respond with Transfer-Encoding: chunked, no
        // Content-Length — a combination that's caused repeated real
        // freezes on this ESP32 HTTPClient (both a truncated "IncompleteInput"
        // parse on a large multi-series response, and, confirmed live this
        // time with curl -D-, a full freeze on a TINY 7KB single-series
        // response too — ruling out payload size as the cause and pointing
        // at HTTPClient's chunked-decode path itself being flaky here).
        // Also confirmed live: declaring HTTP/1.0 makes Datadog drop chunked
        // encoding for this endpoint entirely (falls back to Connection:
        // close instead) — sidesteps the flaky decode path rather than
        // trying to fix it.
        if (buffered) http.useHTTP10(true);

        if (!http.begin(client, url)) { err = "http begin failed"; continue; }
        http.addHeader("DD-API-KEY", apiKey);
        http.addHeader("DD-APPLICATION-KEY", appKey);
        http.addHeader("Accept", "application/json");
        http.addHeader("Connection", "close");

        recordApiCall(endpoint);   // counts the real network attempt, including retries — a retry is a second real call against Datadog's API
        int code = http.GET();
        bool jsonFailed = false;
        if (code == 200) {
            DeserializationError je;
            if (buffered) {
                String body = http.getString();
                je = filter
                    ? deserializeJson(doc, body, DeserializationOption::Filter(*filter))
                    : deserializeJson(doc, body);
            } else {
                je = filter
                    ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter))
                    : deserializeJson(doc, http.getStream());
            }
            http.end();
            if (!je) return true;
            // Confirmed live this isn't confined to the chunked/buffered
            // path — an IncompleteInput on plain streaming too (incidents/
            // search), with no hang involved, just a one-off truncated read.
            // Worth a retry like any other transient failure instead of
            // giving up on the spot, which is what this used to do.
            err = String("json: ") + je.c_str();
            jsonFailed = true;
        } else if (code <= 0) {
            err = String("HTTP ") + code + " (" + HTTPClient::errorToString(code) + ")";
            http.end();
        } else {
            String body = http.getString();
            if (body.length() > 160) body = body.substring(0, 160) + "...";
            err = "HTTP " + String(code);
            if (body.length()) err += " — " + body;
            http.end();
        }
        // Logged immediately, not deferred to the next retry's log line —
        // on the last attempt there IS no next iteration, so this was the
        // one real failure mode with zero serial output at all: a pure
        // connect/handshake/read timeout (code<=0) on the final attempt.
        Serial.printf("[dd] GET %s -> %s%s (attempt %d)\n",
                      url.c_str(), err.c_str(), jsonFailed ? " (HTTP 200)" : "", attempt);

        if (!jsonFailed && code != -1 && code != -5 && code != -11) break;   // permanent error, no retry
    }
    Serial.printf("[dd] GET %s -> giving up: %s\n", url.c_str(), err.c_str());
    return false;
}

// Shared POST/PATCH helper — every mutation (mute/unmute, incident state,
// case/incident create, Bits trigger) was duplicating this same connect/
// header/send/error-format boilerplate. Deliberately NOT retried like
// httpGetJsonRetrying() above: retrying a mutation on a transient error
// risks silently doing it twice (double-muting is harmless, but double-
// creating a case or declaring two incidents from one tap isn't).
// outDoc is optional — pass nullptr for calls that don't need the response
// body (mute/unmute/setIncidentState just check the status code).
static bool httpMutateJson(const char* endpoint, const char* method, const String& url, const String& body,
                            JsonDocument* outDoc, String& err, const JsonDocument* filter = nullptr) {
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    // Covers every POST/PATCH call through this one helper — includes
    // genuine mutations (mute, case/incident create, Bits trigger) as well
    // as events/search, which is a read sent as POST since the query is too
    // long/structured for a query string. No secrets in url/body: API/App
    // keys are headers, never part of either.
    Serial.printf("[dd] %s %s body=%s (free heap: %u)\n", method, url.c_str(), body.c_str(), ESP.getFreeHeap());

    String apiKey = storage::getApiKey();
    String appKey = storage::getAppKey();
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(20);
    client.setTimeout(20000);
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(20000);
    http.setConnectTimeout(15000);
    if (!http.begin(client, url)) { err = "http begin"; return false; }
    http.addHeader("DD-API-KEY", apiKey);
    http.addHeader("DD-APPLICATION-KEY", appKey);
    http.addHeader("Content-Type", "application/json");

    recordApiCall(endpoint);
    int code;
    if (strcmp(method, "POST") == 0)      code = http.POST(body);
    else if (strcmp(method, "PATCH") == 0) code = http.PATCH(body);
    else if (strcmp(method, "PUT") == 0)   code = http.PUT(body);
    else { err = "unsupported method"; http.end(); return false; }

    bool ok = (code >= 200 && code < 300);
    if (ok && outDoc) {
        DeserializationError je = filter
            ? deserializeJson(*outDoc, http.getStream(), DeserializationOption::Filter(*filter))
            : deserializeJson(*outDoc, http.getStream());
        http.end();
        if (je) {
            err = String("json: ") + je.c_str();
            Serial.printf("[dd] %s %s -> HTTP %d but %s\n", method, url.c_str(), code, err.c_str());
            return false;
        }
        return true;
    }
    if (!ok) {
        String resp = http.getString();
        if (resp.length() > 200) resp = resp.substring(0, 200) + "...";
        err = "HTTP " + String(code);
        if (resp.length()) err += " — " + resp;
    }
    http.end();
    return ok;
}

bool validateKeysAndDetectSite(const String& apiKey, const String& appKey,
                                String& outSite, String& err, ProgressCb onProgress) {
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    for (int i = 0; i < SITE_HOST_COUNT; ++i) {
        const char* host = SITE_HOSTS[i];
        if (onProgress) onProgress(i + 1, SITE_HOST_COUNT, host);
        Serial.printf("[dd] probing site %d/%d: %s\n", i + 1, SITE_HOST_COUNT, host);

        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(4);
        client.setTimeout(4000);

        HTTPClient http;
        http.setReuse(false);
        http.setTimeout(4000);
        http.setConnectTimeout(4000);
        http.setUserAgent("barkboard/1.0");

        String url = String("https://api.") + host + "/api/v2/validate_keys";
        if (!http.begin(client, url)) { continue; }
        http.addHeader("DD-API-KEY", apiKey);
        http.addHeader("DD-APPLICATION-KEY", appKey);
        http.addHeader("Accept", "application/json");
        http.addHeader("Connection", "close");

        recordApiCall("validate_keys");
        int code = http.GET();
        http.end();
        Serial.printf("[dd]   -> HTTP %d\n", code);

        if (code == 200) {
            outSite = host;
            storage::setSite(outSite);
            return true;
        }
    }

    err = "couldn't find a Datadog org with those keys — check for typos or a revoked key";
    return false;
}

static MonitorCounts      g_lastCounts;
static std::vector<Monitor> g_lastMonitors;
static String             g_monitorFilter;   // "" = all

const MonitorCounts& lastMonitorCounts() { return g_lastCounts; }
const std::vector<Monitor>& lastMonitors() { return g_lastMonitors; }
void setMonitorFilter(const String& f) { g_monitorFilter = f; }
const String& getMonitorFilter() { return g_monitorFilter; }

// See moodInputsSnapshot()'s doc comment in datadog.h — the one dd:: value
// read continuously from core 1 outside the normal job-done discipline, so
// it gets its own tiny lock instead of relying on the "read only after
// done==true" convention every other cache here follows.
static portMUX_TYPE g_moodMux = portMUX_INITIALIZER_UNLOCKED;
static int  g_moodAlert = 0;
static int  g_moodWarn = 0;
static bool g_moodHasCritical = false;

void moodInputsSnapshot(int& outAlertCount, int& outWarnCount, bool& outHasCriticalIncident) {
    portENTER_CRITICAL(&g_moodMux);
    outAlertCount = g_moodAlert;
    outWarnCount = g_moodWarn;
    outHasCriticalIncident = g_moodHasCritical;
    portEXIT_CRITICAL(&g_moodMux);
}

bool fetchMonitorCounts(MonitorCounts& out) {
    out.fetchOk = false;
    out.error = "";
    if (WiFi.status() != WL_CONNECTED) { out.error = "no WiFi"; return false; }

    // per_page=1 keeps the *monitors* array in the response tiny, but the
    // "counts" facets (status/type/tag breakdowns) summarize the entire
    // matching set regardless of page size — on a large real org the
    // tag facet alone (one entry per distinct tag value across every
    // monitor) can be big enough to exhaust heap. Filter to counts.status
    // only; nothing else here is ever read.
    String url = apiBase() + "/api/v1/monitor/search?query=" + urlEncode(monitorTeamScope()) + "&per_page=1";
    JsonDocument filter;
    filter["counts"]["status"] = true;
    JsonDocument doc;
    if (!httpGetJsonRetrying("monitor_search", url, doc, out.error, &filter)) {
        out.lastFetchMs = millis();
        g_lastCounts = out;
        return false;
    }

    out.alert = out.warn = out.ok = out.noData = 0;
    for (JsonObject bucket : doc["counts"]["status"].as<JsonArray>()) {
        String name = bucket["name"] | "";
        int count = bucket["count"] | 0;
        if      (name.equalsIgnoreCase("Alert"))   out.alert = count;
        else if (name.equalsIgnoreCase("Warn"))    out.warn = count;
        else if (name.equalsIgnoreCase("OK"))      out.ok = count;
        else if (name.equalsIgnoreCase("No Data")) out.noData = count;
    }
    out.fetchOk = true;
    out.lastFetchMs = millis();
    g_lastCounts = out;
    portENTER_CRITICAL(&g_moodMux);
    g_moodAlert = out.alert;
    g_moodWarn = out.warn;
    portEXIT_CRITICAL(&g_moodMux);
    return true;
}

bool fetchMonitors(const String& statusFilter, std::vector<Monitor>& out, String& err, int limit) {
    out.clear();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    String query;
    String sf = statusFilter; sf.toLowerCase();
    if (sf == "alert")        query = "status:alert";
    else if (sf == "warn")    query = "status:warn";
    else if (sf == "no data") query = "status:\"no data\"";
    else if (sf == "ok")      query = "status:ok";
    // else: "" / "all" — no status term narrows nothing beyond scope, server still caps via per_page.

    String scope = monitorTeamScope();
    if (scope.length()) query = query.length() ? (query + " " + scope) : scope;

    String url = apiBase() + "/api/v1/monitor/search?query=" + urlEncode(query) +
                 "&per_page=" + String(limit) + "&sort=status";
    // Monitor objects carry a lot we never use — templated alert `message`
    // bodies especially can run several KB each. Filtering to just what we
    // read keeps a 14-item page cheap regardless of how verbose the org's
    // monitors are.
    JsonDocument filter;
    JsonObject monFilter = filter["monitors"].add<JsonObject>();
    monFilter["id"] = true;
    monFilter["name"] = true;
    monFilter["status"] = true;
    monFilter["query"] = true;
    monFilter["type"] = true;
    monFilter["overall_state_modified"] = true;
    JsonDocument doc;
    if (!httpGetJsonRetrying("monitor_search", url, doc, err, &filter)) return false;

    for (JsonObject m : doc["monitors"].as<JsonArray>()) {
        Monitor mon;
        mon.id              = m["id"]                     | 0L;
        mon.name            = m["name"]                    | "";
        mon.status          = m["status"]                  | "";
        mon.query           = m["query"]                   | "";
        mon.type            = m["type"]                    | "";
        mon.lastTriggeredTs = m["overall_state_modified"]   | 0L;
        out.push_back(mon);
        if ((int)out.size() >= limit) break;   // belt-and-suspenders cap for the LVGL pool
    }
    g_lastMonitors = out;
    return true;
}

bool fetchMonitorDetail(long monitorId, Monitor& out, String& err) {
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    String url = apiBase() + "/api/v1/monitor/" + String(monitorId);
    // Unfiltered, not the previous field-filtered fetch — a nested array
    // filter (needed below for options.variables[]) is the same shape
    // confirmed live to silently match zero items elsewhere in this file
    // (HTTP 200, no parse error, just an empty result indistinguishable
    // from "this monitor has no variables"). This payload only runs ~1-1.5KB
    // even unfiltered (confirmed against a real monitor), nowhere near the
    // multi-KB-per-item cost that made unfiltered parsing worth avoiding for
    // the Bits investigations list — not worth the filter risk here either.
    JsonDocument doc;
    if (!httpGetJsonRetrying("monitor_get", url, doc, err)) return false;

    out.id     = doc["id"]             | monitorId;
    out.name   = doc["name"]           | "";
    // This endpoint names the field "overall_state", not monitor/search's
    // "status" — same values, confirmed against a live monitor.
    out.status = doc["overall_state"]  | "";
    out.query  = doc["query"]          | "";
    out.type   = doc["type"]           | "";
    out.tags.clear();
    for (JsonVariant t : doc["tags"].as<JsonArray>()) out.tags.push_back(t.as<String>());
    // options.silenced is a map of scope -> mute-end-epoch (or null =
    // indefinite); any entries at all means the monitor is currently muted
    // somewhere. Not verified live (no muted monitor in the test org at the
    // time this was written) — best-effort against the documented shape.
    out.muted = doc["options"]["silenced"].as<JsonObject>().size() > 0;
    // Newer "formula(...) + options.variables[]" monitor query style —
    // confirmed live against a real log-alert monitor whose top-level
    // `query` is literally formula("moving_rollup(query, 300, 'avg')")
    // .last("5m") > 100, with the actual chartable search string buried in
    // variables[0].search.query instead of embedded as logs("...") the way
    // extractWrappedQuery() expects. Only the first variable is read — this
    // org's real monitors only ever define one, and a multi-variable
    // formula monitor's chart wouldn't map cleanly onto a single search
    // string anyway. Left blank (checked via .length() by
    // fetchNonMetricChartSeries()) for the many monitors that don't use
    // this style at all, so the legacy embedded-string extraction still
    // applies to those.
    out.variableSearchQuery = doc["options"]["variables"][0]["search"]["query"] | "";
    JsonVariant crit = doc["options"]["thresholds"]["critical"];
    JsonVariant warn = doc["options"]["thresholds"]["warning"];
    out.criticalThreshold = crit.is<double>() ? crit.as<double>() : NAN;
    out.warningThreshold  = warn.is<double>() ? warn.as<double>() : NAN;
    return true;
}

bool muteMonitor(long monitorId, uint32_t untilEpochSec, String& err) {
    String url = apiBase() + "/api/v1/monitor/" + String(monitorId) + "/mute";
    String body = String("{\"scope\":\"*\",\"end\":") + String(untilEpochSec) + "}";
    return httpMutateJson("monitor_mute", "POST", url, body, nullptr, err);
}

bool unmuteMonitor(long monitorId, String& err) {
    String url = apiBase() + "/api/v1/monitor/" + String(monitorId) + "/unmute";
    return httpMutateJson("monitor_unmute", "POST", url, "{\"scope\":\"*\"}", nullptr, err);
}

static std::vector<Incident> g_lastIncidents;
const std::vector<Incident>& lastIncidents() { return g_lastIncidents; }

bool fetchIncidents(std::vector<Incident>& out, String& err, int limit) {
    out.clear();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    // GET /api/v2/incidents with filter[state]=active silently ignores that
    // parameter — confirmed live: an obviously-invalid state value returned
    // the exact same results as "active". The dedicated search endpoint
    // actually filters (confirmed live: query=state:resolved vs state:active
    // correctly included/excluded a known resolved incident) and is simpler
    // besides — commander comes back inline, no separate include=users +
    // manual id lookup needed. Scope query (Settings page, e.g. "teams:xyz" —
    // incidents' team-association field is a custom field literally named
    // "teams", plural, unlike monitors' "team:" tag) ANDs on as a
    // space-separated term.
    String query = "state:active";
    String scope = incidentTeamScope();
    if (scope.length()) query += " " + scope;

    String url = apiBase() + "/api/v2/incidents/search?query=" + urlEncode(query) +
                 "&page%5Bsize%5D=" + String(limit);
    // Incident objects carry a lot of fields we never read (customer_impact_*,
    // time_to_*, user_defined_fields, etc.) — filter down to just what's used.
    JsonDocument filter;
    JsonObject incFilter = filter["data"]["attributes"]["incidents"].add<JsonObject>();
    incFilter["data"]["id"] = true;
    incFilter["data"]["attributes"]["title"] = true;
    incFilter["data"]["attributes"]["severity"] = true;
    incFilter["data"]["attributes"]["state"] = true;
    incFilter["data"]["attributes"]["created"] = true;
    incFilter["data"]["attributes"]["commander"]["data"]["attributes"]["name"] = true;
    incFilter["data"]["attributes"]["fields"]["services"]["value"] = true;
    JsonDocument doc;
    if (!httpGetJsonRetrying("incident_search", url, doc, err, &filter)) return false;

    for (JsonObject wrapper : doc["data"]["attributes"]["incidents"].as<JsonArray>()) {
        JsonObject item = wrapper["data"];
        JsonObject a = item["attributes"];
        Incident inc;
        inc.id        = item["id"]           | "";
        inc.title     = a["title"]           | "";
        inc.severity  = a["severity"]        | "UNKNOWN";
        inc.state     = a["state"]           | "";
        inc.createdAt = a["created"]         | "";
        inc.commander = a["commander"]["data"]["attributes"]["name"] | "";
        JsonArray svcs = a["fields"]["services"]["value"].as<JsonArray>();
        for (JsonVariant s : svcs) inc.services.push_back(s.as<String>());
        out.push_back(inc);
        if ((int)out.size() >= limit) break;
    }
    g_lastIncidents = out;
    bool hasCritical = false;
    for (const Incident& inc : out) {
        if (inc.severity == "SEV-1" || inc.severity == "SEV-2") { hasCritical = true; break; }
    }
    portENTER_CRITICAL(&g_moodMux);
    g_moodHasCritical = hasCritical;
    portEXIT_CRITICAL(&g_moodMux);
    return true;
}

String nextIncidentState(const String& current) {
    String c = current; c.toLowerCase();
    if (c == "active") return "stable";
    if (c == "stable") return "resolved";
    return "active";   // resolved -> active, or any unrecognized state -> active
}

bool setIncidentState(const String& incidentId, const String& newState, String& err) {
    String url = apiBase() + "/api/v2/incidents/" + incidentId;
    String body = String("{\"data\":{\"id\":\"") + incidentId +
                  "\",\"type\":\"incidents\",\"attributes\":{\"fields\":{\"state\":{\"type\":\"dropdown\",\"value\":\"" +
                  newState + "\"}}}}}";
    return httpMutateJson("incident_update", "PATCH", url, body, nullptr, err);
}

static std::vector<SloSummary> g_lastSlos;
const std::vector<SloSummary>& lastSlos() { return g_lastSlos; }

bool fetchSlos(std::vector<SloSummary>& out, String& err, int limit) {
    out.clear();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    // /api/v1/slo/search, not /api/v1/slo — the search endpoint returns each
    // SLO's current status (state/sli) inline, so the list can show a
    // health-state dot without an N+1 fetchSloStatus() call per row. Its
    // `query` param is unified facet search syntax (unverified live for a
    // team-tag match specifically — this org's test SLOs carry no team_tags
    // at all — but it mirrors the same "team:x" convention monitors/incidents
    // already use here, and an empty/non-matching query just returns zero
    // rows rather than erroring).
    String url = apiBase() + "/api/v1/slo/search?page%5Bsize%5D=" + String(limit);
    String scope = monitorTeamScope();
    if (scope.length()) url += "&query=" + urlEncode(scope);
    // Real shape confirmed live: data.attributes.slos[] is an array of
    // {data: {id, attributes: {name, slo_type, timeframe, target_threshold,
    // status: {state, sli}}}} — deeper-nested than the old /api/v1/slo list,
    // but the field *names* (target_threshold, timeframe) carried over
    // unchanged, just relocated one level under "attributes".
    //
    // Unfiltered — a doubly-nested array filter (data.attributes.slos[].
    // data...) is the same fundamental shape (and even more deeply nested)
    // as the one confirmed live to silently match zero items in
    // fetchBitsInvestigations()'s data.attributes.response.investigations[]
    // (HTTP 200, no parse error, just an empty result indistinguishable
    // from "no SLOs configured"). Not independently confirmed broken here,
    // but not worth the risk of the exact same silent-failure mode for a
    // handful of small SLO objects (each ran a few hundred bytes unfiltered
    // in testing, nowhere near the investigation list's several-KB-per-item
    // cost that motivated trimming that one's page size).
    JsonDocument doc;
    if (!httpGetJsonRetrying("slo_search", url, doc, err, nullptr, true)) return false;

    for (JsonObject item : doc["data"]["attributes"]["slos"].as<JsonArray>()) {
        JsonObject s = item["data"];
        JsonObject a = s["attributes"];
        SloSummary slo;
        slo.id        = s["id"]                  | "";
        slo.name      = a["name"]                | "";
        slo.type      = a["slo_type"]            | "";
        slo.target    = a["target_threshold"]    | 0.0;
        slo.timeframe = a["timeframe"]           | "";
        slo.state     = a["status"]["state"]     | "";
        slo.sliValue  = a["status"]["sli"]       | 0.0;
        out.push_back(slo);
        if ((int)out.size() >= limit) break;
    }
    g_lastSlos = out;
    return true;
}

bool fetchSloStatus(const String& sloId, SloStatus& out, String& err) {
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    // v2/status returns a compact current-state summary regardless of the
    // window size — much lighter than v1/history's full point-series
    // (confirmed live: a 5-minute window still returns just {sli, state,
    // error_budget_remaining}, not a history array).
    uint32_t now = (uint32_t)time(nullptr);
    uint32_t from = (now > 3600) ? now - 3600 : 0;
    String url = apiBase() + "/api/v2/slo/" + sloId + "/status?from_ts=" + String(from) + "&to_ts=" + String(now);
    JsonDocument doc;
    if (!httpGetJsonRetrying("slo_status", url, doc, err)) return false;

    JsonObject a = doc["data"]["attributes"];
    out.sliValue             = a["sli"]                     | 0.0;
    out.state                = a["state"]                   | "";
    out.errorBudgetRemaining = a["error_budget_remaining"]   | 0.0;
    // target isn't in this response — caller fills it from fetchSlos()'s SloSummary.
    return true;
}

bool fetchMyTeams(std::vector<Team>& out, String& err) {
    out.clear();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    // filter[me]=true — confirmed live this resolves to the specific user
    // who created the API/App key pair being used, not a per-request/
    // per-session identity (there is none for a device authenticating with
    // static keys).
    String url = apiBase() + "/api/v2/team?filter%5Bme%5D=true&page%5Bsize%5D=20";
    JsonDocument doc;
    if (!httpGetJsonRetrying("team_list", url, doc, err)) return false;

    for (JsonObject item : doc["data"].as<JsonArray>()) {
        Team t;
        t.id     = item["id"]                   | "";
        t.name   = item["attributes"]["name"]   | "";
        t.handle = item["attributes"]["handle"] | "";
        out.push_back(t);
    }
    g_lastMyTeams = out;
    return true;
}

// `included` is small and bounded (a handful of users/escalation steps per
// team) — a linear scan per lookup is simpler than building a hash map for
// something this size.
static JsonObject findIncluded(JsonArray included, const String& id, const char* type) {
    for (JsonObject item : included) {
        String itemId = item["id"] | "";
        String itemType = item["type"] | "";
        if (itemId == id && itemType == type) return item;
    }
    return JsonObject();
}

static String resolveOnCallUserName(JsonArray included, const String& userId) {
    JsonObject u = findIncluded(included, userId, "users");
    if (u.isNull()) return userId;   // fall back to the bare id rather than showing nothing
    String name = u["attributes"]["name"] | "";
    if (name.length()) return name;
    String email = u["attributes"]["email"] | "";
    return email.length() ? email : userId;
}

bool fetchOnCallForTeamId(const String& teamId, std::vector<OnCallEntry>& out, String& err) {
    out.clear();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    // See datadog.h's doc comment — confirmed live against a team with a
    // real active rotation. `include` is required or `included` (where
    // names actually live) never comes back at all.
    String url = apiBase() + "/api/v2/on-call/teams/" + teamId +
                 "/on-call?include=responders,escalations,escalations.responders";
    JsonDocument doc;
    if (!httpGetJsonRetrying("oncall_get", url, doc, err)) return false;

    JsonArray included = doc["included"].as<JsonArray>();

    for (JsonObject ref : doc["data"]["relationships"]["responders"]["data"].as<JsonArray>()) {
        OnCallEntry e;
        String userId = ref["id"] | "";
        e.user = resolveOnCallUserName(included, userId);
        JsonObject u = findIncluded(included, userId, "users");
        e.email = u["attributes"]["email"] | "";
        e.schedule = "Current";
        e.escalationLevel = 0;
        out.push_back(e);
    }

    // "Escalation step N" is only meaningful when there's more than one
    // step to distinguish — confirmed live this wasn't a numbering bug:
    // a real team came back with exactly one step containing 6 responders,
    // so all 6 correctly got labeled step 1 (there IS only a step 1). But
    // repeating "step 1" six times over doesn't tell you anything a plain
    // "Escalation" wouldn't, and reads like a bug even though it isn't one.
    JsonArray escalations = doc["data"]["relationships"]["escalations"]["data"].as<JsonArray>();
    bool multiStep = escalations.size() > 1;
    int stepIdx = 0;
    for (JsonObject stepRef : escalations) {
        stepIdx++;
        String stepId = stepRef["id"] | "";
        JsonObject step = findIncluded(included, stepId, "escalation_policy_steps");
        for (JsonObject userRef : step["relationships"]["responders"]["data"].as<JsonArray>()) {
            OnCallEntry e;
            e.user = resolveOnCallUserName(included, userRef["id"] | "");
            e.schedule = multiStep ? ("Escalation step " + String(stepIdx)) : "Escalation";
            e.escalationLevel = stepIdx;
            out.push_back(e);
        }
    }
    return true;
}

static OnCallResult g_lastOnCallResult;
const OnCallResult& lastOnCallResult() { return g_lastOnCallResult; }

bool fetchOnCallAll() {
    OnCallResult r;
    String teamId = storage::getOnCallTeamId();

    if (teamId.length() == 0) {
        // Not yet resolved — auto-detect via filter[me]. Exactly one team:
        // persist it and proceed with zero user interaction. Zero or
        // multiple: can't guess, cache the list for the web picker instead.
        std::vector<Team> myTeams;
        String terr;
        fetchMyTeams(myTeams, terr);
        if (myTeams.size() == 1) {
            teamId = myTeams[0].id;
            storage::setOnCallTeamId(teamId);
        } else {
            r.hasTeams = !myTeams.empty();
            r.needsTeamPick = myTeams.size() > 1;
            g_lastOnCallResult = r;
            return true;
        }
    }

    r.hasTeams = true;
    String oerr;
    fetchOnCallForTeamId(teamId, r.entries, oerr);
    g_lastOnCallResult = r;
    return true;
}

bool fetchMetricSeries(const String& query, uint32_t fromEpochSec, uint32_t toEpochSec,
                        std::vector<MetricPoint>& out, String& err) {
    out.clear();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    String url = apiBase() + "/api/v1/query?query=" + urlEncode(query) +
                 "&from=" + String(fromEpochSec) + "&to=" + String(toEpochSec);
    JsonDocument doc;
    // buffered=true — this endpoint responds chunked (see
    // httpGetJsonRetrying's doc comment); a point count capped by the
    // window/interval keeps the buffered size bounded and safe.
    if (!httpGetJsonRetrying("metric_query", url, doc, err, nullptr, true)) return false;

    JsonArray series = doc["series"].as<JsonArray>();
    if (series.size() == 0) {
        // A rejected query still comes back HTTP 200 with status:"error" and
        // a human-readable parser message (confirmed live) — surface that
        // instead of a bare "no series" whenever it's present.
        String apiErr = doc["error"] | "";
        err = apiErr.length() ? apiErr : "no series";
        return false;
    }
    for (JsonVariant pt : series[0]["pointlist"].as<JsonArray>()) {
        // Confirmed live: gaps in the data (nothing reported that interval)
        // come back as a real JSON null, not a missing point — as<double>()
        // on a null silently gives 0.0, which would draw a fake dip to zero
        // on the chart instead of just not having a point there. Skip it.
        if (pt[1].isNull()) continue;
        MetricPoint p;
        p.tsSec = (uint32_t)(pt[0].as<double>() / 1000.0);   // pointlist ts is ms
        p.value = pt[1].as<double>();
        out.push_back(p);
    }
    return true;
}

// Monitor types with no metrics query at all — a different query language
// entirely (logs/synthetics results/APM spans/etc.), not just a differently-
// shaped one. Confirmed live for "log alert"/"synthetics alert" in this org
// (the former's logs() query and the latter's literal "no_query" both come
// back as a parser error from /api/v1/query); the rest are documented types
// with the same fundamental mismatch, best-effort since this org doesn't
// have one of each to test against.
// "log alert" is deliberately still in this list — it's not chartable via
// THIS (the /api/v1/query metric) path. It has its own path instead, see
// fetchLogMonitorChartSeries() and its dispatch in fetchMonitorDetailAndChart().
static bool isChartableMonitorType(const String& type) {
    static const char* const NOT_CHARTABLE[] = {
        "log alert", "synthetics alert", "event alert", "event-v2 alert",
        "process alert", "service check", "trace-analytics alert",
        "rum alert", "audit alert", "error-tracking alert", "composite",
        "ci-pipelines alert", "ci-tests alert",
    };
    String t = type; t.toLowerCase();
    for (const char* nc : NOT_CHARTABLE) if (t == nc) return false;
    return true;
}

// Detection-function wrappers (anomaly/outlier/forecast/change) — these
// compare a *deviation band* derived from the wrapped query against the
// monitor's threshold, not the plotted metric value itself, so a threshold
// value drawn as a literal horizontal line on the raw-metric chart would
// misrepresent what it means. Shared by extractChartableQuery() (which
// unwraps these to find the chartable inner query) and isRawMetricQuery()
// (which uses the same list to decide whether thresholds are even
// applicable to draw).
static const char* const DETECTION_WRAPPERS[] = {
    "anomalies(", "outliers(", "forecast(", "raw_forecast(", "change(", "pct_change(",
};

// True when `query` (the monitor's raw, unstripped query) is a plain
// threshold comparison against a metric — i.e. NOT wrapped in one of
// DETECTION_WRAPPERS — so options.thresholds values are literal points on
// the same axis as the chart and safe to draw as reference lines.
static bool isRawMetricQuery(const String& raw) {
    String q = raw;
    q.trim();
    int firstParen = q.indexOf('(');
    int closeColon = q.indexOf("):");
    if (firstParen >= 0 && closeColon > firstParen) {
        q = q.substring(closeColon + 2);
        q.trim();
    }
    for (const char* w : DETECTION_WRAPPERS) {
        if (q.startsWith(w)) return false;
    }
    return true;
}

// Strips a monitor's evaluation envelope down to a plain metrics-query
// expression /api/v1/query will accept — see fetchMonitorChartSeries's doc
// comment in datadog.h for why this is necessary at all.
static String extractChartableQuery(const String& raw) {
    String q = raw;
    q.trim();

    // Drop the leading "<time_aggr>(<window>):" prefix, e.g. "avg(last_5m):".
    int firstParen = q.indexOf('(');
    int closeColon = q.indexOf("):");
    if (firstParen >= 0 && closeColon > firstParen) {
        q = q.substring(closeColon + 2);
        q.trim();
    }

    // Drop the trailing " <comparison> <threshold>" — scan for a depth-0
    // (outside any (), {}) comparison operator; everything from there on is
    // the alert threshold, not part of the query.
    int depth = 0;
    int cmpAt = -1;
    for (int i = 0; i < (int)q.length(); ++i) {
        char c = q[i];
        if (c == '(' || c == '{') depth++;
        else if (c == ')' || c == '}') depth--;
        else if (depth == 0 && (c == '>' || c == '<')) cmpAt = i;
        else if (depth == 0 && c == '=' && i > 0 && (q[i - 1] == '=' || q[i - 1] == '!')) cmpAt = i - 1;
    }
    if (cmpAt > 0) {
        q = q.substring(0, cmpAt);
        q.trim();
    }

    // Detection-function wrapper (anomaly/outlier/forecast detection
    // methods) — the chartable metric query is always its first argument;
    // everything after the first depth-0 comma is algorithm parameters
    // (e.g. 'agile', 2, direction=..., interval=60), not part of the query.
    for (const char* w : DETECTION_WRAPPERS) {
        size_t wlen = strlen(w);
        if (q.length() > wlen && q.startsWith(w)) {
            int d = 0;
            int argEnd = -1;
            for (int i = (int)wlen; i < (int)q.length(); ++i) {
                char c = q[i];
                if (c == '(' || c == '{') d++;
                else if (c == ')' || c == '}') {
                    if (d == 0) { argEnd = i; break; }
                    d--;
                } else if (c == ',' && d == 0) {
                    argEnd = i;
                    break;
                }
            }
            if (argEnd > (int)wlen) {
                q = q.substring(wlen, argEnd);
                q.trim();
            }
            break;
        }
    }

    return q;
}

// Forward declaration — defined further down, but fetchMonitorChartSeries()
// needs it too now (originally only triggerBitsInvestigation() did).
static bool fetchLatestMonitorAlertEvent(long monitorId, String& outEventId, long long& outEventTsMs,
                                          std::vector<String>& outGroups, String& err);

// Merges monitor_groups (e.g. "service:checkout") into the query's first
// "{...}" scope filter, e.g. "avg:foo{env:prod} by {service}" becomes
// "avg:foo{env:prod,service:checkout} by {service}". Confirmed live this is
// necessary, not cosmetic: a "by {tag}" query returns one series per
// distinct tag value (46 of them, for one real monitor tested against) —
// without this, fetchMetricSeries() has no way to know which of those 46 is
// the one that actually alerted, and just took whichever Datadog listed
// first (an arbitrary, usually-wrong series). Leaves the query unchanged if
// it has no "{...}" to merge into.
static String scopeQueryToGroups(const String& query, const std::vector<String>& groups) {
    if (groups.empty()) return query;
    int open = query.indexOf('{');
    int close = (open >= 0) ? query.indexOf('}', open) : -1;
    if (open < 0 || close < 0) return query;

    String groupFilter;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (i) groupFilter += ",";
        groupFilter += groups[i];
    }
    String existing = query.substring(open + 1, close);
    String merged = existing.length() ? (existing + "," + groupFilter) : groupFilter;
    return query.substring(0, open + 1) + merged + query.substring(close);
}

// Anchor the chart window on the monitor's most recent alert event instead
// of "now" — confirmed live this matters: a monitor that's already
// recovered shows a flat, uneventful "last 1h from now" window, while
// anchoring on when it actually fired shows the real spike. Falls back to a
// live "now" window if there's no recent event (never fired, or events
// lookup failed). Shared by both the metric and log chart paths; `outGroups`
// (monitor_groups from the alert event, metric-query-scoping only — logs
// charting ignores it) is populated only on the anchored path.
static void computeChartWindow(long monitorId, uint32_t& from, uint32_t& to, std::vector<String>& outGroups) {
    outGroups.clear();
    String eventId; long long eventTsMs = 0; String eventErr;
    if (fetchLatestMonitorAlertEvent(monitorId, eventId, eventTsMs, outGroups, eventErr) && eventTsMs > 0) {
        to = (uint32_t)(eventTsMs / 1000);
        from = (to > 3600) ? to - 3600 : 0;
        Serial.printf("[dd] chart window: anchored on alert event (id=%s, groups=%d) from=%u to=%u\n",
                      eventId.c_str(), (int)outGroups.size(), from, to);
    } else {
        to = (uint32_t)time(nullptr);
        from = (to > 3600) ? to - 3600 : 0;
        Serial.printf("[dd] chart window: no recent event (%s) — live 'now' window from=%u to=%u\n",
                      eventErr.c_str(), from, to);
    }
}

bool fetchMonitorChartSeries(const Monitor& monitor, std::vector<MetricPoint>& out, String& err) {
    out.clear();
    if (!isChartableMonitorType(monitor.type)) {
        err = monitor.type.length()
                  ? ("\"" + monitor.type + "\" monitors don't use a metrics query")
                  : "this monitor type doesn't use a metrics query";
        return false;
    }

    uint32_t to, from;
    std::vector<String> groups;
    computeChartWindow(monitor.id, from, to, groups);

    String q = extractChartableQuery(monitor.query);
    if (!groups.empty()) {
        q = scopeQueryToGroups(q, groups);
    } else {
        int byAt = q.indexOf(" by {");
        if (byAt >= 0) {
            // No recent alert event to narrow the "by {tag}" grouping to a
            // single series (scopeQueryToGroups needs monitor_groups for
            // that, which only comes from an event) — confirmed live this is
            // a real failure mode, not just a correctness nit: an unscoped
            // "by {tag}" query fans out one series per distinct tag value
            // (45 services, in one real case tested against), and
            // fetchMetricSeries() only ever reads series[0] anyway.
            // Buffering every other series just to discard them produced a
            // truncated read ("HTTP 200 but json: IncompleteInput") on real
            // hardware. Tried wrapping in top(query, 1, ...) first — confirmed
            // live via curl that /api/v1/query returns a flat "Internal
            // error" for top() regardless of query shape, so that's not
            // usable. Dropping the "by {...}" clause entirely instead:
            // Datadog then returns exactly one series (the aggregate across
            // the whole scope) — confirmed live at ~7KB for the same query
            // that was 45 series/~200KB grouped. Less specific than picking
            // the one alerting group, but reliably small and always exactly
            // one series regardless of how many groups exist in the org.
            int closeBrace = q.indexOf('}', byAt);
            if (closeBrace >= 0) q = q.substring(0, byAt) + q.substring(closeBrace + 1);
        }
    }
    Serial.printf("[dd] monitor %ld raw query: %s\n", monitor.id, monitor.query.c_str());
    Serial.printf("[dd] monitor %ld chart query: %s\n", monitor.id, q.c_str());
    return fetchMetricSeries(q, from, to, out, err);
}

// Titles/queries embedded in JSON bodies need escaping — both are free text
// (org-defined) and can contain '"' or '\', which would otherwise break the
// JSON.
static String jsonEscape(const String& s) {
    String out; out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// A log/trace-analytics/rum-alert monitor's `query` is an evaluation
// expression, e.g. logs("service:foo status:error").index("*").rollup(...)
// or trace-analytics("service:foo").rollup(...) or rum("...").rollup(...) —
// the chartable part is always the search string inside <wrapperPrefix>"...".
// Confirmed the trace-analytics/rum wrapper names live against real
// production monitor queries (dd-go's apmTraceAnalyticsMonitorOkAlert /
// dogweb's rum_alert grammar), not guessed. Handles the one escape these
// queries actually use in practice (a literal `"` inside the search string
// is written `\"`); anything more exotic just fails closed (empty result,
// caller reports "no search query found").
static String extractWrappedQuery(const String& raw, const char* wrapperPrefix) {
    int start = raw.indexOf(wrapperPrefix);
    if (start < 0) return "";
    start += strlen(wrapperPrefix);
    String out;
    for (int i = start; i < (int)raw.length(); ++i) {
        char c = raw[i];
        if (c == '\\' && i + 1 < (int)raw.length() && raw[i + 1] == '"') { out += '"'; i++; continue; }
        if (c == '"') break;
        out += c;
    }
    return out;
}

// Datadog's Logs/Spans/RUM Aggregate APIs all return each timeseries point's
// time as an RFC3339/ISO8601 string ("2024-01-01T00:00:00.000Z"), unlike
// /api/v1/query's epoch-ms number — needed now that Monitor Detail's
// tap-to-inspect shows a point's timestamp. Howard Hinnant's days-from-civil
// algorithm (proleptic Gregorian, UTC) rather than mktime()/timegm() — avoids
// any local-timezone ambiguity, and this format is fixed/predictable enough
// that a full RFC3339 parser isn't needed.
static long long parseIso8601ToEpochSec(const String& iso) {
    int y, mo, d, h, mi, s;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    long long yy = y - (mo <= 2 ? 1 : 0);
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    return days * 86400LL + h * 3600LL + mi * 60LL + s;
}

// Shared by fetchLogMonitorChartSeries/fetchTraceMonitorChartSeries/
// fetchRumMonitorChartSeries — log-alert, trace-analytics-alert, and
// rum-alert monitors all have no metrics query at all (isChartableMonitorType()
// correctly excludes all three from the /api/v1/query path), and their three
// equivalent Aggregate APIs share an identical request shape ({filter:
// {query,from,to}, compute:[{aggregation:"count",type:"timeseries",
// interval:"1m"}]}) and an identical {time,value} timeseries-point shape —
// confirmed against datadog-api-client-go's model_{logs,spans,rum}_*.go.
// The one structural difference: Spans buckets nest computes one level
// deeper, under "attributes" (SpansAggregateBucket.Attributes.Computes),
// while Logs/RUM buckets are flat (LogsAggregateBucket.Computes /
// RUMBucketResponse.Computes) — hence nestedUnderAttributes.
static bool fetchNonMetricChartSeries(const Monitor& monitor, const char* wrapperPrefix,
                                       const char* urlPath, bool nestedUnderAttributes,
                                       const char* kindNameForError,
                                       std::vector<MetricPoint>& out, String& err) {
    out.clear();
    // Prefer the newer formula(...)+options.variables[] style's search
    // query (fetchMonitorDetail() only populates this when present) over
    // the legacy embedded-string extraction — confirmed live a real
    // log-alert monitor uses the former exclusively, with nothing for
    // extractWrappedQuery() to find in its raw `query` at all.
    String q = monitor.variableSearchQuery.length()
        ? monitor.variableSearchQuery
        : extractWrappedQuery(monitor.query, wrapperPrefix);
    if (q.length() == 0) { err = String("no search query found in this monitor's ") + kindNameForError + " query"; return false; }

    uint32_t to, from;
    std::vector<String> unusedGroups;   // monitor_groups is metric-query scoping only; these charting paths ignore it
    computeChartWindow(monitor.id, from, to, unusedGroups);

    String url = apiBase() + urlPath;
    String body = String("{\"filter\":{\"query\":\"") + jsonEscape(q) +
                  "\",\"from\":\"" + String((long long)from * 1000) + "\",\"to\":\"" + String((long long)to * 1000) +
                  "\"},\"compute\":[{\"aggregation\":\"count\",\"type\":\"timeseries\",\"interval\":\"1m\"}]}";
    // kindNameForError is already the low-cardinality "log"/"trace-analytics"/
    // "rum" set (one shared function, three known callers) — reused as-is
    // for the endpoint tag instead of introducing a parallel enum.
    String endpointTag = String("chart_") + kindNameForError;
    JsonDocument doc;
    if (!httpMutateJson(endpointTag.c_str(), "POST", url, body, &doc, err)) return false;

    JsonArray buckets = doc["data"]["buckets"].as<JsonArray>();
    if (buckets.size() == 0) { err = "no buckets returned"; return false; }
    JsonArray points = nestedUnderAttributes
        ? buckets[0]["attributes"]["computes"]["c0"].as<JsonArray>()
        : buckets[0]["computes"]["c0"].as<JsonArray>();
    for (JsonVariant pt : points) {
        MetricPoint p;
        p.tsSec = (uint32_t)parseIso8601ToEpochSec(pt["time"] | "");
        p.value = pt["value"] | 0.0;
        out.push_back(p);
    }
    if (out.empty()) { err = "no data points in aggregate response"; return false; }
    return true;
}

bool fetchLogMonitorChartSeries(const Monitor& monitor, std::vector<MetricPoint>& out, String& err) {
    return fetchNonMetricChartSeries(monitor, "logs(\"", "/api/v2/logs/analytics/aggregate", false, "log", out, err);
}

bool fetchTraceMonitorChartSeries(const Monitor& monitor, std::vector<MetricPoint>& out, String& err) {
    return fetchNonMetricChartSeries(monitor, "trace-analytics(\"", "/api/v2/spans/analytics/aggregate", true, "trace-analytics", out, err);
}

bool fetchRumMonitorChartSeries(const Monitor& monitor, std::vector<MetricPoint>& out, String& err) {
    return fetchNonMetricChartSeries(monitor, "rum(\"", "/api/v2/rum/analytics/aggregate", false, "rum", out, err);
}

static MonitorDetailResult g_lastMonitorDetailResult;
const MonitorDetailResult& lastMonitorDetailResult() { return g_lastMonitorDetailResult; }

bool fetchMonitorDetailAndChart(long monitorId) {
    MonitorDetailResult r;
    Monitor detail;
    r.detailOk = fetchMonitorDetail(monitorId, detail, r.err);
    if (r.detailOk) {
        r.id = detail.id;
        r.status = detail.status;
        r.muted = detail.muted;
        r.criticalThreshold = detail.criticalThreshold;
        r.warningThreshold = detail.warningThreshold;
        r.thresholdsApplicable = isRawMetricQuery(detail.query);
        String t = detail.type; t.toLowerCase();
        if (t == "log alert")             r.chartOk = fetchLogMonitorChartSeries(detail, r.chart, r.err);
        else if (t == "trace-analytics alert") r.chartOk = fetchTraceMonitorChartSeries(detail, r.chart, r.err);
        else if (t == "rum alert")        r.chartOk = fetchRumMonitorChartSeries(detail, r.chart, r.err);
        else                              r.chartOk = fetchMonitorChartSeries(detail, r.chart, r.err);
    }
    g_lastMonitorDetailResult = r;
    return r.detailOk;
}

bool fetchCaseProjects(std::vector<CaseProject>& out, String& err) {
    out.clear();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    String url = apiBase() + "/api/v2/cases/projects";
    JsonDocument filter;
    JsonObject pf = filter["data"].add<JsonObject>();
    pf["id"] = true;
    pf["attributes"]["key"] = true;
    pf["attributes"]["name"] = true;
    JsonDocument doc;
    if (!httpGetJsonRetrying("case_projects", url, doc, err, &filter)) return false;

    for (JsonObject p : doc["data"].as<JsonArray>()) {
        CaseProject cp;
        cp.id   = p["id"]                | "";
        cp.key  = p["attributes"]["key"]  | "";
        cp.name = p["attributes"]["name"] | "";
        out.push_back(cp);
    }
    return true;
}

// Confirmed live via GET /api/v2/cases/types — this ID's all-zeros-but-the-
// last-digit shape looks like a Datadog-wide constant rather than something
// generated per org, but that's an inference, not something the docs state
// outright.
static const char* const CASE_TYPE_STANDARD = "00000000-0000-0000-0000-000000000001";

bool createCase(const String& title, const String& projectId, String& outCaseKey, String& err) {
    String url = apiBase() + "/api/v2/cases";
    String body = String("{\"data\":{\"type\":\"case\",\"attributes\":{\"title\":\"") +
                  jsonEscape(title) + "\",\"priority\":\"P3\",\"type_id\":\"" + CASE_TYPE_STANDARD +
                  "\"},\"relationships\":{\"project\":{\"data\":{\"type\":\"project\",\"id\":\"" +
                  projectId + "\"}}}}}";
    JsonDocument doc;
    if (!httpMutateJson("case_create", "POST", url, body, &doc, err)) return false;
    outCaseKey = doc["data"]["attributes"]["key"] | "";
    return true;
}

bool createIncident(const String& title, String& outIncidentId, String& err) {
    String url = apiBase() + "/api/v2/incidents";
    String body = String("{\"data\":{\"type\":\"incidents\",\"attributes\":{\"title\":\"") +
                  jsonEscape(title) + "\"}}}";
    JsonDocument doc;
    if (!httpMutateJson("incident_create", "POST", url, body, &doc, err)) return false;
    outIncidentId = doc["data"]["id"] | "";
    return true;
}

// Finds the monitor's most recent alert-transition event — confirmed live
// that "@monitor.id:<id>" (not "monitor_id:<id>", which silently matched
// nothing) is the real facet syntax for this endpoint, and that the event's
// evt.id / attributes.timestamp fields are exactly the event_id/event_ts
// Bits AI's trigger endpoint wants (attributes.timestamp is already epoch
// *milliseconds*, matching the trigger endpoint's documented unit — don't
// divide by 1000, that would be seconds and silently wrong).
// outGroups is the monitor_groups this specific alert fired for (e.g.
// "service:checkout") — confirmed live this matters: a "by {tag}" monitor's
// query returns one series per distinct tag value (dozens, for a busy org),
// and without this there's no way to tell which one actually alerted.
static bool fetchLatestMonitorAlertEvent(long monitorId, String& outEventId, long long& outEventTsMs,
                                          std::vector<String>& outGroups, String& err) {
    outGroups.clear();
    String query = "source:alert @monitor.id:" + String(monitorId);
    String url = apiBase() + "/api/v2/events/search";
    String body = String("{\"filter\":{\"query\":\"") + query +
                  "\",\"from\":\"now-7d\",\"to\":\"now\"},\"sort\":\"-timestamp\",\"page\":{\"limit\":1}}";
    // The full event body is a few KB (message text, tags, monitor options,
    // etc.) — filter down to just the three fields actually read below.
    JsonDocument filter;
    JsonObject itemFilter = filter["data"].add<JsonObject>();
    itemFilter["attributes"]["attributes"]["evt"]["id"] = true;
    itemFilter["attributes"]["attributes"]["timestamp"] = true;
    itemFilter["attributes"]["attributes"]["monitor_groups"] = true;
    JsonDocument doc;
    if (!httpMutateJson("events_search", "POST", url, body, &doc, err, &filter)) return false;

    JsonArray data = doc["data"].as<JsonArray>();
    if (data.size() == 0) { err = "no recent alert event found for this monitor"; return false; }
    JsonObject a = data[0]["attributes"]["attributes"];
    outEventId = a["evt"]["id"] | "";
    outEventTsMs = a["timestamp"] | 0LL;
    for (JsonVariant g : a["monitor_groups"].as<JsonArray>()) outGroups.push_back(g.as<String>());
    if (outEventId.length() == 0 || outEventTsMs == 0) { err = "event missing id/timestamp"; return false; }
    return true;
}

bool triggerBitsInvestigation(long monitorId, String& outInvestigationId, String& err) {
    String eventId;
    long long eventTsMs = 0;
    std::vector<String> groups;   // unused here — Bits only needs id/ts, not the group
    if (!fetchLatestMonitorAlertEvent(monitorId, eventId, eventTsMs, groups, err)) return false;

    String url = apiBase() + "/api/v2/bits-ai/investigations";
    String body = String("{\"data\":{\"type\":\"trigger_investigation_request\",\"attributes\":{\"trigger\":{"
                  "\"type\":\"monitor_alert_trigger\",\"monitor_alert_trigger\":{\"monitor_id\":") +
                  String(monitorId) + ",\"event_id\":\"" + eventId + "\",\"event_ts\":" +
                  String(eventTsMs) + "}}}}}";
    JsonDocument doc;
    if (!httpMutateJson("bits_trigger", "POST", url, body, &doc, err)) return false;
    outInvestigationId = doc["data"]["attributes"]["investigation_id"] | "";
    return true;
}

static std::vector<BitsInvestigation> g_lastBitsInvestigations;
const std::vector<BitsInvestigation>& lastBitsInvestigations() { return g_lastBitsInvestigations; }

// Real response shape confirmed live: data.attributes.response.investigations[]
// — each item has uuid/title/status/entity.source/modified_timestamp at its
// top level (unlike the documented v2 list endpoint's JSON:API
// id/attributes.* wrapping).
//
// Deliberately unfiltered — an ArduinoJson::DeserializationOption::Filter
// nested this deeply (data.attributes.response.investigations[].*) was
// confirmed live to silently match zero items despite the real data being
// present (HTTP 200, no parse error, just an empty result — indistinguishable
// from "genuinely no investigations" without a raw unfiltered comparison,
// which is exactly what caught this). Not fully root-caused against
// ArduinoJson's filter semantics; parsing unfiltered sidesteps it rather
// than chase the exact mechanism further.
//
// Single call, server-side team-scoped (same "team:x" convention as
// Monitors/Incidents/SLOs) — NOT the unscoped-plus-client-side-sort design
// tried previously. That approach traded away real team filtering for no
// good reason, and made the buffered-fetch overflow below worse in the
// process: an unscoped fetch always has plenty of org-wide activity to fill
// page_size with full-size items, whereas a team-scoped one naturally
// returns fewer, smaller results (confirmed live: 3 investigations for a
// team that has some, vs. always-8-full-items when unscoped) — so scoping
// isn't just correctness, it's also what keeps the response small enough
// for the buffered String fetch below to actually hold.
//
// page_size 8: confirmed live that 20 unscoped overflows
// httpGetJsonRetrying's buffered String path outright ("short write...
// failed" -> IncompleteInput on both retry attempts, not a network error),
// consistent with each item running several KB unfiltered (narrative/
// hypotheses/timings/entity details this screen never reads). 8 matches
// what's worked live team-scoped; an unscoped fetch (blank team) can still
// hit the same overflow at this size if the org is very active — reduce
// further if that turns out to be a real problem, rather than raise it.
bool fetchBitsInvestigations(std::vector<BitsInvestigation>& out, String& err) {
    out.clear();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    String url = apiBase() + "/api/unstable/bits-ai/investigation/search?page_size=8";
    String scope = monitorTeamScope();
    if (scope.length()) url += "&query=" + urlEncode(scope);

    JsonDocument doc;
    if (!httpGetJsonRetrying("bits_search", url, doc, err, nullptr, true)) return false;

    for (JsonObject item : doc["data"]["attributes"]["response"]["investigations"].as<JsonArray>()) {
        BitsInvestigation inv;
        inv.id           = item["uuid"]               | "";
        inv.title        = item["title"]              | "";
        inv.status       = item["status"]             | "";
        inv.entitySource = item["entity"]["source"]   | "";
        inv.modifiedTs   = (uint32_t)parseIso8601ToEpochSec(item["modified_timestamp"] | "");
        out.push_back(inv);
        if ((int)out.size() >= 14) break;   // same LVGL-pool-protecting cap as other lists
    }
    g_lastBitsInvestigations = out;
    return true;
}

bool fetchBitsInvestigationDetail(const String& investigationId, BitsInvestigationDetail& out, String& err) {
    out = BitsInvestigationDetail();
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    String url = apiBase() + "/api/v2/bits-ai/investigations/" + investigationId;
    // Unfiltered — a nested array filter (conclusions[].*) at this depth
    // was confirmed live to silently match zero items in
    // fetchBitsInvestigations()'s data.attributes.response.investigations[]
    // (HTTP 200, no parse error, just an empty result). Not confirmed this
    // exact one also breaks, but it's the same fundamental shape one level
    // shallower, and getting this wrong reads identically to "no
    // conclusions yet" — not worth the risk to save parsing conclusions[]
    // .description (a multi-KB markdown wall of text per conclusion,
    // confirmed live), which is a transient cost inside `doc` for the
    // single call this function makes, not something out.conclusionSummary
    // ever retains.
    JsonDocument doc;
    if (!httpGetJsonRetrying("bits_get", url, doc, err, nullptr, true)) return false;

    JsonObject a = doc["data"]["attributes"];
    out.title  = a["title"]  | "";
    out.status = a["status"] | "";
    JsonArray conclusions = a["conclusions"].as<JsonArray>();
    if (conclusions.size() > 0) {
        out.conclusionTitle   = conclusions[0]["title"]   | "";
        out.conclusionSummary = conclusions[0]["summary"] | "";
    }
    out.detailOk = true;
    return true;
}

// esp_reset_reason() is stable for the whole runtime (read from an RTC/
// hardware register at boot, not something later code can clear) — safe to
// call from netTask() (core 0) any time, including well after boot.
static const char* resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}
static bool resetReasonIsAbnormal(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT   || r == ESP_RST_BROWNOUT;
}

bool submitDeviceMetrics(TaskHandle_t loopTaskHandle, float loopBusyPct, float netBusyPct, String& err) {
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    // Same NTP-readiness guard used elsewhere in this file for anything
    // that needs a real wall-clock timestamp (see the clock-tick/mute-until
    // logic) — a garbage pre-NTP-sync timestamp this far in the past is the
    // kind of thing metrics ingestion silently drops rather than errors on.
    long ts = (long)time(nullptr);
    if (ts < 1700000000) { err = "NTP not ready"; return false; }

    // Static hardware facts as tags, not repeated gauges — these don't
    // change while the device is running, so they're dimensions to filter/
    // group by (e.g. "compare RSSI across chip revisions"), same convention
    // Datadog's own Agent uses for host tags.
    String hwTags = ",\"chip_model:" + String(ESP.getChipModel()) +
                    "\",\"chip_revision:" + String(ESP.getChipRevision()) +
                    "\",\"cpu_freq_mhz:" + String(ESP.getCpuFreqMHz()) +
                    "\",\"flash_size_mb:" + String(ESP.getFlashChipSize() / (1024 * 1024)) +
                    "\",\"sdk_version:" + String(ESP.getSdkVersion()) +
                    "\",\"boot_reason:" + String(resetReasonStr(esp_reset_reason())) + "\"";
    String baseTags = "[\"service:barkboard\",\"device:" + netcfg::apSsid() + "\",\"firmware_version:" + String(BARKBOARD_VERSION) + "\"" + hwTags + "]";

    uint32_t heapSize = ESP.getHeapSize();
    uint32_t heapFree  = ESP.getFreeHeap();
    double heapUsedPct = heapSize ? (double)(heapSize - heapFree) / heapSize * 100.0 : 0;

    // type 3 = gauge (Datadog Metrics API's type enum: 0 unspecified,
    // 1 count, 2 rate, 3 gauge) — every value here is a point-in-time
    // reading, not something to sum/rate across the submission interval.
    struct Gauge { const char* metric; double value; };
    Gauge gauges[] = {
        { "barkboard.heap.free",      (double)heapFree },
        { "barkboard.heap.min_free",  (double)ESP.getMinFreeHeap() },   // low-water mark since boot — the one that actually catches a slow leak
        { "barkboard.heap.max_alloc", (double)ESP.getMaxAllocHeap() },  // largest contiguous block — fragmentation signal, not just "heap low"
        { "barkboard.heap.size",      (double)heapSize },
        { "barkboard.heap.used_pct",  heapUsedPct },
        { "barkboard.wifi.rssi",      (double)WiFi.RSSI() },
        { "barkboard.uptime",         (double)(millis() / 1000) },
    };

    String body = "{\"series\":[";
    for (size_t i = 0; i < sizeof(gauges) / sizeof(gauges[0]); ++i) {
        if (i) body += ",";
        body += "{\"metric\":\"" + String(gauges[i].metric) + "\",\"type\":3,\"points\":[{\"timestamp\":" +
                String(ts) + ",\"value\":" + String(gauges[i].value, 2) + "}],\"tags\":" + baseTags + "}";
    }

    // Per-task stack headroom — uxTaskGetStackHighWaterMark() is a plain
    // FreeRTOS kernel query with its own internal locking, safe to call
    // cross-core for another task's handle; it's not an LVGL call, so it
    // doesn't cross the core-0/core-1 LVGL boundary this file's other
    // comments warn about. dd-net's own handle comes from
    // xTaskGetCurrentTaskHandle() since this function always runs on it;
    // loopTaskHandle is passed in because it was captured once in setup()
    // (core 1) — a task can't look up another task's handle by name.
    //
    // busy_pct alongside it is NOT real CPU utilization — confirmed by
    // reading this project's pinned framework's prebuilt sdkconfig that
    // configGENERATE_RUN_TIME_STATS/configUSE_TRACE_FACILITY are both off,
    // so the FreeRTOS APIs that report real per-task CPU% aren't available
    // at all (see this file's header doc comment on submitDeviceMetrics()).
    // Instead main.cpp's loop()/netTask() each time how much of their own
    // cycle is spent working versus asleep in delay(5)/vTaskDelay(5), and
    // pass the running average in here — a coarse but honest "is this
    // task's own work starting to crowd out its sleep" signal, cheap
    // enough (a couple of millis() reads per iteration) to leave on
    // unconditionally rather than gating behind anything extra.
    // loopBusyPct/netBusyPct are -1 when no full cycle has completed yet
    // since the last report (e.g. right after boot) — skipped rather than
    // sending a meaningless value.
    struct TaskStack { const char* name; TaskHandle_t handle; float busyPct; };
    TaskStack stacks[] = {
        { "loop",   loopTaskHandle,             loopBusyPct },
        { "dd-net", xTaskGetCurrentTaskHandle(), netBusyPct },
    };
    for (size_t i = 0; i < sizeof(stacks) / sizeof(stacks[0]); ++i) {
        String taskTags = "[\"service:barkboard\",\"device:" + netcfg::apSsid() + "\",\"firmware_version:" + String(BARKBOARD_VERSION) +
                           "\",\"task:" + String(stacks[i].name) + "\"]";
        if (stacks[i].handle) {
            uint32_t freeWords = uxTaskGetStackHighWaterMark(stacks[i].handle);
            body += ",{\"metric\":\"barkboard.task.stack_free\",\"type\":3,\"points\":[{\"timestamp\":" +
                    String(ts) + ",\"value\":" + String((double)freeWords, 2) + "}],\"tags\":" + taskTags + "}";
        }
        if (stacks[i].busyPct >= 0) {
            body += ",{\"metric\":\"barkboard.task.busy_pct\",\"type\":3,\"points\":[{\"timestamp\":" +
                    String(ts) + ",\"value\":" + String((double)stacks[i].busyPct, 2) + "}],\"tags\":" + taskTags + "}";
        }
    }

    // type 1 = count — a sum-since-last-report of real HTTP requests this
    // device made, not a point-in-time reading like the gauges above.
    // Drained (not just read) here: each report should reflect calls made
    // since the previous one, not a running total that never resets — and
    // this function only ever runs on netTask (core 0), same as every
    // recordApiCall() site, so no mutex is needed around the swap.
    std::map<String, uint32_t> callCounts;
    callCounts.swap(g_apiCallCounts);
    for (const auto& kv : callCounts) {
        String endpointTags = "[\"service:barkboard\",\"device:" + netcfg::apSsid() + "\",\"firmware_version:" + String(BARKBOARD_VERSION) +
                               "\",\"endpoint:" + kv.first + "\"]";
        body += ",{\"metric\":\"barkboard.api.calls\",\"type\":1,\"points\":[{\"timestamp\":" +
                String(ts) + ",\"value\":" + String((double)kv.second, 2) + "}],\"tags\":" + endpointTags + "}";
    }
    body += "]}";

    String url = apiBase() + "/api/v2/series";
    return httpMutateJson("metrics_submit", "POST", url, body, nullptr, err);
}

bool reportBootEvent(String& err) {
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    esp_reset_reason_t reason = esp_reset_reason();
    bool abnormal = resetReasonIsAbnormal(reason);
    String reasonStr = String(resetReasonStr(reason));

    // Every boot now files an Event when Events are enabled — a plain
    // reset-button press (ESP_RST_EXT) or power cycle used to be totally
    // silent, which read as "did Events even work" the moment someone
    // actually wanted to see one land. alert_type still separates the two
    // cases (error vs info) so an abnormal reboot still stands out — in
    // Datadog's Events explorer, in a monitor's severity, and visually
    // (red vs blue) — from routine ones instead of blending in.
    String title = abnormal
        ? ("BarkBoard rebooted abnormally (" + reasonStr + ")")
        : ("BarkBoard booted (" + reasonStr + ")");
    String text = abnormal
        ? ("Device " + netcfg::apSsid() + " restarted after a " + reasonStr +
           " reset — see barkboard.task.stack_free and barkboard.heap.* around this time for a possible cause.")
        : ("Device " + netcfg::apSsid() + " booted (" + reasonStr + ").");
    String body = "{\"title\":\"" + title + "\","
                  "\"text\":\"" + text + "\","
                  "\"alert_type\":\"" + String(abnormal ? "error" : "info") + "\","
                  "\"tags\":[\"service:barkboard\",\"device:" + netcfg::apSsid() + "\",\"firmware_version:" + String(BARKBOARD_VERSION) +
                  "\",\"boot_reason:" + reasonStr + "\"]}";

    String url = apiBase() + "/api/v1/events";
    return httpMutateJson("event_submit", "POST", url, body, nullptr, err);
}

// Keep in sync by hand with docs/datadog/metrics.json (tools/push_metric_metadata.py's
// data file for pushing this same metadata against an *existing* org from a
// dev machine) — this is the on-device equivalent, so a brand-new install
// gets labeled metrics automatically instead of only working for whoever
// happens to notice the website's download link and run that script. Every
// unit id here was confirmed live against Datadog's real unit list before
// being committed (see push_metric_metadata.py's doc comment) — a typo
// fails loudly (HTTP 404 "unit not found") rather than silently applying
// nothing.
struct MetricMeta { const char* metric; const char* shortName; const char* unit; const char* description; };
static const MetricMeta METRIC_METADATA[] = {
    { "barkboard.heap.free", "free heap", "byte",
      "Free heap memory on the device right now." },
    { "barkboard.heap.min_free", "min free heap", "byte",
      "Lowest free-heap reading observed since boot — the low-water mark that actually catches a slow leak, unlike the point-in-time free reading." },
    { "barkboard.heap.max_alloc", "max allocatable block", "byte",
      "Largest contiguous free heap block — a fragmentation signal distinct from total free heap; can be low even when free heap looks healthy." },
    { "barkboard.heap.size", "heap size", "byte",
      "Total heap size available to the device." },
    { "barkboard.heap.used_pct", "heap used", "percent",
      "Percentage of heap currently in use." },
    { "barkboard.wifi.rssi", "WiFi RSSI", "decibel-milliwatt",
      "WiFi signal strength (RSSI) of the device's connection to its access point." },
    { "barkboard.uptime", "uptime", "second",
      "Seconds since the device last booted." },
    { "barkboard.task.stack_free", "task stack free", "byte",
      "Minimum free stack space observed for a FreeRTOS task since boot (tagged task:loop/task:dd-net) — a high-water mark, not the current value." },
    { "barkboard.task.busy_pct", "task busy", "percent",
      "Share of a task's own loop cycle spent working versus asleep, averaged since the last report. A coarse proxy, not true CPU utilization — real per-task CPU% isn't available on this hardware/framework combination." },
    { "barkboard.api.calls", "API calls", "request",
      "Number of HTTP requests the device made to the Datadog API since the last report, tagged by endpoint. A retry counts as a separate call." },
};

// One-shot per firmware version, same reasoning as reportBootEvent()'s
// per-boot guard: metadata almost never changes, so re-pushing it every
// report (or every boot) would just be 10 extra HTTP calls for nothing.
// Gated on BARKBOARD_VERSION rather than a plain bool so a future release
// that tweaks a description/unit re-pushes once instead of staying stale
// forever. Only marks storage::setMetricMetadataVersion() once every single
// entry succeeds — a partial failure (one flaky call) leaves it unmarked so
// the next metrics-report interval just retries the whole set; PUT is
// idempotent, so redoing already-succeeded entries is harmless.
bool pushMetricMetadataIfNeeded(String& err) {
    if (storage::getMetricMetadataVersion() == BARKBOARD_VERSION) return true;
    if (WiFi.status() != WL_CONNECTED) { err = "no WiFi"; return false; }

    bool allOk = true;
    for (const MetricMeta& m : METRIC_METADATA) {
        String url = apiBase() + "/api/v1/metrics/" + m.metric;
        String body = String("{\"short_name\":\"") + jsonEscape(m.shortName) +
                      "\",\"unit\":\"" + jsonEscape(m.unit) +
                      "\",\"description\":\"" + jsonEscape(m.description) + "\"}";
        String oneErr;
        if (!httpMutateJson("metric_metadata", "PUT", url, body, nullptr, oneErr)) {
            allOk = false;
            err = oneErr;   // last failure wins — enough to see something went wrong in Serial
        }
    }
    if (allOk) storage::setMetricMetadataVersion(BARKBOARD_VERSION);
    return allOk;
}

}
