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

#include <vector>

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

    String merr;
    std::vector<dd::Monitor> monitors;
    dd::fetchMonitors(dd::getMonitorFilter(), monitors, merr);

    String ierr;
    std::vector<dd::Incident> incidents;
    dd::fetchIncidents(incidents, ierr);

    String serr;
    std::vector<dd::SloSummary> slos;
    dd::fetchSlos(slos, serr);

    std::vector<dd::OnCallEntry> oncall;
    String oerr;
    dd::fetchOnCallForTeam("team-1", oncall, oerr);
    ui::notifyOnCallRefreshed(oncall, /*hasTeams=*/true);
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
    seedFakeDashboardData();
    ui::setStatusOnline(true);
    ui::showOverview();

    printf("[sim] BarkBoard simulator running. Click/drag with the mouse the way you'd touch the panel. Ctrl+C to quit.\n");

    while (1) {
        lv_timer_handler();
        SDL_Delay(5);
    }
    return 0;
}
