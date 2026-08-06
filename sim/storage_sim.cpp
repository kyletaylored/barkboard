// Simulator-only stand-in for src/storage.cpp. The real one depends on
// <Preferences.h> (ESP32 NVS flash), which doesn't exist on desktop. This
// implements the same storage.h API surface with plain in-memory statics —
// fine for a demo process that starts fresh every run.
#include "storage.h"

namespace {
String g_apiKey, g_appKey, g_site = "datadoghq.com";
bool g_timeFormat24h = true;
bool g_ledBreathe = true;
bool g_autoRotate = false;
}

void storage::begin() {}

String storage::getApiKey() { return g_apiKey; }
void storage::setApiKey(const String& key) { g_apiKey = key; }

String storage::getAppKey() { return g_appKey; }
void storage::setAppKey(const String& key) { g_appKey = key; }

String storage::getSite() { return g_site; }
void storage::setSite(const String& site) { g_site = site; }

bool storage::getTimeFormat24h() { return g_timeFormat24h; }
void storage::setTimeFormat24h(bool v) { g_timeFormat24h = v; }

bool storage::getLedBreatheEnabled() { return g_ledBreathe; }
void storage::setLedBreatheEnabled(bool v) { g_ledBreathe = v; }

bool storage::getAutoRotateEnabled() { return g_autoRotate; }
void storage::setAutoRotateEnabled(bool v) { g_autoRotate = v; }

void storage::clearAll() { g_apiKey = ""; g_appKey = ""; g_site = ""; }
bool storage::hasKeys() { return g_apiKey.length() > 0 && g_appKey.length() > 0; }
bool storage::hasSite() { return g_site.length() > 0; }
