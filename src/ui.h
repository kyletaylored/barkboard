#pragma once
#include <Arduino.h>
#include "datadog.h"

// LVGL screens. Boot/setup flow is fully built; the dashboard rotation
// covers Overview / Monitors / Incidents / On-Call (BARKBOARD_PLAN.md §8
// phases 5+7). SLOs land in phase 8 and slot into the same rotation (see
// DASH_COUNT in ui.cpp).
namespace ui {
    enum class Screen {
        None, Connecting, Setup, Waiting,
        Overview, Monitors, MonitorDetail, Incidents, OnCall, IncidentDetail, Slo, SloDetail, Settings, BitsIdle, Bits, BitsInvestigationDetail
    };

    void begin();

    // Full-screen busy overlay (spinner + label) on top of whatever screen
    // is showing. main.cpp's loop() is single-threaded and several actions
    // (monitor detail drill-in, mute, incident advance, on-call/SLO fetch,
    // ...) block on 1-3 sequential HTTPS calls with nothing else servicing
    // LVGL in between — without this, those stretches look indistinguishable
    // from a genuine freeze. Show before the blocking call(s), hide after.
    void showBusy(const String& msg);
    void hideBusy();

    void showConnecting(const String& msg);
    void showSetupHint(const String& ssid, const String& password, const String& help);
    void showWaitingForKeys(const String& portalUrl);
    void showOverview();              // enters the dashboard rotation (builds it on first call)
    Screen currentScreen();

    void setStatusOnline(bool online);
    void setClockText(const String& hhmm);

    // Data refresh notifications — read from dd::lastMonitorCounts()/dd::lastMonitors().
    void notifyMonitorCountsRefreshed();
    void notifyMonitorsListRefreshed();

    // Handoff to main.cpp's poll loop, mirroring pagerduty.cpp's ListFilter pattern:
    // ui.cpp sets the pending flag (filter chip tap, or landing on the Monitors
    // screen), main.cpp drains it, calls dd::fetchMonitors(), then notifies back.
    bool monitorsFetchPending(String& outFilter);

    // Monitor Detail (BARKBOARD_PLAN.md §4) — tapping a row sets the pending
    // request; main.cpp drains it, fetches detail + a 1h metric series, and
    // calls showMonitorDetail(). Long-pressing MUTE works the same way.
    bool monitorDetailRequestPending(long& outId);
    // Transitions to the detail screen immediately using the Monitor already
    // cached from the list row (name/status/query) — called synchronously
    // from the row-tap handler, before the real fetch below even starts, so
    // the tap gets instant visual feedback instead of a multi-second stall
    // while two sequential HTTPS calls (detail + metric series) complete.
    void showMonitorDetailLoading(const dd::Monitor& cached);
    // chartError explains *why* there's no chart (query error, no series for
    // this monitor type, etc.) instead of a blanket claim — advanced monitor
    // types (anomaly/outlier/forecast) often fail here because their `query`
    // field includes monitor-evaluation syntax that isn't valid classic
    // metrics-query syntax on its own, not because "this monitor type never
    // charts".
    void showMonitorDetail(const dd::Monitor& detail, const std::vector<dd::MetricPoint>& sparkline,
                           bool sparklineOk, const String& chartError);
    // outUntilEpochSec==0 is the sentinel for "unmute now" (see the Mute
    // 1h / Mute Today / Unmute action bar on Monitor Detail); any nonzero
    // value is a real mute-until epoch.
    bool monitorMutePending(long& outId, uint32_t& outUntilEpochSec);
    void applyMuteResult(bool ok, const String& msg);

    // Declare (action bar "Declare" -> Case/Incident) and Bits (action bar
    // "Bits" -> trigger investigation) — both navigate straight back to
    // Monitor Detail after the user's final tap and share its result label,
    // so there's just one applyDeclareResult()/applyBitsResult() each rather
    // than a result label per sub-screen.
    bool caseProjectsFetchPending();     // set when the user taps Declare > Case
    void notifyCaseProjectsRefreshed(const std::vector<dd::CaseProject>& projects);
    bool caseCreatePending(String& outProjectId, String& outTitle);
    bool incidentCreatePending(String& outTitle);
    bool bitsTriggerPending(long& outMonitorId);
    void applyDeclareResult(bool ok, const String& msg);
    void applyBitsResult(bool ok, const String& msg);

    // Incidents (BARKBOARD_PLAN.md §4 Screen 3). Landing on the Incidents
    // screen sets the pending flag; main.cpp drains it via dd::fetchIncidents()
    // and calls notifyIncidentsRefreshed(), which reads dd::lastIncidents().
    bool incidentsFetchPending();
    void notifyIncidentsRefreshed();

    // Incident Detail — built directly from the already-cached Incident
    // (no extra fetch needed; BARKBOARD_PLAN.md §3.1 notes the timeline data
    // is often sparse, so this screen deliberately stays simple).
    void showIncidentDetail(const dd::Incident& inc);
    bool incidentAdvancePending(String& outId, String& outNewState);
    void applyIncidentAdvanceResult(bool ok, const String& newState, const String& msg);

    // On-Call (BARKBOARD_PLAN.md §4 Screen 4). hasTeams distinguishes "no
    // teams configured" from "teams exist but no one's on call right now".
    bool oncallFetchPending();
    void notifyOnCallRefreshed(const std::vector<dd::OnCallEntry>& entries, bool hasTeams, bool needsTeamPick);

    // SLOs (BARKBOARD_PLAN.md §4 Screen 5) — list reads dd::lastSlos();
    // tapping a row requests the compact v2/status fetch for the arc gauge.
    bool sloFetchPending();
    void notifySlosRefreshed();
    bool sloDetailRequestPending(String& outId);
    void showSloDetail(const dd::SloSummary& summary, const dd::SloStatus& status, bool statusOk);

    // Bits AI Investigations (dashboard rotation) — list reads
    // dd::lastBitsInvestigations(), the most recent investigations org-wide
    // (not team-scoped — see dd::fetchBitsInvestigations()'s doc comment
    // for the /api/unstable/ endpoint this uses and why team scoping was
    // actually hiding real results). Tapping a row requests full detail via
    // the documented per-id GET.
    bool bitsInvestigationsFetchPending();
    void notifyBitsInvestigationsRefreshed();
    bool bitsInvestigationDetailRequestPending(String& outId);
    void showBitsInvestigationDetail(const dd::BitsInvestigationDetail& detail, const String& err);

    // Settings (BARKBOARD_PLAN.md §4 gear-icon screen) — reachable from any
    // dashboard screen's status bar. The scope-query/time-format preferences
    // live on the web Settings page (portal.cpp), not on-device — there's no
    // on-panel text entry (LV_USE_TEXTAREA/LV_USE_KEYBOARD are both off).
    void showSettings();
    bool chirpMuted();
    bool redetectSitePending();
    void applyRedetectResult(bool ok, const String& site, const String& err);
    bool factoryResetPending();

    // Bits idle screen (BARKBOARD_PLAN.md §4 Easter egg) — tap the Bits
    // mark in any status bar to reach it; tap back to return to whichever
    // dashboard screen was showing.
    void showBitsIdle();
}
