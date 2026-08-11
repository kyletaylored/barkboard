// Sim-only Arduino compatibility shim. Provides just enough of the
// ESP32 Arduino core's surface for src/ui.cpp (and the vendored WString) to
// compile unmodified on a desktop: the String class (via the vendored
// WString.h/.cpp), millis(), a stand-in Serial/WiFi/ESP object, and the
// handful of numeric helpers ui.cpp reaches for. Never linked into the real
// ESP32 firmware — that build never sees this include path.
//
// NOTE: this header is included from both C++ translation units (ui.cpp,
// the shim itself) AND plain C ones — LVGL's lv_hal_tick.c pulls it in via
// LV_TICK_CUSTOM_INCLUDE (sim/lv_conf.h) to get millis(). Keep everything
// above the __cplusplus guard C-parseable.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Elapsed milliseconds since the simulator process started — stands in for
// the ESP32 core's tick count. Implemented in millis.c (plain C, using
// clock_gettime) so it links cleanly whether the including TU is C or C++.
uint32_t millis(void);

#ifdef __cplusplus
}
#endif

#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif

// NOTE: deliberately NOT providing an Arduino-style map() macro/function
// here — LVGL's own sources use "map" extensively as an identifier (button
// matrix maps, keyboard maps, etc.), and a #define map(...) would collide.
// src/ui.cpp doesn't call map() (only src/display.cpp does, for touch
// coordinate scaling, which isn't compiled into the simulator at all).

#ifdef __cplusplus
// ---- Everything below here is C++-only (String, Serial/WiFi/ESP stand-ins)
// ----------------------------------------------------------------------------
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#include "WString.h"

using std::max;
using std::min;

inline void delay(uint32_t) { /* main_sim.cpp drives its own frame pacing */ }

// ---- Serial ----------------------------------------------------------------
// ui.cpp only ever calls Serial.printf(...) for a one-time diagnostic log;
// route it to stdout instead of the ESP32 UART.
struct SerialShim {
    void printf(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    void println(const char* s = "") { ::printf("%s\n", s); }
    void print(const char* s) { ::printf("%s", s); }
};
extern SerialShim Serial;

// ---- WiFi -------------------------------------------------------------------
// ui.cpp's Settings screen reads WiFi.SSID()/localIP()/RSSI() for the
// diagnostic info box. Fake but stable values — there's no real WiFi in the
// simulator.
struct IPAddressShim {
    String toString() const { return String("192.168.1.42"); }
};
struct WiFiShim {
    String SSID() const { return String("BarkBoard-Sim"); }
    IPAddressShim localIP() const { return IPAddressShim(); }
    int RSSI() const { return -47; }
    // Feeds ui::deviceHostname()'s MAC-suffix slicing (see ui.cpp) — fake
    // but fixed, same as the rest of this shim.
    String macAddress() const { return String("AA:BB:CC:DD:EE:FF"); }
};
extern WiFiShim WiFi;

// ---- ESP --------------------------------------------------------------------
struct ESPShim {
    uint32_t getFreeHeap() const { return 180 * 1024; }
};
extern ESPShim ESP;

// ---- FreeRTOS stand-ins -------------------------------------------------
// ui.cpp guards a couple of flags shared with the real firmware's netTask()
// (core 0) behind portMUX_TYPE critical sections — real cross-core state on
// hardware, but the simulator is single-threaded, so these are safe no-ops
// here rather than a real spinlock. TaskHandle_t only needs to exist as a
// type: it shows up in datadog.h's submitDeviceMetrics() signature (pulled
// in transitively via ui.h), which the simulator never calls — main.cpp and
// datadog.cpp aren't compiled into the sim at all (see sim/Makefile; the sim
// links datadog_sim.cpp/storage_sim.cpp instead), so no real implementation
// is needed, just something for the declaration to parse against.
typedef int  portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux)  ((void)(mux))
typedef void* TaskHandle_t;

#endif // __cplusplus
