// BarkBoard LVGL simulator entry point. NOT part of the ESP32 firmware —
// this replaces src/main.cpp's hardware bring-up (WiFi, captive portal,
// TFT_eSPI/XPT2046 init) with LVGL's SDL2 display+mouse drivers so the exact
// same src/ui.cpp screen-building code renders in a desktop window that a
// screen recorder can capture. See sim/README.md for build/run instructions.
#include <lvgl.h>
// Full relative path (not just "sdl.h") to avoid an include-search
// ambiguity: macOS's case-insensitive filesystem would otherwise let a bare
// "sdl.h"/<SDL.h> lookup resolve to SDL2's own SDL.h instead of
// lv_drivers' driver header of (almost) the same name — see the Makefile's
// INCLUDES comment for the full story.
#include "vendor/lv_drivers/sdl/sdl.h"
#include SDL_INCLUDE_PATH

#include "ui.h"
#include "datadog.h"
#include "storage.h"
#include "config.h"

#include <ctime>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCREEN_WIDTH * SCREEN_HEIGHT];
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// Seeds dd::'s cached "last*" results the same way main.cpp's poll loop
// does after a real fetch — fetchXxx() populate the sim's canned data (see
// sim/datadog_sim.cpp), then lastXxx() is what ui.cpp actually reads when
// building each screen.
static void seedFakeDashboardData() {
    dd::MonitorCounts counts;
    dd::fetchMonitorCounts(counts);
    ui::notifyMonitorCountsRefreshed();

    String merr;
    std::vector<dd::Monitor> monitors;
    dd::fetchMonitors(dd::getMonitorFilter(), monitors, merr);
    ui::notifyMonitorsListRefreshed();

    String ierr;
    std::vector<dd::Incident> incidents;
    dd::fetchIncidents(incidents, ierr);
    ui::notifyIncidentsRefreshed();

    String serr;
    std::vector<dd::SloSummary> slos;
    dd::fetchSlos(slos, serr);
    ui::notifySlosRefreshed();

    std::vector<dd::OnCallEntry> oncall;
    String oerr;
    dd::fetchOnCallForTeamId("team-1", oncall, oerr);
    ui::notifyOnCallRefreshed(oncall, /*hasTeams=*/true, /*needsTeamPick=*/false);

    std::vector<dd::BitsInvestigation> investigations;
    String berr;
    dd::fetchBitsInvestigations(investigations, berr);
    ui::notifyBitsInvestigationsRefreshed();
}

// main.cpp's clock tick has a real equivalent here — the desktop system
// clock is already correct with no NTP-sync gate needed, so this is
// simpler than the firmware's version, but it's the same reason the clock
// never showed up before this: nothing was ever calling ui::setClockText()
// in the simulator at all, since that logic lived only in src/main.cpp,
// which isn't compiled into the sim (see sim/Makefile).
static void tickClock() {
    static time_t lastTick = 0;
    time_t now = time(nullptr);
    if (lastTick != 0 && now - lastTick < 30) return;
    lastTick = now;
    struct tm tmu; localtime_r(&now, &tmu);
    char buf[12];
    strftime(buf, sizeof(buf), storage::getTimeFormat24h() ? "%H:%M" : "%I:%M %p", &tmu);
    ui::setClockText(buf);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    lv_init();
    sdl_init();

    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_WIDTH * SCREEN_HEIGHT);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCREEN_WIDTH;
    disp_drv.ver_res  = SCREEN_HEIGHT;
    disp_drv.flush_cb = sdl_display_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&indev_drv);

    storage::begin();

    ui::begin();
    ui::setStatusOnline(true);
    // Must run before seedFakeDashboardData() — showOverview() is what lazily
    // builds all six dashboard screens (buildDashboard()) the first time
    // it's called. Every notifyXRefreshed() in seedFakeDashboardData() has an
    // `if (!s_xxxList) return;` guard (real widgets, populated once built),
    // so calling it first meant every one of those was a silent no-op —
    // the underlying dd:: fetch caches got populated fine, nothing ever
    // rendered them. Screens just show their normal "Loading..." placeholder
    // for the one frame between showOverview() and the seed call finishing.
    ui::showOverview();
    seedFakeDashboardData();
    tickClock();

    printf("[sim] BarkBoard simulator running. Click/drag with the mouse the way you'd touch the panel. Ctrl+C to quit.\n");

#ifdef __EMSCRIPTEN__
    // The browser owns its own event loop — blocking here with a native
    // while(1)/SDL_Delay would freeze the tab (no pthreads in this build).
    // emscripten_set_main_loop hands each "tick" back to the browser between
    // calls, the same role SDL_Delay(5) plays in the native build below.
    emscripten_set_main_loop([]() {
        lv_timer_handler();
        tickClock();
    }, 0, 1);
#else
    while (1) {
        lv_timer_handler();
        tickClock();
        SDL_Delay(5);
    }
#endif
    return 0;
}
