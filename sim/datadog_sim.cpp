// Simulator-only stand-in for src/datadog.cpp. Implements the exact public
// dd:: API surface declared in src/datadog.h with hardcoded/canned data
// instead of any real network I/O — the real datadog.cpp can't link into a
// desktop build at all (WiFiClientSecure/HTTPClient/WiFi.h are ESP32-Arduino
// only). This file is never built into the ESP32 firmware; sim/Makefile
// compiles it in place of src/datadog.cpp for the simulator target only.
#include "datadog.h"
#include <cmath>

using namespace dd;

namespace {

MonitorCounts g_counts;
std::vector<Monitor> g_monitors;
std::vector<Incident> g_incidents;
std::vector<SloSummary> g_slos;
String g_monitorFilter = "";

void seedIfNeeded() {
    if (!g_monitors.empty()) return;

    g_counts.alert = 2;
    g_counts.warn = 1;
    g_counts.ok = 11;
    g_counts.noData = 1;
    g_counts.fetchOk = true;
    g_counts.error = "";
    g_counts.lastFetchMs = 0;

    Monitor m1; m1.id = 1001; m1.name = "checkout-service p99 latency"; m1.status = "Alert";
    m1.query = "avg(last_5m):avg:trace.http.request.duration{env:prod,service:checkout-service} > 0.8";
    m1.type = "metric alert"; m1.lastTriggeredTs = 1750000000; m1.tags = {"team:checkout", "env:prod"};
    m1.muted = false;

    Monitor m2; m2.id = 1002; m2.name = "orders-db replication lag"; m2.status = "Alert";
    m2.query = "avg(last_5m):avg:postgresql.replication_delay_bytes{service:orders-db} > 5000000";
    m2.type = "metric alert"; m2.lastTriggeredTs = 1750001200; m2.tags = {"team:data-platform"};
    m2.muted = false;

    Monitor m3; m3.id = 1003; m3.name = "auth-api 5xx rate"; m3.status = "Warn";
    m3.query = "avg(last_10m):sum:trace.http.request.errors{service:auth-api}.as_count() > 10";
    m3.type = "metric alert"; m3.lastTriggeredTs = 1749998800; m3.tags = {"team:identity"};
    m3.muted = false;

    Monitor m4; m4.id = 1004; m4.name = "api-gateway CPU utilization"; m4.status = "OK";
    m4.query = "avg(last_15m):avg:system.cpu.user{service:api-gateway} > 85"; m4.type = "metric alert";
    m4.lastTriggeredTs = 1749990000; m4.tags = {"team:platform"};

    Monitor m5; m5.id = 1005; m5.name = "search-index freshness"; m5.status = "No Data";
    m5.query = "avg(last_30m):avg:search.index.lag_seconds{*} > 300"; m5.type = "metric alert";
    m5.lastTriggeredTs = 0; m5.tags = {"team:search"};

    Monitor m6; m6.id = 1006; m6.name = "web-frontend synthetic check"; m6.status = "OK";
    m6.query = "\"synthetics\" test failure"; m6.type = "synthetics alert";
    m6.lastTriggeredTs = 1749980000; m6.tags = {"team:web"};

    g_monitors = {m1, m2, m3, m4, m5, m6};

    Incident i1; i1.id = "9001"; i1.title = "Checkout errors spiking in us-east-1";
    i1.severity = "SEV-2"; i1.state = "active"; i1.createdAt = "2026-08-02T14:03:00Z";
    i1.commander = "jane.doe"; i1.services = {"checkout-service", "payments-api"};

    Incident i2; i2.id = "9002"; i2.title = "Elevated replication lag on orders-db";
    i2.severity = "SEV-3"; i2.state = "stable"; i2.createdAt = "2026-08-01T09:22:00Z";
    i2.commander = ""; i2.services = {"orders-db"};

    g_incidents = {i1, i2};

    SloSummary s1; s1.id = "slo-001"; s1.name = "Checkout availability"; s1.type = "metric";
    s1.target = 99.9; s1.timeframe = "30d";
    SloSummary s2; s2.id = "slo-002"; s2.name = "Auth API latency"; s2.type = "monitor";
    s2.target = 99.5; s2.timeframe = "7d";
    SloSummary s3; s3.id = "slo-003"; s3.name = "Search index freshness"; s3.type = "time_slice";
    s3.target = 99.0; s3.timeframe = "30d";
    g_slos = {s1, s2, s3};
}

} // namespace

bool dd::validateKeysAndDetectSite(const String&, const String&, String& outSite, String& err, ProgressCb) {
    outSite = "datadoghq.com";
    err = "";
    return true;
}

bool dd::isConfigured() { return true; }

bool dd::fetchMonitorCounts(MonitorCounts& out) {
    seedIfNeeded();
    out = g_counts;
    return true;
}
const MonitorCounts& dd::lastMonitorCounts() { seedIfNeeded(); return g_counts; }

bool dd::fetchMonitors(const String& statusFilter, std::vector<Monitor>& out, String& err, int limit) {
    seedIfNeeded();
    out.clear();
    for (const auto& m : g_monitors) {
        bool match = statusFilter.length() == 0
            ? (m.status != "OK")
            : m.status.equalsIgnoreCase(statusFilter);
        if (match) out.push_back(m);
        if ((int)out.size() >= limit) break;
    }
    err = "";
    return true;
}
const std::vector<Monitor>& dd::lastMonitors() { seedIfNeeded(); return g_monitors; }
void dd::setMonitorFilter(const String& f) { g_monitorFilter = f; }
const String& dd::getMonitorFilter() { return g_monitorFilter; }

bool dd::fetchMonitorDetail(long monitorId, Monitor& out, String& err) {
    seedIfNeeded();
    for (const auto& m : g_monitors) {
        if (m.id == monitorId) { out = m; err = ""; return true; }
    }
    err = "monitor not found (sim)";
    return false;
}

bool dd::muteMonitor(long, uint32_t, String& err) { err = ""; return true; }
bool dd::unmuteMonitor(long, String& err) { err = ""; return true; }

bool dd::fetchIncidents(std::vector<Incident>& out, String& err, int limit) {
    seedIfNeeded();
    out = g_incidents;
    if ((int)out.size() > limit) out.resize(limit);
    err = "";
    return true;
}
const std::vector<Incident>& dd::lastIncidents() { seedIfNeeded(); return g_incidents; }

String dd::nextIncidentState(const String& current) {
    if (current.equalsIgnoreCase("active")) return "stable";
    if (current.equalsIgnoreCase("stable")) return "resolved";
    return "active";
}
bool dd::setIncidentState(const String& incidentId, const String& newState, String& err) {
    for (auto& i : g_incidents) {
        if (i.id == incidentId) { i.state = newState; err = ""; return true; }
    }
    err = "incident not found (sim)";
    return false;
}

bool dd::fetchSlos(std::vector<SloSummary>& out, String& err, int limit) {
    seedIfNeeded();
    out = g_slos;
    if ((int)out.size() > limit) out.resize(limit);
    err = "";
    return true;
}
const std::vector<SloSummary>& dd::lastSlos() { seedIfNeeded(); return g_slos; }

bool dd::fetchSloStatus(const String& sloId, SloStatus& out, String& err) {
    seedIfNeeded();
    out.sliValue = 99.94;
    out.target = 99.9;
    out.errorBudgetRemaining = 61.0;
    out.state = "ok";
    for (const auto& s : g_slos) if (s.id == sloId) out.target = s.target;
    err = "";
    return true;
}

bool dd::fetchTeams(std::vector<Team>& out, String& err, int limit) {
    Team t; t.id = "team-1"; t.name = "checkout";
    out = {t};
    if ((int)out.size() > limit) out.resize(limit);
    err = "";
    return true;
}

bool dd::fetchOnCallForTeam(const String&, std::vector<OnCallEntry>& out, String& err) {
    OnCallEntry e1; e1.user = "jane.doe"; e1.schedule = "checkout-primary"; e1.escalationLevel = 1;
    OnCallEntry e2; e2.user = "sam.lee"; e2.schedule = "checkout-secondary"; e2.escalationLevel = 2;
    out = {e1, e2};
    err = "";
    return true;
}

bool dd::fetchMetricSeries(const String&, uint32_t fromEpochSec, uint32_t toEpochSec,
                            std::vector<MetricPoint>& out, String& err) {
    out.clear();
    if (toEpochSec <= fromEpochSec) toEpochSec = fromEpochSec + 3600;
    uint32_t span = toEpochSec - fromEpochSec;
    int nPoints = 40;
    for (int i = 0; i < nPoints; ++i) {
        MetricPoint p;
        p.tsSec = fromEpochSec + (span * i) / (nPoints - 1);
        double t = (double)i / nPoints;
        p.value = 50.0 + 20.0 * std::sin(t * 6.28318 * 2.0) + (i % 5);
        out.push_back(p);
    }
    err = "";
    return true;
}

bool dd::fetchMonitorChartSeries(const Monitor& monitor, uint32_t fromEpochSec, uint32_t toEpochSec,
                                  std::vector<MetricPoint>& out, String& err) {
    if (monitor.type == "synthetics alert") {
        out.clear();
        err = "no metrics query for this monitor type (sim)";
        return false;
    }
    return dd::fetchMetricSeries(monitor.query, fromEpochSec, toEpochSec, out, err);
}
