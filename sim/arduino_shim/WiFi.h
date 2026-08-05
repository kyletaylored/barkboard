// Sim-only stand-in for ESP32 Arduino's <WiFi.h> — src/ui.cpp includes this
// literally (for the Settings screen's SSID/IP/RSSI readout). The real
// WiFiShim/global `WiFi` object lives in Arduino.h so both the explicit
// `#include <WiFi.h>` and the implicit pull-in via Arduino.h resolve to the
// same declaration.
#pragma once
#include "Arduino.h"
