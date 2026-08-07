#include "storage.h"
#include "config.h"
#include <Preferences.h>

static Preferences prefs;

void storage::begin() {
    prefs.begin(NVS_NS, false);

    // Seed defaults for optional keys the very first time — ESP-IDF logs an
    // error-level line on every getString()/getBool() against a key that's
    // never been written, even though our code handles the fallback fine.
    // Writing the default once means the key always exists afterward and
    // that noise stops for good (matches the earlier dd_api_key flood fix —
    // same root cause, different key).
    if (!prefs.isKey(NVS_KEY_TIME_FORMAT_24H))   prefs.putBool(NVS_KEY_TIME_FORMAT_24H, true);
    if (!prefs.isKey(NVS_KEY_LED_BREATHE))       prefs.putBool(NVS_KEY_LED_BREATHE, true);
    if (!prefs.isKey(NVS_KEY_POLL_INTERVAL_SEC)) prefs.putInt(NVS_KEY_POLL_INTERVAL_SEC, DD_POLL_INTERVAL_SEC_DEFAULT);
    if (!prefs.isKey(NVS_KEY_ONCALL_TEAM_ID))    prefs.putString(NVS_KEY_ONCALL_TEAM_ID, "");
    if (!prefs.isKey(NVS_KEY_METRICS_ENABLED))   prefs.putBool(NVS_KEY_METRICS_ENABLED, false);
    if (!prefs.isKey(NVS_KEY_EVENTS_ENABLED))    prefs.putBool(NVS_KEY_EVENTS_ENABLED, false);
    if (!prefs.isKey(NVS_KEY_AUTO_ROTATE))       prefs.putBool(NVS_KEY_AUTO_ROTATE, false);
}

String storage::getApiKey() {
    return prefs.getString(NVS_KEY_API_KEY, "");
}

void storage::setApiKey(const String& key) {
    prefs.putString(NVS_KEY_API_KEY, key);
}

String storage::getAppKey() {
    return prefs.getString(NVS_KEY_APP_KEY, "");
}

void storage::setAppKey(const String& key) {
    prefs.putString(NVS_KEY_APP_KEY, key);
}

String storage::getSite() {
    return prefs.getString(NVS_KEY_SITE, "");
}

void storage::setSite(const String& site) {
    prefs.putString(NVS_KEY_SITE, site);
}

String storage::getOnCallTeamId() {
    return prefs.getString(NVS_KEY_ONCALL_TEAM_ID, "");
}

void storage::setOnCallTeamId(const String& teamId) {
    prefs.putString(NVS_KEY_ONCALL_TEAM_ID, teamId);
}

bool storage::getTimeFormat24h() {
    return prefs.getBool(NVS_KEY_TIME_FORMAT_24H, true);
}

void storage::setTimeFormat24h(bool v) {
    prefs.putBool(NVS_KEY_TIME_FORMAT_24H, v);
}

bool storage::getLedBreatheEnabled() {
    return prefs.getBool(NVS_KEY_LED_BREATHE, true);
}

void storage::setLedBreatheEnabled(bool v) {
    prefs.putBool(NVS_KEY_LED_BREATHE, v);
}

int storage::getPollIntervalSec() {
    return prefs.getInt(NVS_KEY_POLL_INTERVAL_SEC, DD_POLL_INTERVAL_SEC_DEFAULT);
}

void storage::setPollIntervalSec(int v) {
    prefs.putInt(NVS_KEY_POLL_INTERVAL_SEC, v);
}

bool storage::getAutoRotateEnabled() {
    return prefs.getBool(NVS_KEY_AUTO_ROTATE, false);   // off by default
}

void storage::setAutoRotateEnabled(bool v) {
    prefs.putBool(NVS_KEY_AUTO_ROTATE, v);
}

bool storage::getMetricsEnabled() {
    return prefs.getBool(NVS_KEY_METRICS_ENABLED, false);   // opt-in
}

void storage::setMetricsEnabled(bool v) {
    prefs.putBool(NVS_KEY_METRICS_ENABLED, v);
}

bool storage::getEventsEnabled() {
    return prefs.getBool(NVS_KEY_EVENTS_ENABLED, false);   // opt-in
}

void storage::setEventsEnabled(bool v) {
    prefs.putBool(NVS_KEY_EVENTS_ENABLED, v);
}

String storage::getMetricMetadataVersion() {
    return prefs.getString(NVS_KEY_METRIC_METADATA_VER, "");
}

void storage::setMetricMetadataVersion(const String& v) {
    prefs.putString(NVS_KEY_METRIC_METADATA_VER, v);
}

void storage::clearAll() {
    prefs.clear();
}

bool storage::hasKeys() {
    return prefs.getString(NVS_KEY_API_KEY, "").length() > 10 &&
           prefs.getString(NVS_KEY_APP_KEY, "").length() > 10;
}

bool storage::hasSite() {
    return prefs.getString(NVS_KEY_SITE, "").length() > 0;
}
