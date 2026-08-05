// Plain-C implementation of millis(), declared in Arduino.h. Kept as its own
// C translation unit (rather than in arduino_shim.cpp) so it links cleanly
// when pulled in by LVGL's plain-C lv_hal_tick.c (via
// LV_TICK_CUSTOM_INCLUDE "Arduino.h" in sim/lv_conf.h) as well as by C++
// callers like src/ui.cpp.
#include "Arduino.h"
#include <time.h>

uint32_t millis(void) {
    static int inited = 0;
    static struct timespec t0;
    struct timespec now;
    if (!inited) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        inited = 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec = now.tv_sec - t0.tv_sec;
    long nsec = now.tv_nsec - t0.tv_nsec;
    return (uint32_t)(sec * 1000L + nsec / 1000000L);
}
