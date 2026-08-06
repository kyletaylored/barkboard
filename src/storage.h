#pragma once
#include <Arduino.h>

namespace storage {
    void begin();
    String getApiKey();
    void   setApiKey(const String& key);
    String getAppKey();
    void   setAppKey(const String& key);
    String getSite();             // detected Datadog site host, "" if not yet detected
    void   setSite(const String& site);

    // On-Call's team selection — see NVS_KEY_ONCALL_TEAM_ID's doc comment in
    // config.h; this now also doubles as Monitors/Incidents/Bits
    // Investigations' scope (dd::bareTeamScope() in datadog.cpp).
    // "" means not yet resolved.
    String getOnCallTeamId();
    void   setOnCallTeamId(const String& teamId);

    bool   getTimeFormat24h();    // true = 24h clock, false = 12h with AM/PM
    void   setTimeFormat24h(bool v);

    bool   getLedBreatheEnabled();  // true = purple breathing LED when healthy, false = solid green
    void   setLedBreatheEnabled(bool v);

    int    getPollIntervalSec();    // how often the dashboard re-polls Datadog; default 60s
    void   setPollIntervalSec(int v);

    // Off by default — see NVS_KEY_AUTO_ROTATE's doc comment in config.h.
    bool   getAutoRotateEnabled();
    void   setAutoRotateEnabled(bool v);

    // Opt-in — off by default. Sends a handful of device-health gauges
    // (free heap, WiFi RSSI, uptime) to the same Datadog org via
    // dd::submitDeviceMetrics(), tagged device:<AP SSID>. See its doc
    // comment in datadog.h for exactly what's sent and why it's opt-in.
    bool   getMetricsEnabled();
    void   setMetricsEnabled(bool v);

    // Separate opt-in — off by default. Gates dd::reportBootEvent() (a
    // Datadog Event fired only after an abnormal reboot). Kept independent
    // of getMetricsEnabled() above since custom metrics and Events are both
    // billable Datadog usage but distinct products a user may want on/off
    // separately.
    bool   getEventsEnabled();
    void   setEventsEnabled(bool v);

    void   clearAll();
    bool   hasKeys();             // both api key and app key present
    bool   hasSite();
}
