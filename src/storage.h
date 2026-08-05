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

    // A single bare team value (e.g. "my-team") ANDed onto every screen's
    // fetch, set via the web Settings page. Each fetcher in datadog.cpp
    // builds its own screen-specific query fragment from this — monitors/
    // SLOs use the "team:x" tag convention, incidents use the "teams:x"
    // (plural) custom field, on-call filters the team list by name — rather
    // than making the user know which raw syntax each screen wants.
    // "" means no scoping.
    String getTeamScope();
    void   setTeamScope(const String& team);

    // On-Call's own team selection — see NVS_KEY_ONCALL_TEAM_ID's doc
    // comment in config.h for why this is separate from getTeamScope().
    // "" means not yet resolved.
    String getOnCallTeamId();
    void   setOnCallTeamId(const String& teamId);

    bool   getTimeFormat24h();    // true = 24h clock, false = 12h with AM/PM
    void   setTimeFormat24h(bool v);

    bool   getLedBreatheEnabled();  // true = purple breathing LED when healthy, false = solid green
    void   setLedBreatheEnabled(bool v);

    int    getPollIntervalSec();    // how often the dashboard re-polls Datadog; default 60s
    void   setPollIntervalSec(int v);

    void   clearAll();
    bool   hasKeys();             // both api key and app key present
    bool   hasSite();
}
