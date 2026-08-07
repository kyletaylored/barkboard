#include "ui.h"
#include "config.h"
#include "datadog.h"
#include "storage.h"
#include "fonts/fonts.h"
#include "assets_gen.h"
#include <lvgl.h>
#include <WiFi.h>

// DESIGN.md color tokens
#define COLOR_BG     lv_color_hex(0x0B0B10)
#define COLOR_SURFACE lv_color_hex(0x16151C)
#define COLOR_BORDER lv_color_hex(0x26242E)
#define COLOR_INK    lv_color_hex(0xF2F0F5)
#define COLOR_MUTED  lv_color_hex(0xB5B2C0)
#define COLOR_PURPLE lv_color_hex(0x632CA6)
#define COLOR_OK     lv_color_hex(0x3FB950)
#define COLOR_WARN   lv_color_hex(0xF2C94C)
#define COLOR_ALERT  lv_color_hex(0xF0506E)
#define COLOR_NODATA lv_color_hex(0x8A8894)

// Nav arrows now sit at the bottom, flanking the page dots (see
// addNavArrows/addPageDots) instead of the screen's left/right edges — this
// is just the standard outer margin now, not clearance for a 28px-wide
// side button.
#define LIST_INSET  8
#define LIST_WIDTH  (SCREEN_WIDTH - LIST_INSET * 2)
#define PAGE_DOT_W   8
#define PAGE_DOT_GAP 6
#define NAV_ARROW_W  24
#define NAV_ARROW_H  24

// Single-line row title height for each font — LV_LABEL_LONG_DOT needs an
// explicit height to know when to truncate with "…"; without one it just
// wraps indefinitely and overlaps whatever's below it.
#define ROW_TITLE_H_14 18
#define ROW_TITLE_H_16 20
#define ROW_TITLE_H_18 22

// ============================================================================
// Modal screens (Connecting / Setup / Waiting) — rebuilt in place on
// lv_scr_act() each call. Mutually exclusive with the dashboard rotation
// below: the device is either still setting up, or fully in the dashboard,
// never both, so reusing a single scratch screen object is safe and simple.
// ============================================================================

static ui::Screen s_screen = ui::Screen::None;
static lv_obj_t*  s_title  = nullptr;
static lv_obj_t*  s_body   = nullptr;
static lv_obj_t*  s_statusDot = nullptr;
static lv_obj_t*  s_clock  = nullptr;
static lv_obj_t*  s_qr     = nullptr;

static void clearScreen() {
    lv_obj_clean(lv_scr_act());
    s_title = s_body = s_statusDot = s_clock = s_qr = nullptr;
}

// ---- Busy overlay ----
// Parented to lv_layer_top() so it floats above whichever screen is showing
// without touching that screen's own objects (clearScreen()/screen swaps
// elsewhere never affect it). Built once and hidden/shown from then on —
// churning lv_spinner_create() per call would leak into the 56 KB LVGL pool
// (include/lv_conf.h) since nothing here ever deletes it.
static lv_obj_t* s_busyOverlay = nullptr;
static lv_obj_t* s_busyLabel   = nullptr;

static void buildBusyOverlayIfNeeded() {
    if (s_busyOverlay) return;
    s_busyOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_busyOverlay, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_pos(s_busyOverlay, 0, 0);
    lv_obj_set_style_bg_color(s_busyOverlay, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_busyOverlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_busyOverlay, 0, 0);
    lv_obj_set_style_radius(s_busyOverlay, 0, 0);
    lv_obj_clear_flag(s_busyOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* spinner = lv_spinner_create(s_busyOverlay, 1000, 90);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_style_arc_color(spinner, COLOR_PURPLE, LV_PART_INDICATOR);

    s_busyLabel = lv_label_create(s_busyOverlay);
    lv_obj_set_style_text_color(s_busyLabel, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_busyLabel, &outfit_thin_14, 0);
    lv_obj_align(s_busyLabel, LV_ALIGN_CENTER, 0, 24);

    lv_obj_add_flag(s_busyOverlay, LV_OBJ_FLAG_HIDDEN);
}

void ui::showBusy(const String& msg) {
    buildBusyOverlayIfNeeded();
    lv_label_set_text(s_busyLabel, msg.c_str());
    lv_obj_clear_flag(s_busyOverlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_busyOverlay);
}

void ui::hideBusy() {
    if (s_busyOverlay) lv_obj_add_flag(s_busyOverlay, LV_OBJ_FLAG_HIDDEN);
}

static void showQrScreen(const String& title, const String& qrData, const String& caption) {
    clearScreen();
    lv_obj_set_style_bg_color(lv_scr_act(), COLOR_BG, 0);

    // Bits mark (BARKBOARD_PLAN.md §6) — official white vertical icon,
    // downloaded from Datadog's press kit and converted via png_to_lvgl.py.
    lv_obj_t* bits = lv_img_create(lv_scr_act());
    lv_img_set_src(bits, &bits_icon_small);
    lv_obj_align(bits, LV_ALIGN_TOP_LEFT, 12, 8);

    s_title = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(s_title, COLOR_PURPLE, 0);
    lv_label_set_text(s_title, title.c_str());
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 10, 10);

    s_qr = lv_qrcode_create(lv_scr_act(), 110, lv_color_black(), lv_color_white());
    lv_qrcode_update(s_qr, qrData.c_str(), qrData.length());
    lv_obj_align(s_qr, LV_ALIGN_LEFT_MID, 18, 6);
    lv_obj_set_style_border_color(s_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_qr, 6, 0);

    s_body = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(s_body, COLOR_INK, 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body, 150);
    lv_label_set_text(s_body, caption.c_str());
    lv_obj_align(s_body, LV_ALIGN_RIGHT_MID, -16, 6);
}

// Boot/connecting screen — Bits mark replaces the reference project's plain
// "Booting..." text (BARKBOARD_PLAN.md §6). Callers only ever pass "BarkBoard"
// as the title; kept as a parameter for symmetry with showQrScreen.
static void showCentered(const String& title, const String& body) {
    clearScreen();
    lv_obj_set_style_bg_color(lv_scr_act(), COLOR_BG, 0);

    lv_obj_t* bits = lv_img_create(lv_scr_act());
    lv_img_set_src(bits, &bits_icon_big);
    lv_obj_align(bits, LV_ALIGN_CENTER, 0, -48);

    s_title = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(s_title, COLOR_PURPLE, 0);
    lv_obj_set_style_text_font(s_title, &outfit_bold_14, 0);
    lv_obj_set_style_text_letter_space(s_title, 2, 0);
    lv_label_set_text(s_title, title.c_str());
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -4);

    s_body = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(s_body, COLOR_INK, 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body, SCREEN_WIDTH - 40);
    lv_label_set_text(s_body, body.c_str());
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 24);
}

void ui::showConnecting(const String& msg) {
    s_screen = Screen::Connecting;
    showCentered("BarkBoard", msg);
}

void ui::showSetupHint(const String& ssid, const String& password, const String& help) {
    s_screen = Screen::Setup;
    // Standard WIFI-QR format (BARKBOARD_PLAN.md §3.2) — both iOS and Android
    // camera apps join the network directly from this, no keyboard needed.
    // The AP is deliberately open (empty password), so this encodes T:nopass
    // rather than T:WPA — see config.h's AP_SSID_PREFIX comment.
    bool open = password.length() == 0;
    String qrData = open ? ("WIFI:T:nopass;S:" + ssid + ";;")
                          : ("WIFI:T:WPA;S:" + ssid + ";P:" + password + ";;");
    String caption = open
        ? (String("Scan to join\n\"") + ssid + "\"\n\nor type:\n" + ssid)
        : (String("Scan to join\n\"") + ssid + "\"\n\nor type:\n" + ssid + "\n" + password);
    showQrScreen("Scan to set up", qrData, caption);
}

void ui::showWaitingForKeys(const String& portalUrl) {
    s_screen = Screen::Waiting;
    int nl = portalUrl.indexOf('\n');
    String url = nl >= 0 ? portalUrl.substring(0, nl) : portalUrl;
    String caption = "Scan to open\nsetup page\n\nor visit:\n" + portalUrl;
    showQrScreen("Almost there", url, caption);
}

// ============================================================================
// Dashboard rotation — persistent screen objects, page dots, swipe gestures.
// Overview / Monitors / Incidents / On-Call exist (BARKBOARD_PLAN.md §8
// phases 5+7); SLOs slot into DASH_COUNT in phase 8.
// ============================================================================

enum DashIdx { DASH_OVERVIEW = 0, DASH_MONITORS, DASH_INCIDENTS, DASH_ONCALL, DASH_SLO, DASH_BITS, DASH_COUNT };
static const char* const DASH_TITLE[DASH_COUNT] = { "OVERVIEW", "MONITORS", "INCIDENTS", "ON-CALL", "SLOS", "BITS" };

static bool      s_dashBuilt = false;
static lv_obj_t* s_dashScr[DASH_COUNT] = { nullptr };
static lv_obj_t* s_dashDots[DASH_COUNT][DASH_COUNT] = { { nullptr } };
static lv_obj_t* s_dashStatusDot[DASH_COUNT] = { nullptr };
static lv_obj_t* s_dashClock[DASH_COUNT] = { nullptr };
static int       s_dashIdx = 0;
static bool      s_online = true;

// Overview widgets
static lv_obj_t* s_ovMonitorBadge  = nullptr;
static lv_obj_t* s_ovIncidentBadge = nullptr;
static lv_obj_t* s_ovLastPoll      = nullptr;

// Monitors widgets
static const char* const MON_FILTERS[] = { "ALL", "ALERT", "WARN", "NO DATA" };
static const int MON_FILTER_COUNT = 4;
static lv_obj_t* s_monChips[MON_FILTER_COUNT] = { nullptr };
static int       s_monActiveFilter = 0;
static lv_obj_t* s_monList = nullptr;
static lv_obj_t* s_monEmpty = nullptr;

static volatile bool s_monitorsFetchPending = false;
static volatile bool s_monitorDetailRequestPending = false;
static long           s_monitorDetailRequestId = 0;
// Guards s_monitorDetailRequestPending/Id and s_oncallFetchPending only —
// the two pending flags whose consumer is now the core-0 net task
// (main.cpp), not core 1's loop() like every other pending flag in this
// file. `volatile` alone (already on both bools) stops compiler reordering
// within one core's view but gives no cross-core memory barrier and no
// atomicity across the flag+payload pair — a real critical section is
// needed once the reader and writer are on different cores.
static portMUX_TYPE s_pendingMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_monitorMutePending = false;
static long           s_monitorMuteId = 0;
// 0 is a real epoch value in principle, but muteUntilEpochSec is always
// "now + duration" (>1.7B in 2026) whenever it means "mute" — 0 is reserved
// to mean "unmute" so one pending-flag pair can carry all three actions.
static uint32_t       s_monitorMuteUntilEpochSec = 0;

// Incidents widgets
static lv_obj_t* s_incList = nullptr;
static volatile bool s_incidentsFetchPending = false;

// On-Call widgets
static lv_obj_t* s_ocList = nullptr;
static lv_obj_t* s_ocCurrentCard = nullptr;   // hero card for who's on call right now
static lv_obj_t* s_ocCurrentIcon = nullptr;
static lv_obj_t* s_ocCurrentName = nullptr;
static lv_obj_t* s_ocCurrentEmail = nullptr;   // secondary line — the only other metadata this API actually returns for a responder (confirmed live: no schedule/shift-timing field exists here)
static std::vector<dd::OnCallEntry> s_lastOnCall;
static volatile bool s_oncallFetchPending = false;

// SLO widgets
static lv_obj_t* s_sloList = nullptr;
static volatile bool s_sloFetchPending = false;

// Bits Investigations widgets
static lv_obj_t* s_bitsInvList = nullptr;
static volatile bool s_bitsInvestigationsFetchPending = false;
static volatile bool s_bitsInvestigationDetailRequestPending = false;
static String s_bitsInvestigationDetailRequestId;

// Bits Investigation Detail widgets
static lv_obj_t* s_bitsInvDetailScr = nullptr;
static lv_obj_t* s_bitsInvDetailTitle = nullptr;
static lv_obj_t* s_bitsInvDetailStatus = nullptr;
static lv_obj_t* s_bitsInvDetailBody = nullptr;   // scrollable container below the fixed header
static lv_obj_t* s_bitsInvDetailConclTitle = nullptr;
static lv_obj_t* s_bitsInvDetailConclSummary = nullptr;

// SLO Detail
static lv_obj_t* s_sloDetailScr   = nullptr;
static lv_obj_t* s_sloDetailName  = nullptr;
static lv_obj_t* s_sloDetailArc   = nullptr;
static lv_obj_t* s_sloDetailPct   = nullptr;
static lv_obj_t* s_sloDetailMeta  = nullptr;
static volatile bool s_sloDetailRequestPending = false;
static String    s_sloDetailRequestId;

// Incident Detail
static lv_obj_t* s_incDetailScr    = nullptr;
static lv_obj_t* s_incDetailTitle  = nullptr;
static lv_obj_t* s_incDetailBadge  = nullptr;
static lv_obj_t* s_incDetailMeta   = nullptr;
static lv_obj_t* s_incDetailResult = nullptr;
static String    s_incDetailId;
static String    s_incDetailState;
static String    s_incDetailSeverity;
static volatile bool s_incidentAdvancePending = false;
static String    s_incidentAdvanceId;

static lv_obj_t* s_detailScr    = nullptr;
static lv_obj_t* s_detailName   = nullptr;
static lv_obj_t* s_detailStatus = nullptr;
static lv_obj_t* s_detailChart  = nullptr;
static lv_chart_series_t* s_detailSeries = nullptr;   // created once; see buildDetailScreenIfNeeded()
static lv_chart_cursor_t* s_detailCursor = nullptr;   // vertical line marking which point the labels refer to
// Mirrors the currently-plotted (decimated) points — value AND timestamp,
// kept around purely so the tap-to-inspect handler on s_detailChart (see
// buildDetailScreenIfNeeded) can look up a pressed point's real value/time
// by index. lv_chart itself only stores the lv_coord_t-cast, already-lossy
// plotted values, and doesn't track timestamps at all.
static std::vector<dd::MetricPoint> s_detailChartPoints;
static lv_obj_t* s_detailChartLoLbl  = nullptr;   // y-axis min, bottom-left of chart
static lv_obj_t* s_detailChartHiLbl  = nullptr;   // y-axis max, top-left of chart
static lv_obj_t* s_detailCurrentLbl  = nullptr;   // highlighted point's value, top-right of chart
static lv_obj_t* s_detailCritLine    = nullptr;   // horizontal reference line at options.thresholds.critical
static lv_obj_t* s_detailWarnLine    = nullptr;   // horizontal reference line at options.thresholds.warning
// X-axis reference ticks: 3 evenly-spaced marks along the chart's time span
// — "Now" (right edge) through the oldest plotted point (left edge), each
// with a short tick line below the chart plus an age label below that.
// Replaces the old raw-query text row entirely (near-unreadable truncated
// to one line, and told you nothing about *when* — see updateChartAxisLabels()).
#define DETAIL_AXIS_TICK_COUNT 3
static lv_obj_t* s_detailAxisTick[DETAIL_AXIS_TICK_COUNT]  = { nullptr };
static lv_obj_t* s_detailAxisLbl[DETAIL_AXIS_TICK_COUNT]   = { nullptr };
static lv_obj_t* s_detailNoChart = nullptr;
static lv_obj_t* s_detailMuteResult = nullptr;
// Action bar: 3 equal slots. Slot 1 is state-aware (Mute <-> Unmute, same
// object re-labeled — not two objects like the old Mute1h/MuteToday pair,
// since only one of Mute/Unmute is ever valid at a time); slots 2/3 are
// fixed (Declare, Bits). All three navigate to a confirm/options screen on
// tap rather than acting immediately — Unmute is the one exception (long-
// press executes directly), since re-enabling alerting is the "safe"
// direction and doesn't need the same guard the other three do.
static lv_obj_t* s_detailMuteBtn    = nullptr;
static lv_obj_t* s_detailMuteIcon   = nullptr;
static lv_obj_t* s_detailMuteLbl    = nullptr;
static long       s_detailMonitorId = 0;
static bool        s_detailMonitorMuted = false;

// 72, not 90 — "No Data" (the longest status string) still fits comfortably
// at this font size, and the ~18px it frees widens the marquee title box
// next to it, which was cramped for longer monitor names.
#define DETAIL_STATUS_W 72
// Pinned near the bottom of the screen (240px tall) rather than right under
// the query line — there's room now that tags are gone and the chart grew
// to fill the middle, and bottom placement reads more like a persistent
// toolbar than a stray row of buttons wherever the content happened to end.
#define ACTION_BAR_Y   204
#define ACTION_BTN_H   32
#define ACTION_BTN_GAP 8
#define ACTION_BTN_W   ((SCREEN_WIDTH - 24 - 2 * ACTION_BTN_GAP) / 3)

// Auto-generated title for Declare (Case/Incident) — there's no on-panel
// keyboard, so this is the whole title, not a starting point the user edits.
static String declareTitleFor(const String& monitorName, const String& status) {
    String s = status; s.toUpperCase();
    // Plain hyphen, not an em dash — the on-device bitmap font (lv_font_conv
    // output) doesn't include U+2014, and drawing a missing glyph logs an
    // LVGL warning on *every redraw*, not just once. Harmless by itself, but
    // real spam once this label sits inside a scrolling marquee (buildTableRow)
    // or repaints every frame for any other reason.
    return monitorName + " - " + s;
}

// Settings
static lv_obj_t* s_settingsScr    = nullptr;
static lv_obj_t* s_settingsInfo   = nullptr;
static lv_obj_t* s_settingsSite   = nullptr;
static lv_obj_t* s_settingsChirp  = nullptr;
static lv_obj_t* s_settingsReset  = nullptr;
static lv_obj_t* s_settingsResult = nullptr;
static ui::Screen s_screenBeforeSettings = ui::Screen::Overview;
// volatile — chirpMuted() is now read from the core-0 net task (main.cpp's
// netTask(), for the new-alert chirp), not just core 1.
static volatile bool s_chirpMuted = false;
static int       s_resetConfirmArmed = 0;   // 0=idle, 1=armed (tap again to confirm)
static volatile bool s_redetectSitePending = false;
static volatile bool s_factoryResetPending = false;

static void rotateTo(int idx, bool fromUser);
static void onDashGesture(lv_event_t* e);
static void logMemCheckpoint(const char* label);
static void showMuteOptionsScreen();
static void showDeclareOptionsScreen();
static void showBitsConfirmScreen();

static void styleFullscreen(lv_obj_t* o) {
    lv_obj_set_size(o, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(o, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// Status bar: 24px tall — Bits mark (left, BARKBOARD_PLAN.md §6), title,
// clock + online dot (right).
static void addStatusBar(lv_obj_t* parent, int idx) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCREEN_WIDTH, 24);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bits = lv_img_create(bar);
    lv_img_set_src(bits, &bits_icon_small);
    lv_obj_align(bits, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_flag(bits, LV_OBJ_FLAG_CLICKABLE);
    // The icon itself is only 18x20px — too small a target to hit reliably
    // on a resistive touchscreen. Widens the invisible tappable area to
    // ~20px on every side without changing how it looks.
    lv_obj_set_ext_click_area(bits, 20);
    lv_obj_add_event_cb(bits, [](lv_event_t*) { ui::showBitsIdle(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* t = lv_label_create(bar);
    lv_label_set_text(t, DASH_TITLE[idx]);
    lv_obj_set_style_text_color(t, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(t, &outfit_bold_14, 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 32, 0);

    lv_obj_t* clk = lv_label_create(bar);
    lv_obj_set_style_text_color(clk, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(clk, &outfit_thin_14, 0);
    lv_label_set_text(clk, "--:--");
    // -100, not -66 — makes room for the Home button below on every screen
    // except Overview itself.
    lv_obj_align(clk, LV_ALIGN_RIGHT_MID, -100, 0);
    s_dashClock[idx] = clk;

    // Home: jumps straight back to Overview. Without this, tapping a stream-
    // deck tile on Overview lands you inside the swipe carousel with no way
    // back except swiping through every other screen — this is the direct
    // "go home" action that was missing.
    if (idx != DASH_OVERVIEW) {
        lv_obj_t* home = lv_btn_create(bar);
        lv_obj_set_size(home, 24, 24);
        lv_obj_set_style_bg_opa(home, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(home, 0, 0);
        lv_obj_set_style_border_width(home, 0, 0);
        lv_obj_align(home, LV_ALIGN_RIGHT_MID, -60, 0);
        lv_obj_t* homeLbl = lv_label_create(home);
        lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
        lv_obj_set_style_text_color(homeLbl, COLOR_MUTED, 0);
        lv_obj_center(homeLbl);
        lv_obj_add_event_cb(home, [](lv_event_t*) { rotateTo(DASH_OVERVIEW, true); }, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t* dot = lv_obj_create(bar);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, s_online ? COLOR_OK : COLOR_ALERT, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -8, 0);
    s_dashStatusDot[idx] = dot;

    // Gear icon: reaches Settings from any dashboard screen (BARKBOARD_PLAN.md
    // §4 "Status/Settings screen (gear icon, same as original)").
    lv_obj_t* gear = lv_btn_create(bar);
    lv_obj_set_size(gear, 24, 24);
    lv_obj_set_style_bg_opa(gear, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(gear, 0, 0);
    lv_obj_set_style_border_width(gear, 0, 0);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -34, 0);
    lv_obj_t* gearLbl = lv_label_create(gear);
    lv_label_set_text(gearLbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gearLbl, COLOR_MUTED, 0);
    lv_obj_center(gearLbl);
    lv_obj_add_event_cb(gear, [](lv_event_t*) { ui::showSettings(); }, LV_EVENT_CLICKED, nullptr);
}

// Page dots: 8px diameter, bottom-center, 6px gutter (DESIGN.md) — tap to jump.
// Shared geometry — addNavArrows() needs the dot row's bounds to flank it,
// so both functions compute from the same constants instead of one
// hardcoding a number that has to stay in sync with the other.
static int pageDotsRowY()     { return SCREEN_HEIGHT - 16; }
static int pageDotsRowWidth() { return DASH_COUNT * PAGE_DOT_W + (DASH_COUNT - 1) * PAGE_DOT_GAP; }
static int pageDotsRowX0()    { return (SCREEN_WIDTH - pageDotsRowWidth()) / 2; }

static void addPageDots(lv_obj_t* parent, int idx) {
    int y = pageDotsRowY();
    int x0 = pageDotsRowX0();
    for (int i = 0; i < DASH_COUNT; ++i) {
        lv_obj_t* dt = lv_obj_create(parent);
        lv_obj_set_size(dt, PAGE_DOT_W, PAGE_DOT_W);
        lv_obj_set_pos(dt, x0 + i * (PAGE_DOT_W + PAGE_DOT_GAP), y);
        lv_obj_set_style_radius(dt, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dt, 0, 0);
        lv_obj_set_style_bg_color(dt, (i == idx) ? COLOR_PURPLE : COLOR_BORDER, 0);
        lv_obj_clear_flag(dt, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(dt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(dt, (void*)(intptr_t)i);
        lv_obj_add_event_cb(dt, [](lv_event_t* e) {
            int target = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            rotateTo(target, true);
        }, LV_EVENT_CLICKED, nullptr);
        s_dashDots[idx][i] = dt;
    }
}

// Explicit tap zones, not just swipe — the Incidents/Monitors/On-Call/SLO
// screens are mostly a scrollable lv_list, which by default consumes swipe
// gestures for its own vertical scrolling rather than bubbling them up to
// this screen's LV_EVENT_GESTURE handler (that's also fixed separately via
// LV_OBJ_FLAG_GESTURE_BUBBLE on each list), so swipe-to-page-over could
// silently stop working once a list fills most of the screen. These arrows
// work regardless of what's underneath them.
//
// Bottom-flanking the page dots, not the screen's left/right edges — freed
// up a 28px-per-side strip every list screen used to reserve as clearance
// (see LIST_INSET), which now goes straight to table width instead.
static void addNavArrows(lv_obj_t* parent, int idx) {
    int y = pageDotsRowY() - (NAV_ARROW_H - PAGE_DOT_W) / 2;   // vertically centered on the dot row
    int leftX  = pageDotsRowX0() - PAGE_DOT_GAP - NAV_ARROW_W;
    int rightX = pageDotsRowX0() + pageDotsRowWidth() + PAGE_DOT_GAP;

    lv_obj_t* left = lv_btn_create(parent);
    lv_obj_set_size(left, NAV_ARROW_W, NAV_ARROW_H);
    lv_obj_set_pos(left, leftX, y);
    lv_obj_set_style_bg_color(left, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_60, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_radius(left, 6, 0);
    lv_obj_set_user_data(left, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(left, [](lv_event_t* e) {
        int cur = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
        rotateTo(cur - 1, true);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* leftLbl = lv_label_create(left);
    lv_label_set_text(leftLbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(leftLbl, COLOR_MUTED, 0);
    lv_obj_center(leftLbl);

    lv_obj_t* right = lv_btn_create(parent);
    lv_obj_set_size(right, NAV_ARROW_W, NAV_ARROW_H);
    lv_obj_set_pos(right, rightX, y);
    lv_obj_set_style_bg_color(right, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_60, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_radius(right, 6, 0);
    lv_obj_set_user_data(right, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(right, [](lv_event_t* e) {
        int cur = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
        rotateTo(cur + 1, true);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* rightLbl = lv_label_create(right);
    lv_label_set_text(rightLbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(rightLbl, COLOR_MUTED, 0);
    lv_obj_center(rightLbl);
}

static lv_obj_t* makeDashScreen(int idx) {
    lv_obj_t* s = lv_obj_create(nullptr);
    styleFullscreen(s);
    addStatusBar(s, idx);
    addPageDots(s, idx);
    // addNavArrows() is deliberately NOT called here — it needs to run after
    // buildOverview()/buildMonitors()/etc populate each screen's content, so
    // the arrows land on top in z-order and stay tappable at the screen
    // edges instead of getting covered by a list that also reaches there.
    lv_obj_add_event_cb(s, onDashGesture, LV_EVENT_GESTURE, nullptr);
    return s;
}

// ---- Overview ----
// Stream-deck-style home: a 2x2 grid of tappable tiles, one per dashboard
// screen, jumping straight there instead of requiring several swipes —
// replaces the earlier ALERT/WARN/OK big-counter layout, which only ever
// linked to Monitors and buried the other three screens behind swipe-only
// navigation. Each tile's optional badge surfaces the one number worth
// knowing before you tap in (how many monitors need attention, how many
// incidents are open); On-Call/SLOs skip a badge since neither has a single
// summary number cheap enough to keep resident on this screen.
static lv_obj_t* addNavTile(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                             const char* icon, const char* label, int targetIdx, lv_obj_t** outBadge,
                             lv_event_cb_t customClickCb = nullptr) {
    lv_obj_t* tile = lv_obj_create(parent);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_bg_color(tile, COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(tile, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_radius(tile, 10, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    if (customClickCb) {
        // Caller wants tap behavior other than "rotate to a DashIdx tab"
        // (e.g. jumping straight to the Bits idle overlay, which isn't a
        // dashboard tab at all) — user_data/targetIdx are unused in this path.
        lv_obj_add_event_cb(tile, customClickCb, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_set_user_data(tile, (void*)(intptr_t)targetIdx);
        lv_obj_add_event_cb(tile, [](lv_event_t* e) {
            int target = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            rotateTo(target, true);
        }, LV_EVENT_CLICKED, nullptr);
    }

    // Tile is 50px tall (3-row grid, see buildOverview()). outfit_bold_22's
    // line_height is 24px, outfit_bold_14's is 15px: 6 (top gap) + 24 (icon)
    // + 4 (bottom gap) + 15 (label) - 1px overlap headroom leaves 1px to
    // spare at the very bottom, so top-align the icon and bottom-align the
    // label rather than trying to center both independently.
    lv_obj_t* iconLbl = lv_label_create(tile);
    lv_obj_set_style_text_font(iconLbl, &outfit_bold_22, 0);
    lv_obj_set_style_text_color(iconLbl, COLOR_PURPLE, 0);
    lv_label_set_text(iconLbl, icon);
    lv_obj_align(iconLbl, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t* nameLbl = lv_label_create(tile);
    lv_obj_set_style_text_font(nameLbl, &outfit_bold_14, 0);
    lv_obj_set_style_text_color(nameLbl, COLOR_INK, 0);
    lv_label_set_text(nameLbl, label);
    lv_obj_align(nameLbl, LV_ALIGN_BOTTOM_MID, 0, -4);

    if (outBadge) {
        lv_obj_t* badge = lv_label_create(tile);
        lv_obj_set_style_text_font(badge, &outfit_bold_12, 0);
        lv_obj_set_style_text_color(badge, COLOR_ALERT, 0);
        lv_label_set_text(badge, "");
        lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -6, 2);
        *outBadge = badge;
    }
    return tile;
}

// Sibling to addNavTile() for the one Overview tile that doesn't jump to a
// DASH_* tab: tapping it opens the Bits idle overlay directly (a full-screen
// widget reached elsewhere only via the status-bar mark or a long-press,
// see showBitsIdle()). Uses the small Bits bitmap instead of a symbol-font
// icon, so it needs an lv_img rather than addNavTile()'s icon label.
static lv_obj_t* addBitsNavTile(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                                 const char* label) {
    lv_obj_t* tile = lv_obj_create(parent);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_bg_color(tile, COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(tile, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_radius(tile, 10, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, [](lv_event_t*) { ui::showBitsIdle(); }, LV_EVENT_CLICKED, nullptr);

    // bits_icon_small is 18x20 (see addStatusBar()) - same 6px top gap as
    // addNavTile()'s icon, well within the 50px tile height alongside the
    // 15px label line below it.
    lv_obj_t* img = lv_img_create(tile);
    lv_img_set_src(img, &bits_icon_small);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t* nameLbl = lv_label_create(tile);
    lv_obj_set_style_text_font(nameLbl, &outfit_bold_14, 0);
    lv_obj_set_style_text_color(nameLbl, COLOR_INK, 0);
    lv_label_set_text(nameLbl, label);
    lv_obj_align(nameLbl, LV_ALIGN_BOTTOM_MID, 0, -4);

    return tile;
}

static void buildOverview() {
    lv_obj_t* s = s_dashScr[DASH_OVERVIEW];

    // 3 rows x 2 columns to fit six tiles (four dashboard tabs, an
    // Investigations shortcut into DASH_BITS, and a direct Bits-mark
    // shortcut) while keeping tileW - and the room for text like
    // "Investigations" - the same as the original 2x2 layout, at the cost
    // of a shorter tileH.
    const int gap    = 10;
    const int gridX  = LIST_INSET;      // matches the nav-arrow inset every other screen uses
    const int gridY  = 30;
    const int gridW  = LIST_WIDTH;
    const int gridH  = 170;             // leaves room below for the last-poll line + page dots
    const int tileW  = (gridW - gap) / 2;
    const int tileH  = (gridH - 2 * gap) / 3;
    const int row0   = gridY;
    const int row1   = gridY + tileH + gap;
    const int row2   = gridY + 2 * (tileH + gap);
    const int col0   = gridX;
    const int col1   = gridX + tileW + gap;

    addNavTile(s, col0, row0, tileW, tileH, LV_SYMBOL_BELL,    "Monitors",  DASH_MONITORS,  &s_ovMonitorBadge);
    addNavTile(s, col1, row0, tileW, tileH, LV_SYMBOL_WARNING, "Incidents", DASH_INCIDENTS, &s_ovIncidentBadge);
    addNavTile(s, col0, row1, tileW, tileH, LV_SYMBOL_CALL,    "On-Call",   DASH_ONCALL,    nullptr);
    addNavTile(s, col1, row1, tileW, tileH, LV_SYMBOL_OK,      "SLOs",      DASH_SLO,       nullptr);
    addNavTile(s, col0, row2, tileW, tileH, LV_SYMBOL_EYE_OPEN, "Investigations", DASH_BITS, nullptr);
    addBitsNavTile(s, col1, row2, tileW, tileH, "Bits");

    s_ovLastPoll = lv_label_create(s);
    lv_obj_set_style_text_font(s_ovLastPoll, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_ovLastPoll, COLOR_MUTED, 0);
    lv_label_set_text(s_ovLastPoll, "Waiting for first poll...");
    lv_obj_align(s_ovLastPoll, LV_ALIGN_BOTTOM_MID, 0, -26);
}

// The Overview counters are a fixed 100px-wide zone at a 48px font, sized
// for a typical count — a 4+ digit raw number (thousands of alerts, which
// happens on larger orgs) overflows that zone. Abbreviating past 999 keeps
// the string short regardless of scale instead of just buying a few more
// digits, same convention Grafana/Datadog's own big-number widgets use.
static String formatCount(int n) {
    if (n < 1000) return String(n);
    if (n < 10000) return String(n / 1000.0f, 1) + "k";   // 3.4k
    if (n < 1000000) return String(n / 1000) + "k";        // 42k
    return String(n / 1000000.0f, 1) + "M";
}

// Formats a metric value for the Monitor Detail chart's axis/current-value
// overlay labels: whole numbers print bare (matches how these show up in
// Datadog's own UI for count-shaped metrics), fractional ones get 2 decimal
// places.
static String formatChartValue(double v) {
    if (v == (double)(long)v) return String((long)v);
    return String(v, 2);
}

// "Now" / "Xs ago" / "Xm ago" / "Xh ago" — used for the chart's x-axis
// reference labels (see updateChartAxisLabels()), not per-point display.
static String formatAgeLabel(long long ageSec) {
    if (ageSec <= 0) return "Now";
    if (ageSec < 60) return String(ageSec) + "s ago";
    long long mins = ageSec / 60;
    if (mins < 60) return String(mins) + "m ago";
    return String(mins / 60) + "h ago";
}

// Shared by showMonitorDetail() (defaults to the latest point) and the
// tap-to-inspect handler on s_detailChart (see buildDetailScreenIfNeeded) —
// updates the value label and snaps the vertical cursor line to the given
// point, so there's always a clear visual answer to "what data point am I
// looking at", not just a bare number.
static void updateChartHighlight(int idx) {
    if (idx < 0 || idx >= (int)s_detailChartPoints.size()) return;
    const dd::MetricPoint& p = s_detailChartPoints[idx];
    lv_label_set_text(s_detailCurrentLbl, formatChartValue(p.value).c_str());
    if (s_detailCursor) lv_chart_set_cursor_point(s_detailChart, s_detailCursor, s_detailSeries, (uint16_t)idx);
}

// Sets the 3 x-axis reference labels ("Now" at the right edge through the
// oldest plotted point at the left edge) from the actual time span of
// `points` — called once per showMonitorDetail(), not per tap (the axis
// itself doesn't move as you inspect different points, only the cursor
// does). Falls back to hiding the labels (leaving the tick marks, still
// useful as pure visual dividers) if timestamps aren't usable — the log-
// chart path's ISO8601 parser or the metric path's raw pointlist could both
// plausibly return a 0 in some edge case, and "Now" through "0h ago" would
// be actively misleading, not just uninformative.
static void updateChartAxisLabels() {
    if (s_detailChartPoints.size() < 2) {
        for (int i = 0; i < DETAIL_AXIS_TICK_COUNT; ++i) if (s_detailAxisLbl[i]) lv_label_set_text(s_detailAxisLbl[i], "");
        return;
    }
    uint32_t oldestTs = s_detailChartPoints.front().tsSec;
    uint32_t newestTs = s_detailChartPoints.back().tsSec;
    if (oldestTs == 0 || newestTs == 0 || newestTs <= oldestTs) {
        for (int i = 0; i < DETAIL_AXIS_TICK_COUNT; ++i) if (s_detailAxisLbl[i]) lv_label_set_text(s_detailAxisLbl[i], "");
        return;
    }
    long long spanSec = (long long)newestTs - (long long)oldestTs;
    lv_label_set_text(s_detailAxisLbl[0], formatAgeLabel(spanSec).c_str());
    lv_label_set_text(s_detailAxisLbl[1], formatAgeLabel(spanSec / 2).c_str());
    lv_label_set_text(s_detailAxisLbl[2], formatAgeLabel(0).c_str());   // "Now" — the rightmost/most recent point
}

void ui::notifyMonitorCountsRefreshed() {
    const dd::MonitorCounts& c = dd::lastMonitorCounts();
    if (s_ovMonitorBadge) {
        int notOk = c.alert + c.warn + c.noData;
        lv_label_set_text(s_ovMonitorBadge, notOk > 0 ? formatCount(notOk).c_str() : "");
        lv_obj_set_style_text_color(s_ovMonitorBadge, c.alert > 0 ? COLOR_ALERT : COLOR_WARN, 0);
    }
    if (s_ovLastPoll) {
        String txt = c.fetchOk ? "Updated just now" : ("Poll failed: " + c.error);
        lv_label_set_text(s_ovLastPoll, txt.c_str());
        lv_obj_set_style_text_color(s_ovLastPoll, c.fetchOk ? COLOR_MUTED : COLOR_ALERT, 0);
    }
}

// ---- Shared table row (Monitors/Incidents/On-Call/SLOs) ----
// Single-line row: optional status dot, title (flex-grows to fill whatever
// width is left), meta text right of it at its own natural width. Replaces
// the old 44px bordered/backgrounded "card" per row (DESIGN.md's previous
// "List-row card" style) — a hairline bottom border separates rows instead
// of a boxed background, and rows are dense enough that ~7 fit in the space
// ~4 used to. Caller attaches LV_OBJ_FLAG_CLICKABLE + user_data + a tap
// handler on the returned row for screens that navigate on tap.
#define TABLE_ROW_H 26

static lv_obj_t* buildTableRow(lv_obj_t* parent, bool showDot, lv_color_t dotColor,
                                const String& title, const String& meta) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), TABLE_ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COLOR_BORDER, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (showDot) {
        lv_obj_t* dot = lv_obj_create(row);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, dotColor, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_right(dot, 6, 0);
    }

    lv_obj_t* titleLbl = lv_label_create(row);
    lv_obj_set_style_text_font(titleLbl, &outfit_bold_12, 0);
    lv_obj_set_style_text_color(titleLbl, COLOR_INK, 0);
    // Plain truncation (LV_LABEL_LONG_DOT), not the LV_LABEL_LONG_SCROLL_CIRCULAR
    // marquee this used to be — reverted after two live-confirmed LVGL pool
    // (LV_MEM_SIZE, 64KB) exhaustion crashes. Each marquee label carried two
    // extra local styles (anim_speed + anim), which on this LVGL version means
    // a style-array realloc per label (get_local_style()/lv_obj_style.c); with
    // six dashboard screens' worth of rows kept resident at once (see this
    // file's DASH_COUNT + lv_conf.h's LV_MEM_SIZE comment on the pool already
    // being at its measured ceiling), that was consistently enough to tip the
    // pool over during ordinary navigation. Long titles lose the tail to "…"
    // instead of scrolling — a real but minor UX downgrade, not a bug.
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(titleLbl, 1);   // absorb remaining width after dot + meta
    lv_label_set_text(titleLbl, title.c_str());

    lv_obj_t* metaLbl = lv_label_create(row);
    lv_obj_set_style_text_font(metaLbl, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(metaLbl, COLOR_MUTED, 0);
    lv_obj_set_style_pad_left(metaLbl, 8, 0);
    lv_label_set_text(metaLbl, meta.c_str());

    return row;
}

// ---- Monitors ----

static lv_color_t statusColor(const String& status) {
    if (status.equalsIgnoreCase("Alert")) return COLOR_ALERT;
    if (status.equalsIgnoreCase("Warn"))  return COLOR_WARN;
    if (status.equalsIgnoreCase("OK"))    return COLOR_OK;
    return COLOR_NODATA;
}

static void requestMonitorsFetch(const char* ddFilter) {
    dd::setMonitorFilter(ddFilter);
    portENTER_CRITICAL(&s_pendingMux);
    s_monitorsFetchPending = true;
    portEXIT_CRITICAL(&s_pendingMux);
}

static void onChipClicked(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    s_monActiveFilter = idx;
    for (int i = 0; i < MON_FILTER_COUNT; ++i) {
        lv_obj_set_style_bg_color(s_monChips[i], (i == idx) ? COLOR_PURPLE : COLOR_SURFACE, 0);
    }
    static const char* const DD_FILTERS[] = { "", "alert", "warn", "no data" };
    requestMonitorsFetch(DD_FILTERS[idx]);
}

static void buildMonitors() {
    lv_obj_t* s = s_dashScr[DASH_MONITORS];

    // Filter chip row: 28px tall pills, horizontal-scrollable if overflow (DESIGN.md).
    lv_obj_t* chipRow = lv_obj_create(s);
    lv_obj_set_size(chipRow, SCREEN_WIDTH, 32);
    lv_obj_set_pos(chipRow, 0, 26);
    lv_obj_set_style_bg_opa(chipRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chipRow, 0, 0);
    lv_obj_set_style_pad_all(chipRow, 4, 0);
    lv_obj_set_style_pad_column(chipRow, 6, 0);
    lv_obj_set_flex_flow(chipRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(chipRow, LV_DIR_HOR);
    lv_obj_clear_flag(chipRow, LV_OBJ_FLAG_SCROLL_ELASTIC);

    for (int i = 0; i < MON_FILTER_COUNT; ++i) {
        lv_obj_t* chip = lv_btn_create(chipRow);
        lv_obj_set_height(chip, 24);
        lv_obj_set_style_radius(chip, 12, 0);
        lv_obj_set_style_bg_color(chip, (i == s_monActiveFilter) ? COLOR_PURPLE : COLOR_SURFACE, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_set_user_data(chip, (void*)(intptr_t)i);
        lv_obj_add_event_cb(chip, onChipClicked, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* lbl = lv_label_create(chip);
        lv_obj_set_style_text_font(lbl, &outfit_bold_14, 0);
        lv_obj_set_style_text_color(lbl, COLOR_INK, 0);
        lv_label_set_text(lbl, MON_FILTERS[i]);
        lv_obj_center(lbl);
        s_monChips[i] = chip;
    }

    // List rows: 44px tall, icon (status dot) + single-line truncated title
    // + meta line. Inset horizontally so the nav arrows never overlap content.
    s_monList = lv_list_create(s);
    lv_obj_set_size(s_monList, LIST_WIDTH, SCREEN_HEIGHT - 66 - 24);
    lv_obj_set_pos(s_monList, LIST_INSET, 66);
    lv_obj_set_style_bg_opa(s_monList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_monList, 0, 0);
    lv_obj_set_style_pad_all(s_monList, 0, 0);
    lv_obj_add_flag(s_monList, LV_OBJ_FLAG_GESTURE_BUBBLE);   // let swipe-to-page-over reach the screen

    s_monEmpty = lv_label_create(s_monList);
    lv_obj_set_style_text_font(s_monEmpty, &outfit_thin_14, 0);
    lv_obj_set_style_text_color(s_monEmpty, COLOR_MUTED, 0);
    lv_label_set_text(s_monEmpty, "Loading monitors...");
}

// Relative "Xm ago" / "Xh ago" from a unix-seconds timestamp — cheap enough
// to compute on every list render instead of caching.
static String relativeAge(long unixSec) {
    if (unixSec <= 0) return "";
    long now = (long)time(nullptr);
    if (now < 1700000000) return "";
    long deltaSec = now - unixSec;
    if (deltaSec < 60) return String(deltaSec) + "s ago";
    if (deltaSec < 3600) return String(deltaSec / 60) + "m ago";
    if (deltaSec < 86400) return String(deltaSec / 3600) + "h ago";
    return String(deltaSec / 86400) + "d ago";
}

void ui::notifyMonitorsListRefreshed() {
    if (!s_monList) return;
    lv_obj_clean(s_monList);

    const std::vector<dd::Monitor>& monitors = dd::lastMonitors();
    if (monitors.empty()) {
        s_monEmpty = lv_label_create(s_monList);
        lv_obj_set_style_text_font(s_monEmpty, &outfit_thin_14, 0);
        lv_obj_set_style_text_color(s_monEmpty, COLOR_MUTED, 0);
        lv_label_set_text(s_monEmpty, "No monitors match this filter.");
        return;
    }

    for (size_t i = 0; i < monitors.size(); ++i) {
        const dd::Monitor& m = monitors[i];
        String metaText = m.status + " - " + relativeAge(m.lastTriggeredTs);
        lv_obj_t* row = buildTableRow(s_monList, true, statusColor(m.status), m.name, metaText);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            const std::vector<dd::Monitor>& list = dd::lastMonitors();
            if (idx < 0 || idx >= (int)list.size()) return;
            const dd::Monitor& m = list[idx];
            // Show the detail screen NOW using what the list already has
            // cached (name/status/query), instead of waiting on the two
            // sequential HTTPS fetches below to land before anything moves —
            // that gap read as the screen being unresponsive to the tap.
            // showMonitorDetail() overwrites tags/chart once the real fetch
            // completes, same as Incident Detail already does from its cache.
            ui::showMonitorDetailLoading(m);
            portENTER_CRITICAL(&s_pendingMux);
            s_monitorDetailRequestId = m.id;
            s_monitorDetailRequestPending = true;
            portEXIT_CRITICAL(&s_pendingMux);
        }, LV_EVENT_CLICKED, nullptr);
    }
}

bool ui::monitorsFetchPending(String& outFilter) {
    if (!s_monitorsFetchPending) return false;
    portENTER_CRITICAL(&s_pendingMux);
    bool hit = s_monitorsFetchPending;
    s_monitorsFetchPending = false;
    portEXIT_CRITICAL(&s_pendingMux);
    if (!hit) return false;
    outFilter = dd::getMonitorFilter();
    return true;
}

// ---- Monitor Detail ----
// Not part of the swipe rotation — reached by tapping a Monitors row, and
// returns to the Monitors screen via the back button (DESIGN.md's "back-
// button left, action-button right" detail-screen convention).

bool ui::monitorDetailRequestPending(long& outId) {
    if (!s_monitorDetailRequestPending) return false;
    portENTER_CRITICAL(&s_pendingMux);
    bool hit = s_monitorDetailRequestPending;
    s_monitorDetailRequestPending = false;
    outId = s_monitorDetailRequestId;
    portEXIT_CRITICAL(&s_pendingMux);
    if (!hit) return false;
    return true;
}

bool ui::monitorMutePending(long& outId, uint32_t& outUntilEpochSec) {
    if (!s_monitorMutePending) return false;
    s_monitorMutePending = false;
    outId = s_monitorMuteId;
    outUntilEpochSec = s_monitorMuteUntilEpochSec;
    return true;
}

static void buildDetailScreenIfNeeded() {
    if (s_detailScr) return;
    logMemCheckpoint("before Monitor Detail");
    s_detailScr = lv_obj_create(nullptr);
    styleFullscreen(s_detailScr);

    lv_obj_t* back = lv_btn_create(s_detailScr);
    lv_obj_set_size(back, 44, 28);
    lv_obj_set_pos(back, 8, 8);
    lv_obj_set_style_bg_color(back, COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        if (s_dashScr[DASH_MONITORS]) lv_scr_load_anim(s_dashScr[DASH_MONITORS], LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
        s_screen = ui::Screen::Monitors;
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLbl = lv_label_create(back);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLbl, COLOR_INK, 0);
    lv_obj_center(backLbl);

    // Every zone below has a fixed height reserved up front, and the next
    // zone starts after it — nothing overlaps regardless of how many lines
    // a given monitor's name/tags/query actually need. The previous version
    // used absolute y-positions sized for the common case, so a long name
    // wrapping to 2 lines pushed straight through the tags line below it.

    // Title + status share one row: title marquee-scrolls in place if it's
    // too long for its box instead of wrapping to a 2nd line (LVGL's
    // circular-scroll long-mode only kicks in when content overflows —
    // short titles just sit still), and status is pinned to the row's far
    // right instead of stacking below. Tags are gone entirely — the chart
    // and query below are the useful part of this screen; tags were mostly
    // just eating vertical space.
    s_detailName = lv_label_create(s_detailScr);
    lv_obj_set_style_text_font(s_detailName, &outfit_bold_16, 0);
    lv_obj_set_style_text_color(s_detailName, COLOR_INK, 0);
    lv_obj_set_size(s_detailName, SCREEN_WIDTH - 24 - DETAIL_STATUS_W - 6, 22);
    lv_label_set_long_mode(s_detailName, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(s_detailName, 12, 36);

    s_detailStatus = lv_label_create(s_detailScr);
    lv_obj_set_style_text_font(s_detailStatus, &outfit_bold_14, 0);
    lv_obj_set_size(s_detailStatus, DETAIL_STATUS_W, 22);
    lv_obj_set_style_text_align(s_detailStatus, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_detailStatus, SCREEN_WIDTH - 12 - DETAIL_STATUS_W, 36);

    // Chart grew from 70px to 100px tall — with tags gone and the action
    // bar now pinned near the bottom of the screen (see ACTION_BAR_Y)
    // instead of sitting right under the query line, there was real
    // unused vertical space in the middle of this screen; the chart is
    // the actual informational content here, so it gets that space.
    s_detailChart = lv_chart_create(s_detailScr);
    lv_obj_set_size(s_detailChart, SCREEN_WIDTH - 24, 100);
    lv_obj_set_pos(s_detailChart, 12, 64);
    lv_chart_set_type(s_detailChart, LV_CHART_TYPE_LINE);
    lv_obj_set_style_bg_color(s_detailChart, COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(s_detailChart, COLOR_BORDER, 0);
    lv_chart_set_div_line_count(s_detailChart, 3, 0);
    // Created once, here, and reused for every monitor viewed this boot —
    // showMonitorDetail() only ever updates its point count/values in place.
    // Previously it called lv_chart_remove_series()+lv_chart_add_series() on
    // every view; that call reliably hung starting on the 2nd monitor opened
    // (LVGL 8.4.0's TLSF-backed lv_mem_free() never returned, even with a
    // healthy, unfragmented pool per lv_mem_monitor() — root cause not fully
    // chased down, but avoiding the call entirely sidesteps it).
    s_detailSeries = lv_chart_add_series(s_detailChart, COLOR_PURPLE, LV_CHART_AXIS_PRIMARY_Y);
    // Vertical cursor line — the only way to show "which point" the value/
    // info labels refer to; lv_chart has no built-in tooltip in LVGL 8.4.
    // Snapped to a point via updateChartHighlight(), never removed/re-added
    // (see the comment above about why churning chart objects is dangerous
    // on this LVGL version).
    s_detailCursor = lv_chart_add_cursor(s_detailChart, COLOR_INK, LV_DIR_VER);
    // Tap-to-inspect: LV_EVENT_VALUE_CHANGED fires whenever the pressed
    // point changes (including back to LV_CHART_POINT_NONE on release), and
    // lv_chart_get_pressed_point() gives its index — both confirmed against
    // the installed lv_chart.c (LVGL 8.4 has no built-in tooltip, just this
    // event + getter). Reverts to highlighting the latest point on release,
    // so the cursor/value/info line always has an answer, not just while
    // actively touching the chart.
    lv_obj_add_event_cb(s_detailChart, [](lv_event_t* e) {
        lv_obj_t* chart = lv_event_get_target(e);
        uint32_t idx = lv_chart_get_pressed_point(chart);
        if (idx != LV_CHART_POINT_NONE) {
            updateChartHighlight((int)idx);
        } else if (!s_detailChartPoints.empty()) {
            updateChartHighlight((int)s_detailChartPoints.size() - 1);
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // Y-axis bounds + latest value — the chart used to be just lines and
    // dots with no indication of actual scale. All three sit on top of the
    // chart (children of s_detailScr, not s_detailChart, so they aren't
    // affected by the chart's own point/series churn) and get repositioned/
    // retexted in showMonitorDetail() each time the range changes.
    s_detailChartHiLbl = lv_label_create(s_detailScr);
    lv_obj_set_style_text_font(s_detailChartHiLbl, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_detailChartHiLbl, COLOR_MUTED, 0);
    lv_obj_set_pos(s_detailChartHiLbl, 16, 66);

    s_detailChartLoLbl = lv_label_create(s_detailScr);
    lv_obj_set_style_text_font(s_detailChartLoLbl, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_detailChartLoLbl, COLOR_MUTED, 0);
    lv_obj_set_pos(s_detailChartLoLbl, 16, 64 + 100 - 16);

    s_detailCurrentLbl = lv_label_create(s_detailScr);
    lv_obj_set_style_text_font(s_detailCurrentLbl, &outfit_bold_12, 0);
    lv_obj_set_style_text_color(s_detailCurrentLbl, COLOR_INK, 0);
    lv_obj_set_size(s_detailCurrentLbl, SCREEN_WIDTH - 24 - 16, 16);
    lv_obj_set_style_text_align(s_detailCurrentLbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_detailCurrentLbl, 12, 66);

    // Threshold reference lines — thin bars, not part of the chart widget
    // (lv_chart has no native reference-line primitive in LVGL 8.x), laid
    // over it in the same coordinate space (both are direct children of
    // s_detailScr at the chart's own x/12..SCREEN_WIDTH-12, y 64..164).
    // Hidden by default; only shown when thresholdsApplicable and the
    // threshold value falls within the current chart's y-range (see
    // showMonitorDetail() — a query wrapped in anomalies()/outliers()/etc.
    // compares a deviation band, not the plotted value, against this
    // threshold, so drawing it here would misrepresent what it means).
    s_detailCritLine = lv_obj_create(s_detailScr);
    lv_obj_set_size(s_detailCritLine, SCREEN_WIDTH - 24, 2);
    lv_obj_set_style_bg_color(s_detailCritLine, COLOR_ALERT, 0);
    lv_obj_set_style_bg_opa(s_detailCritLine, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_detailCritLine, 0, 0);
    lv_obj_clear_flag(s_detailCritLine, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detailCritLine, LV_OBJ_FLAG_HIDDEN);

    s_detailWarnLine = lv_obj_create(s_detailScr);
    lv_obj_set_size(s_detailWarnLine, SCREEN_WIDTH - 24, 2);
    lv_obj_set_style_bg_color(s_detailWarnLine, COLOR_WARN, 0);
    lv_obj_set_style_bg_opa(s_detailWarnLine, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_detailWarnLine, 0, 0);
    lv_obj_clear_flag(s_detailWarnLine, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detailWarnLine, LV_OBJ_FLAG_HIDDEN);

    s_detailNoChart = lv_label_create(s_detailScr);
    lv_obj_set_style_text_font(s_detailNoChart, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_detailNoChart, COLOR_MUTED, 0);
    lv_obj_set_size(s_detailNoChart, SCREEN_WIDTH - 24, 100);
    lv_obj_set_pos(s_detailNoChart, 12, 64);
    lv_label_set_long_mode(s_detailNoChart, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_detailNoChart, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_detailNoChart, LV_OBJ_FLAG_HIDDEN);

    // X-axis reference ticks — replaces the old full raw-query row (near-
    // unreadable truncated to one line, and told you nothing about *when*).
    // Text/positions are static per-build (left/mid edge x, right edge x);
    // only the label strings change per monitor, set in
    // updateChartAxisLabels() from the actual plotted time span.
    {
        const int chartX0 = 12, chartX1 = SCREEN_WIDTH - 12, chartBottom = 64 + 100;
        const int tickX[DETAIL_AXIS_TICK_COUNT] = { chartX0, chartX0 + (chartX1 - chartX0) / 2, chartX1 };
        const lv_text_align_t lblAlign[DETAIL_AXIS_TICK_COUNT] = {
            LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT
        };
        for (int i = 0; i < DETAIL_AXIS_TICK_COUNT; ++i) {
            lv_obj_t* tick = lv_obj_create(s_detailScr);
            lv_obj_set_size(tick, 1, 5);
            lv_obj_set_style_bg_color(tick, COLOR_MUTED, 0);
            lv_obj_set_style_border_width(tick, 0, 0);
            lv_obj_clear_flag(tick, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(tick, tickX[i], chartBottom);
            s_detailAxisTick[i] = tick;

            lv_obj_t* lbl = lv_label_create(s_detailScr);
            lv_obj_set_style_text_font(lbl, &outfit_thin_12, 0);
            lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
            lv_obj_set_size(lbl, 70, 14);
            lv_obj_set_style_text_align(lbl, lblAlign[i], 0);
            int lblX = (i == 0) ? chartX0 : (i == 1) ? (tickX[1] - 35) : (chartX1 - 70);
            lv_obj_set_pos(lbl, lblX, chartBottom + 6);
            s_detailAxisLbl[i] = lbl;
        }
    }

    // Action bar: 3 equal slots — Mute/Unmute (state-aware), Declare, Bits.
    // All navigate to a confirm/options screen on tap (the navigation itself
    // is the "are you sure" gate) except Unmute, which long-presses straight
    // through since re-enabling alerting is the safe direction.
    lv_obj_t* muteBtn = lv_btn_create(s_detailScr);
    lv_obj_set_size(muteBtn, ACTION_BTN_W, ACTION_BTN_H);
    lv_obj_set_pos(muteBtn, 12, ACTION_BAR_Y);
    lv_obj_set_style_bg_color(muteBtn, COLOR_PURPLE, 0);
    lv_obj_set_style_border_width(muteBtn, 0, 0);
    // Buttons default to ~5-8px of theme padding on every side — with
    // children aligned relative to that padded content box rather than
    // the button's full outer box, that default padding was throwing the
    // icon/label positions off. Zero it explicitly.
    lv_obj_set_style_pad_all(muteBtn, 0, 0);
    lv_obj_clear_flag(muteBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(muteBtn, [](lv_event_t*) {
        if (s_detailMonitorMuted) return;   // tap does nothing while showing "Unmute" — long-press below handles it
        showMuteOptionsScreen();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(muteBtn, [](lv_event_t*) {
        if (!s_detailMonitorMuted) return;  // long-press only unmutes; muting always goes through the options screen
        s_monitorMuteId = s_detailMonitorId;
        s_monitorMuteUntilEpochSec = 0;     // sentinel: unmute
        s_monitorMutePending = true;
    }, LV_EVENT_LONG_PRESSED, nullptr);
    // Icon left-aligned, label beside it — not stacked icon-over-label —
    // now that the action bar has a full 32px-tall row to itself at the
    // bottom of the screen instead of squeezing both into a shared line.
    lv_obj_t* muteIcon = lv_label_create(muteBtn);
    lv_obj_set_style_text_font(muteIcon, &outfit_bold_16, 0);
    lv_obj_align(muteIcon, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t* muteLbl = lv_label_create(muteBtn);
    lv_obj_set_style_text_font(muteLbl, &outfit_bold_12, 0);
    lv_obj_align(muteLbl, LV_ALIGN_LEFT_MID, 32, 0);
    s_detailMuteBtn = muteBtn;
    s_detailMuteIcon = muteIcon;
    s_detailMuteLbl = muteLbl;

    lv_obj_t* declareBtn = lv_btn_create(s_detailScr);
    lv_obj_set_size(declareBtn, ACTION_BTN_W, ACTION_BTN_H);
    lv_obj_set_pos(declareBtn, 12 + ACTION_BTN_W + ACTION_BTN_GAP, ACTION_BAR_Y);
    lv_obj_set_style_bg_color(declareBtn, COLOR_PURPLE, 0);
    lv_obj_set_style_border_width(declareBtn, 0, 0);
    lv_obj_set_style_pad_all(declareBtn, 0, 0);
    lv_obj_clear_flag(declareBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(declareBtn, [](lv_event_t*) { showDeclareOptionsScreen(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* declareIcon = lv_label_create(declareBtn);
    lv_obj_set_style_text_font(declareIcon, &outfit_bold_16, 0);
    // UPLOAD is a placeholder — no built-in LVGL symbol reads as "declare/
    // announce" (no trumpet/megaphone glyph exists in this font); swap for
    // custom pixel art via tools/png_to_lvgl.py once one's designed.
    lv_label_set_text(declareIcon, LV_SYMBOL_UPLOAD);
    lv_obj_align(declareIcon, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t* declareLbl = lv_label_create(declareBtn);
    lv_obj_set_style_text_font(declareLbl, &outfit_bold_12, 0);
    lv_label_set_text(declareLbl, "Declare");
    lv_obj_align(declareLbl, LV_ALIGN_LEFT_MID, 32, 0);

    lv_obj_t* bitsBtn = lv_btn_create(s_detailScr);
    lv_obj_set_size(bitsBtn, ACTION_BTN_W, ACTION_BTN_H);
    lv_obj_set_pos(bitsBtn, 12 + 2 * (ACTION_BTN_W + ACTION_BTN_GAP), ACTION_BAR_Y);
    lv_obj_set_style_bg_color(bitsBtn, COLOR_PURPLE, 0);
    lv_obj_set_style_border_width(bitsBtn, 0, 0);
    lv_obj_set_style_pad_all(bitsBtn, 0, 0);
    lv_obj_clear_flag(bitsBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bitsBtn, [](lv_event_t*) { showBitsConfirmScreen(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* bitsIcon = lv_img_create(bitsBtn);
    lv_img_set_src(bitsIcon, &bits_icon_small);
    lv_obj_align(bitsIcon, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t* bitsLbl = lv_label_create(bitsBtn);
    lv_obj_set_style_text_font(bitsLbl, &outfit_bold_12, 0);
    lv_label_set_text(bitsLbl, "Bits");
    lv_obj_align(bitsLbl, LV_ALIGN_LEFT_MID, 32, 0);

    // Above the action bar now, not below it — the buttons sit at the
    // bottom of the screen (see ACTION_BAR_Y), so there's no room left
    // under them.
    // Bold, not thin — this is the only feedback the user gets after Mute/
    // Declare/Bits actions (all of which navigate straight back to this
    // screen optimistically, before their async API call even finishes; see
    // backToDetail()'s doc comment), and the thin 12px muted-color text this
    // used to be was easy to miss entirely, especially on this panel's
    // documented dim/off-axis legibility issues. Explicit width + LONG_DOT
    // truncation instead of wrap — ACTION_BAR_Y leaves too little vertical
    // room below this line to wrap a long message without overlapping it.
    s_detailMuteResult = lv_label_create(s_detailScr);
    lv_obj_set_style_text_font(s_detailMuteResult, &outfit_bold_12, 0);
    lv_obj_set_size(s_detailMuteResult, SCREEN_WIDTH - 24, 16);
    lv_label_set_long_mode(s_detailMuteResult, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_detailMuteResult, 12, 187);
    logMemCheckpoint("after Monitor Detail");
}

// Swaps the Mute-slot's icon/text/color between "Mute" (unmuted — tap opens
// the 1h/Today options screen) and "Unmute" (muted — long-press executes
// directly). One button object re-labeled, not two objects toggled visible/
// hidden, since exactly one of these is ever the valid action.
static void updateMuteActionBar(bool muted) {
    if (!s_detailMuteBtn) return;
    s_detailMonitorMuted = muted;
    if (muted) {
        lv_label_set_text(s_detailMuteIcon, LV_SYMBOL_VOLUME_MAX);
        lv_label_set_text(s_detailMuteLbl, "HOLD:\nUnmute");
        lv_obj_set_style_bg_color(s_detailMuteBtn, COLOR_SURFACE, 0);
        lv_obj_set_style_border_color(s_detailMuteBtn, COLOR_PURPLE, 0);
        lv_obj_set_style_border_width(s_detailMuteBtn, 1, 0);
    } else {
        lv_label_set_text(s_detailMuteIcon, LV_SYMBOL_MUTE);
        lv_label_set_text(s_detailMuteLbl, "Mute");
        lv_obj_set_style_bg_color(s_detailMuteBtn, COLOR_PURPLE, 0);
        lv_obj_set_style_border_width(s_detailMuteBtn, 0, 0);
    }
}

// ---- Monitor Detail action-bar confirm/options screens ----
// Shared shape across Mute/Declare/Bits: back button, title, a stack of
// full-width option buttons, done. All of them navigate straight back to
// Monitor Detail the moment the user makes a final choice (optimistic —
// the actual API call finishes async, same pattern Mute already used) and
// the result shows up in s_detailMuteResult back on that screen, so none of
// these need their own result label.

static void backToDetail() {
    if (s_detailScr) lv_scr_load_anim(s_detailScr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
    s_screen = ui::Screen::MonitorDetail;
}

static lv_obj_t* buildOptionScreen(const char* title) {
    lv_obj_t* scr = lv_obj_create(nullptr);
    styleFullscreen(scr);

    lv_obj_t* back = lv_btn_create(scr);
    lv_obj_set_size(back, 44, 28);
    lv_obj_set_pos(back, 8, 8);
    lv_obj_set_style_bg_color(back, COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, [](lv_event_t*) { backToDetail(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLbl = lv_label_create(back);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLbl, COLOR_INK, 0);
    lv_obj_center(backLbl);

    lv_obj_t* titleLbl = lv_label_create(scr);
    lv_obj_set_style_text_font(titleLbl, &outfit_bold_18, 0);
    lv_obj_set_style_text_color(titleLbl, COLOR_INK, 0);
    lv_label_set_text(titleLbl, title);
    lv_obj_set_pos(titleLbl, 60, 14);

    return scr;
}

static lv_obj_t* addOptionButton(lv_obj_t* scr, int y, const char* label) {
    lv_obj_t* btn = lv_btn_create(scr);
    lv_obj_set_size(btn, SCREEN_WIDTH - 24, 40);
    lv_obj_set_pos(btn, 12, y);
    lv_obj_set_style_bg_color(btn, COLOR_PURPLE, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &outfit_bold_16, 0);
    lv_label_set_text(lbl, label);
    lv_obj_center(lbl);
    return btn;
}

// -- Mute: "1 Hour" / "Today" --
static lv_obj_t* s_muteOptScr = nullptr;

static void buildMuteOptionsScreenIfNeeded() {
    if (s_muteOptScr) return;
    s_muteOptScr = buildOptionScreen("Mute for how long?");

    lv_obj_t* oneHourBtn = addOptionButton(s_muteOptScr, 60, "1 Hour");
    lv_obj_add_event_cb(oneHourBtn, [](lv_event_t*) {
        s_monitorMuteId = s_detailMonitorId;
        s_monitorMuteUntilEpochSec = (uint32_t)time(nullptr) + 3600;
        s_monitorMutePending = true;
        backToDetail();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* todayBtn = addOptionButton(s_muteOptScr, 110, "Today");
    lv_obj_add_event_cb(todayBtn, [](lv_event_t*) {
        s_monitorMuteId = s_detailMonitorId;
        s_monitorMuteUntilEpochSec = (uint32_t)time(nullptr) + 86400;
        s_monitorMutePending = true;
        backToDetail();
    }, LV_EVENT_CLICKED, nullptr);
}

static void showMuteOptionsScreen() {
    buildMuteOptionsScreenIfNeeded();
    lv_scr_load_anim(s_muteOptScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

// -- Bits: single "Trigger Investigation" confirm --
static volatile bool s_bitsTriggerPending = false;
static long           s_bitsTriggerMonitorId = 0;
static lv_obj_t* s_bitsConfirmScr = nullptr;

static void buildBitsConfirmScreenIfNeeded() {
    if (s_bitsConfirmScr) return;
    s_bitsConfirmScr = buildOptionScreen("Bits AI Investigation");

    lv_obj_t* note = lv_label_create(s_bitsConfirmScr);
    lv_obj_set_style_text_font(note, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(note, COLOR_MUTED, 0);
    lv_obj_set_size(note, SCREEN_WIDTH - 24, 40);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(note, "Investigates this monitor's most recent alert event.");
    lv_obj_set_pos(note, 12, 50);

    lv_obj_t* btn = addOptionButton(s_bitsConfirmScr, 100, "Trigger Investigation");
    lv_obj_add_event_cb(btn, [](lv_event_t*) {
        s_bitsTriggerMonitorId = s_detailMonitorId;
        s_bitsTriggerPending = true;
        backToDetail();
    }, LV_EVENT_CLICKED, nullptr);
}

static void showBitsConfirmScreen() {
    buildBitsConfirmScreenIfNeeded();
    lv_scr_load_anim(s_bitsConfirmScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

// -- Declare > Incident: single "Create Incident" confirm --
// Title is computed lazily from s_detailName/s_detailStatus's own label
// text (lv_label_get_text(), not a separate cached String) in
// ui::incidentCreatePending() — those labels hold steady between the tap
// and main.cpp draining the flag on the next loop() iteration, and DRAM
// here is tight enough that trimming a
// String is worth it (see the build note where this was added).
static volatile bool s_incidentCreatePending = false;
static lv_obj_t* s_incidentConfirmScr = nullptr;

static void buildIncidentConfirmScreenIfNeeded() {
    if (s_incidentConfirmScr) return;
    s_incidentConfirmScr = buildOptionScreen("Declare Incident");

    lv_obj_t* btn = addOptionButton(s_incidentConfirmScr, 100, "Create Incident");
    lv_obj_add_event_cb(btn, [](lv_event_t*) {
        s_incidentCreatePending = true;
        backToDetail();
    }, LV_EVENT_CLICKED, nullptr);
}

static void showIncidentConfirmScreen() {
    buildIncidentConfirmScreenIfNeeded();
    lv_scr_load_anim(s_incidentConfirmScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

// -- Declare > Case: pick a project (list is org-specific, fetched live) --
static volatile bool s_caseProjectsFetchPending = false;
static volatile bool s_caseCreatePending = false;
static String         s_caseCreateProjectId;
static lv_obj_t* s_caseProjectScr = nullptr;
static lv_obj_t* s_caseProjectList = nullptr;

static void buildCaseProjectScreenIfNeeded() {
    if (s_caseProjectScr) return;
    s_caseProjectScr = buildOptionScreen("Pick a project");

    s_caseProjectList = lv_list_create(s_caseProjectScr);
    lv_obj_set_size(s_caseProjectList, SCREEN_WIDTH - 24, SCREEN_HEIGHT - 60);
    lv_obj_set_pos(s_caseProjectList, 12, 50);
    lv_obj_set_style_bg_opa(s_caseProjectList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_caseProjectList, 0, 0);
    lv_obj_set_style_pad_all(s_caseProjectList, 0, 0);

    lv_obj_t* loading = lv_label_create(s_caseProjectList);
    lv_obj_set_style_text_font(loading, &outfit_thin_14, 0);
    lv_obj_set_style_text_color(loading, COLOR_MUTED, 0);
    lv_label_set_text(loading, "Loading projects...");
}

static void showCaseProjectScreen() {
    buildCaseProjectScreenIfNeeded();
    s_caseProjectsFetchPending = true;
    lv_scr_load_anim(s_caseProjectScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

bool ui::caseProjectsFetchPending() {
    if (!s_caseProjectsFetchPending) return false;
    s_caseProjectsFetchPending = false;
    return true;
}

void ui::notifyCaseProjectsRefreshed(const std::vector<dd::CaseProject>& projects) {
    if (!s_caseProjectList) return;
    lv_obj_clean(s_caseProjectList);

    if (projects.empty()) {
        lv_obj_t* empty = lv_label_create(s_caseProjectList);
        lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
        lv_label_set_text(empty, "No case projects available.");
        return;
    }

    for (size_t i = 0; i < projects.size(); ++i) {
        lv_obj_t* btn = lv_btn_create(s_caseProjectList);
        lv_obj_set_size(btn, LV_PCT(100), 44);
        lv_obj_set_style_bg_color(btn, COLOR_SURFACE, 0);
        lv_obj_set_style_border_color(btn, COLOR_BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        // Stash the project id directly on the button (not just an index) —
        // it's a String, too big for user_data's void*, so heap-allocate a
        // copy and free it on delete, mirroring the row-cache pattern used
        // for the Monitors list's tap-to-open-detail data.
        lv_obj_set_user_data(btn, (void*)new String(projects[i].id));
        lv_obj_add_event_cb(btn, [](lv_event_t* e) { delete (String*)lv_obj_get_user_data(lv_event_get_target(e)); }, LV_EVENT_DELETE, nullptr);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            String* projectId = (String*)lv_obj_get_user_data(lv_event_get_target(e));
            s_caseCreateProjectId = *projectId;
            s_caseCreatePending = true;
            backToDetail();
        }, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &outfit_bold_16, 0);
        lv_obj_set_style_text_color(lbl, COLOR_INK, 0);
        lv_label_set_text(lbl, projects[i].name.c_str());
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    }
}

bool ui::caseCreatePending(String& outProjectId, String& outTitle) {
    if (!s_caseCreatePending) return false;
    s_caseCreatePending = false;
    outProjectId = s_caseCreateProjectId;
    outTitle = declareTitleFor(lv_label_get_text(s_detailName), lv_label_get_text(s_detailStatus));
    return true;
}

bool ui::incidentCreatePending(String& outTitle) {
    if (!s_incidentCreatePending) return false;
    s_incidentCreatePending = false;
    outTitle = declareTitleFor(lv_label_get_text(s_detailName), lv_label_get_text(s_detailStatus));
    return true;
}

bool ui::bitsTriggerPending(long& outMonitorId) {
    if (!s_bitsTriggerPending) return false;
    s_bitsTriggerPending = false;
    outMonitorId = s_bitsTriggerMonitorId;
    return true;
}

void ui::applyDeclareResult(bool ok, const String& msg) {
    ui::applyMuteResult(ok, msg);   // same shared label on Monitor Detail — see backToDetail()'s comment
}

void ui::applyBitsResult(bool ok, const String& msg) {
    ui::applyMuteResult(ok, msg);   // same shared label — see applyDeclareResult()
}

// -- Declare: "Case" / "Incident" --
static lv_obj_t* s_declareOptScr = nullptr;

static void buildDeclareOptionsScreenIfNeeded() {
    if (s_declareOptScr) return;
    s_declareOptScr = buildOptionScreen("Declare");

    lv_obj_t* caseBtn = addOptionButton(s_declareOptScr, 60, "Case");
    lv_obj_add_event_cb(caseBtn, [](lv_event_t*) { showCaseProjectScreen(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* incBtn = addOptionButton(s_declareOptScr, 110, "Incident");
    lv_obj_add_event_cb(incBtn, [](lv_event_t*) { showIncidentConfirmScreen(); }, LV_EVENT_CLICKED, nullptr);
}

static void showDeclareOptionsScreen() {
    buildDeclareOptionsScreenIfNeeded();
    lv_scr_load_anim(s_declareOptScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void ui::showMonitorDetailLoading(const dd::Monitor& cached) {
    buildDetailScreenIfNeeded();
    s_detailMonitorId = cached.id;

    lv_label_set_text(s_detailName, cached.name.c_str());
    lv_label_set_text(s_detailStatus, cached.status.c_str());
    lv_obj_set_style_text_color(s_detailStatus, statusColor(cached.status), 0);
    lv_label_set_text(s_detailMuteResult, "");
    updateMuteActionBar(cached.muted);

    lv_obj_add_flag(s_detailChart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_detailNoChart, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_detailNoChart, "Loading chart...");

    s_screen = ui::Screen::MonitorDetail;
    lv_scr_load_anim(s_detailScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void ui::showMonitorDetail(const dd::Monitor& detail, const std::vector<dd::MetricPoint>& sparkline,
                           bool sparklineOk, const String& chartError) {
    buildDetailScreenIfNeeded();
    s_detailMonitorId = detail.id;

    lv_label_set_text(s_detailName, detail.name.c_str());
    lv_label_set_text(s_detailStatus, detail.status.c_str());
    lv_obj_set_style_text_color(s_detailStatus, statusColor(detail.status), 0);
    lv_label_set_text(s_detailMuteResult, "");
    updateMuteActionBar(detail.muted);

    if (sparklineOk && sparkline.size() >= 2) {
        lv_obj_clear_flag(s_detailChart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_detailNoChart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_detailChartHiLbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_detailChartLoLbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_detailCurrentLbl, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < DETAIL_AXIS_TICK_COUNT; ++i) {
            lv_obj_clear_flag(s_detailAxisTick[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_detailAxisLbl[i], LV_OBJ_FLAG_HIDDEN);
        }

        // Decimate to ~40 points (BARKBOARD_PLAN.md §4) to keep the LVGL
        // pool happy regardless of how many raw points came back. Keeps the
        // full MetricPoint (value + tsSec), not just the value, so tap-to-
        // inspect can show a real timestamp per point.
        const int MAX_POINTS = 40;
        int n = (int)sparkline.size();
        int stride = (n > MAX_POINTS) ? (n / MAX_POINTS) : 1;
        std::vector<dd::MetricPoint> decimated;
        for (int i = 0; i < n; i += stride) decimated.push_back(sparkline[i]);

        double lo = decimated[0].value, hi = decimated[0].value;
        for (const dd::MetricPoint& p : decimated) { if (p.value < lo) lo = p.value; if (p.value > hi) hi = p.value; }
        // Widen the range slightly to fit critical/warning threshold lines
        // that sit just outside the data's own min/max, so they aren't
        // clipped flush against the chart's top/bottom edge.
        double rangeLo = lo, rangeHi = hi;
        if (detail.thresholdsApplicable) {
            if (!isnan(detail.criticalThreshold)) { rangeLo = min(rangeLo, detail.criticalThreshold); rangeHi = max(rangeHi, detail.criticalThreshold); }
            if (!isnan(detail.warningThreshold))  { rangeLo = min(rangeLo, detail.warningThreshold);  rangeHi = max(rangeHi, detail.warningThreshold); }
        }
        if (rangeLo == rangeHi) { rangeLo -= 1; rangeHi += 1; }
        // lv_coord_t is int16_t on this build (LV_USE_LARGE_COORD=0) — clamp
        // before the cast below, since some metrics (e.g. an hourly error-
        // count rate) can genuinely exceed +/-32767, and casting an
        // out-of-range double to a narrower int type is undefined behavior.
        if (rangeLo < -32000) rangeLo = -32000;
        if (rangeHi >  32000) rangeHi =  32000;

        lv_chart_set_point_count(s_detailChart, (uint16_t)decimated.size());
        lv_chart_set_range(s_detailChart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)rangeLo, (lv_coord_t)rangeHi);
        for (const dd::MetricPoint& p : decimated) lv_chart_set_next_value(s_detailChart, s_detailSeries, (lv_coord_t)p.value);
        s_detailChartPoints = decimated;   // tap-to-inspect reads this — see buildDetailScreenIfNeeded()
        updateChartAxisLabels();

        lv_label_set_text(s_detailChartHiLbl, formatChartValue(hi).c_str());
        lv_label_set_text(s_detailChartLoLbl, formatChartValue(lo).c_str());
        updateChartHighlight((int)decimated.size() - 1);   // default: highlight the latest point

        // Threshold reference lines — position within the chart's own 100px
        // plot area (y 64..164) by linear-interpolating the threshold value
        // into that range, same math the chart itself uses internally.
        // Approximate: doesn't account for the chart's few px of internal
        // padding around the plotted line, so lines may sit a couple of
        // pixels off from the actual data — acceptable at this panel size.
        auto positionThresholdLine = [&](lv_obj_t* line, double threshold) {
            if (!detail.thresholdsApplicable || isnan(threshold) || threshold < rangeLo || threshold > rangeHi) {
                lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            double frac = (threshold - rangeLo) / (rangeHi - rangeLo);
            int y = 64 + (int)((1.0 - frac) * 100.0) - 1;
            lv_obj_set_pos(line, 12, y);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_HIDDEN);
        };
        positionThresholdLine(s_detailCritLine, detail.criticalThreshold);
        positionThresholdLine(s_detailWarnLine, detail.warningThreshold);
    } else {
        s_detailChartPoints.clear();
        if (s_detailCursor) lv_chart_set_cursor_point(s_detailChart, s_detailCursor, s_detailSeries, LV_CHART_POINT_NONE);
        lv_obj_add_flag(s_detailChart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_detailNoChart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_detailChartHiLbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_detailChartLoLbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_detailCurrentLbl, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < DETAIL_AXIS_TICK_COUNT; ++i) {
            lv_obj_add_flag(s_detailAxisTick[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_detailAxisLbl[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_detailCritLine, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_detailWarnLine, LV_OBJ_FLAG_HIDDEN);
        // Show the real reason, not a blanket claim — many "no chart"
        // cases are a query error (advanced monitor types like anomaly/
        // outlier/forecast embed monitor-evaluation syntax in their `query`
        // field that isn't valid classic metrics-query syntax on its own),
        // not an actual absence of chartable data for that monitor.
        String msg = chartError.length() ? ("No chart: " + chartError) : "No chart data returned for this query.";
        lv_label_set_text(s_detailNoChart, msg.c_str());
    }

    s_screen = ui::Screen::MonitorDetail;
    lv_scr_load_anim(s_detailScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void ui::applyMuteResult(bool ok, const String& msg) {
    if (!s_detailMuteResult) return;
    lv_label_set_text(s_detailMuteResult, msg.c_str());
    lv_obj_set_style_text_color(s_detailMuteResult, ok ? COLOR_OK : COLOR_ALERT, 0);
}

// ---- Incidents ----
// Real 5-step severity badge art is a polish-pass asset (BARKBOARD_PLAN.md
// §7/§9); until then a colored dot approximates SEV-1 (red) fading to
// SEV-5/UNKNOWN (grey), same gradient the plan describes.
static lv_color_t severityColor(const String& sev) {
    if (sev == "SEV-1" || sev == "SEV-2") return COLOR_ALERT;
    if (sev == "SEV-3") return COLOR_WARN;
    return COLOR_NODATA;   // SEV-4, SEV-5, UNKNOWN
}

static void buildIncidents() {
    lv_obj_t* s = s_dashScr[DASH_INCIDENTS];
    s_incList = lv_list_create(s);
    lv_obj_set_size(s_incList, LIST_WIDTH, SCREEN_HEIGHT - 24 - 24 - 12);
    lv_obj_set_pos(s_incList, LIST_INSET, 30);
    lv_obj_set_style_bg_opa(s_incList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_incList, 0, 0);
    lv_obj_set_style_pad_all(s_incList, 0, 0);
    lv_obj_add_flag(s_incList, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t* empty = lv_label_create(s_incList);
    lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
    lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
    lv_label_set_text(empty, "Loading incidents...");
}

void ui::notifyIncidentsRefreshed() {
    const std::vector<dd::Incident>& incidents = dd::lastIncidents();
    if (s_ovIncidentBadge) {
        lv_label_set_text(s_ovIncidentBadge, incidents.empty() ? "" : formatCount((int)incidents.size()).c_str());
    }

    if (!s_incList) return;
    lv_obj_clean(s_incList);

    if (incidents.empty()) {
        lv_obj_t* empty = lv_label_create(s_incList);
        lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
        lv_label_set_text(empty, "No active incidents.");
        return;
    }

    for (size_t i = 0; i < incidents.size(); ++i) {
        const dd::Incident& inc = incidents[i];
        String svc = inc.services.empty() ? "(no service)" : inc.services[0];
        String metaText = inc.severity + " - " + svc;
        lv_obj_t* row = buildTableRow(s_incList, true, severityColor(inc.severity), inc.title, metaText);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            const std::vector<dd::Incident>& list = dd::lastIncidents();
            if (idx >= 0 && idx < (int)list.size()) ui::showIncidentDetail(list[idx]);
        }, LV_EVENT_CLICKED, nullptr);
    }
}

bool ui::incidentsFetchPending() {
    if (!s_incidentsFetchPending) return false;
    portENTER_CRITICAL(&s_pendingMux);
    bool hit = s_incidentsFetchPending;
    s_incidentsFetchPending = false;
    portEXIT_CRITICAL(&s_pendingMux);
    return hit;
}

// ---- On-Call ----

// Hero card height/gap — reserved above the escalation list so whoever's on
// call *right now* reads at a glance, distinct from the escalation-step
// rows below it (which all rendered identically before, active or not).
#define OC_CARD_H   48
#define OC_CARD_GAP 8

static void buildOnCall() {
    lv_obj_t* s = s_dashScr[DASH_ONCALL];

    s_ocCurrentCard = lv_obj_create(s);
    lv_obj_set_size(s_ocCurrentCard, LIST_WIDTH, OC_CARD_H);
    lv_obj_set_pos(s_ocCurrentCard, LIST_INSET, 30);
    lv_obj_set_style_bg_color(s_ocCurrentCard, COLOR_PURPLE, 0);
    lv_obj_set_style_border_width(s_ocCurrentCard, 0, 0);
    lv_obj_set_style_radius(s_ocCurrentCard, 8, 0);
    lv_obj_set_style_pad_all(s_ocCurrentCard, 0, 0);
    lv_obj_clear_flag(s_ocCurrentCard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ocCurrentCard, LV_OBJ_FLAG_HIDDEN);   // shown once real data lands

    s_ocCurrentIcon = lv_label_create(s_ocCurrentCard);
    lv_obj_set_style_text_font(s_ocCurrentIcon, &outfit_bold_16, 0);
    lv_obj_set_style_text_color(s_ocCurrentIcon, lv_color_white(), 0);
    lv_label_set_text(s_ocCurrentIcon, LV_SYMBOL_CALL);
    lv_obj_align(s_ocCurrentIcon, LV_ALIGN_LEFT_MID, 12, 0);

    // Name + email as a two-line block, positioned (not LV_ALIGN_*_MID) so
    // the block's own vertical center lands on the card's center to match
    // the icon — outfit_bold_16/outfit_thin_12's real line heights are
    // 17px/13px, so an 8px top offset centers the 32px-tall (17+2+13)
    // block within this 48px-tall card. The old code gave the name label
    // itself the *card's* height and centered that box, which visually
    // pushed the text (drawn top-down within its own box, not centered in
    // it) above the icon rather than level with it.
    s_ocCurrentName = lv_label_create(s_ocCurrentCard);
    lv_obj_set_style_text_font(s_ocCurrentName, &outfit_bold_16, 0);
    lv_obj_set_style_text_color(s_ocCurrentName, lv_color_white(), 0);
    lv_obj_set_width(s_ocCurrentName, LIST_WIDTH - 44);
    lv_label_set_long_mode(s_ocCurrentName, LV_LABEL_LONG_DOT);
    lv_obj_align(s_ocCurrentName, LV_ALIGN_TOP_LEFT, 36, 8);

    s_ocCurrentEmail = lv_label_create(s_ocCurrentCard);
    lv_obj_set_style_text_font(s_ocCurrentEmail, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_ocCurrentEmail, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s_ocCurrentEmail, LV_OPA_70, 0);
    lv_obj_set_width(s_ocCurrentEmail, LIST_WIDTH - 44);
    lv_label_set_long_mode(s_ocCurrentEmail, LV_LABEL_LONG_DOT);
    lv_obj_align(s_ocCurrentEmail, LV_ALIGN_TOP_LEFT, 36, 27);

    s_ocList = lv_list_create(s);
    lv_obj_set_size(s_ocList, LIST_WIDTH, SCREEN_HEIGHT - 24 - 24 - 12 - OC_CARD_H - OC_CARD_GAP);
    lv_obj_set_pos(s_ocList, LIST_INSET, 30 + OC_CARD_H + OC_CARD_GAP);
    lv_obj_set_style_bg_opa(s_ocList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ocList, 0, 0);
    lv_obj_set_style_pad_all(s_ocList, 0, 0);
    lv_obj_add_flag(s_ocList, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t* empty = lv_label_create(s_ocList);
    lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
    lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
    lv_label_set_text(empty, "Loading on-call...");
}

void ui::notifyOnCallRefreshed(const std::vector<dd::OnCallEntry>& entries, bool hasTeams, bool needsTeamPick) {
    s_lastOnCall = entries;
    if (!s_ocList) return;
    lv_obj_clean(s_ocList);
    lv_obj_add_flag(s_ocCurrentCard, LV_OBJ_FLAG_HIDDEN);

    if (needsTeamPick) {
        lv_obj_t* empty = lv_label_create(s_ocList);
        lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
        lv_label_set_text(empty, "Multiple teams found for your account.\nPick one at http://barkboard.local");
        return;
    }
    if (!hasTeams) {
        lv_obj_t* empty = lv_label_create(s_ocList);
        lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
        lv_label_set_text(empty, "No teams configured for On-Call.\nSet teams up in Datadog first.");
        return;
    }
    if (entries.empty()) {
        lv_obj_t* empty = lv_label_create(s_ocList);
        lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
        lv_label_set_text(empty, "No one currently on call.");
        return;
    }

    // Split "Current" (escalationLevel==0) from escalation steps — the
    // former get the hero card up top instead of blending into the dense
    // list at the same visual weight as everyone else in the policy.
    String currentNames;
    String currentEmails;
    bool anyEscalation = false;
    for (const dd::OnCallEntry& e : entries) {
        if (e.escalationLevel == 0) {
            if (currentNames.length()) currentNames += " & ";
            currentNames += e.user.length() ? e.user : "(unassigned)";
            // Skip when email is empty, or when it's literally what's
            // already shown as the name (resolveOnCallUserName() falls
            // back to email itself when the user has no display name) —
            // no point repeating the same string on both lines.
            if (e.email.length() && e.email != e.user) {
                if (currentEmails.length()) currentEmails += " & ";
                currentEmails += e.email;
            }
        } else {
            anyEscalation = true;
        }
    }
    if (currentNames.length()) {
        lv_label_set_text(s_ocCurrentName, currentNames.c_str());
        lv_label_set_text(s_ocCurrentEmail, currentEmails.c_str());
        lv_obj_clear_flag(s_ocCurrentCard, LV_OBJ_FLAG_HIDDEN);
    }

    if (!anyEscalation) {
        lv_obj_t* empty = lv_label_create(s_ocList);
        lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
        lv_label_set_text(empty, "No escalation policy configured.");
        return;
    }
    for (const dd::OnCallEntry& e : entries) {
        if (e.escalationLevel == 0) continue;   // already shown in the hero card
        // No dot (no health-state concept here) and no click handler — this
        // screen has no On-Call Detail to drill into, same as before.
        buildTableRow(s_ocList, false, COLOR_MUTED,
                      e.user.length() ? e.user : "(unassigned)", e.schedule);
    }
}

bool ui::oncallFetchPending() {
    if (!s_oncallFetchPending) return false;
    portENTER_CRITICAL(&s_pendingMux);
    bool hit = s_oncallFetchPending;
    s_oncallFetchPending = false;
    portEXIT_CRITICAL(&s_pendingMux);
    return hit;
}

// ---- SLOs ----
// The "wait, that's cool" screen (BARKBOARD_PLAN.md §4 Screen 5) — an error
// budget is literally a number you're spending, perfect for a physical gauge.

static void buildSlos() {
    lv_obj_t* s = s_dashScr[DASH_SLO];
    s_sloList = lv_list_create(s);
    lv_obj_set_size(s_sloList, LIST_WIDTH, SCREEN_HEIGHT - 24 - 24 - 12);
    lv_obj_set_pos(s_sloList, LIST_INSET, 30);
    lv_obj_set_style_bg_opa(s_sloList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sloList, 0, 0);
    lv_obj_set_style_pad_all(s_sloList, 0, 0);
    lv_obj_add_flag(s_sloList, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t* empty = lv_label_create(s_sloList);
    lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
    lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
    lv_label_set_text(empty, "Loading SLOs...");
}

// SLOState values (Datadog API): "breached" | "warning" | "ok" | "no_data".
static lv_color_t sloStateColor(const String& state) {
    if (state == "breached") return COLOR_ALERT;
    if (state == "warning") return COLOR_WARN;
    if (state == "ok") return COLOR_OK;
    return COLOR_NODATA;   // "no_data" or unset (older cached rows, etc.)
}

void ui::notifySlosRefreshed() {
    if (!s_sloList) return;
    lv_obj_clean(s_sloList);

    const std::vector<dd::SloSummary>& slos = dd::lastSlos();
    if (slos.empty()) {
        lv_obj_t* empty = lv_label_create(s_sloList);
        lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
        lv_label_set_text(empty, "No SLOs configured.");
        return;
    }

    for (size_t i = 0; i < slos.size(); ++i) {
        const dd::SloSummary& slo = slos[i];
        String metaText = String(slo.target, 2) + "% target - " + slo.timeframe;
        lv_obj_t* row = buildTableRow(s_sloList, true, sloStateColor(slo.state), slo.name, metaText);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
            const std::vector<dd::SloSummary>& list = dd::lastSlos();
            if (idx >= 0 && idx < (int)list.size()) {
                s_sloDetailRequestId = list[idx].id;
                s_sloDetailRequestPending = true;
            }
        }, LV_EVENT_CLICKED, nullptr);
    }
}

bool ui::sloFetchPending() {
    if (!s_sloFetchPending) return false;
    portENTER_CRITICAL(&s_pendingMux);
    bool hit = s_sloFetchPending;
    s_sloFetchPending = false;
    portEXIT_CRITICAL(&s_pendingMux);
    return hit;
}

bool ui::sloDetailRequestPending(String& outId) {
    if (!s_sloDetailRequestPending) return false;
    s_sloDetailRequestPending = false;
    outId = s_sloDetailRequestId;
    return true;
}

// ---- Bits Investigations ----
// Real status vocabulary confirmed live (dd::fetchBitsInvestigations()'s doc
// comment): "conclusive" is the one actually seen; "inconclusive"/"failed"/
// "pending"/"in progress" are documented but unseen in the org this was
// tested against, so "inconclusive" is treated as a soft-warning color
// rather than assumed-bad, and unrecognized values fall back to WARN rather
// than a false-confident OK.
static lv_color_t bitsStatusColor(const String& status) {
    String s = status; s.toLowerCase();
    if (s == "conclusive") return COLOR_OK;
    if (s == "failed") return COLOR_ALERT;
    if (s == "inconclusive" || s == "pending" || s == "in progress") return COLOR_WARN;
    return COLOR_WARN;
}

static void buildBitsInvestigations() {
    lv_obj_t* s = s_dashScr[DASH_BITS];
    s_bitsInvList = lv_list_create(s);
    lv_obj_set_size(s_bitsInvList, LIST_WIDTH, SCREEN_HEIGHT - 24 - 24 - 12);
    lv_obj_set_pos(s_bitsInvList, LIST_INSET, 30);
    lv_obj_set_style_bg_opa(s_bitsInvList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bitsInvList, 0, 0);
    lv_obj_set_style_pad_all(s_bitsInvList, 0, 0);
    lv_obj_add_flag(s_bitsInvList, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t* empty = lv_label_create(s_bitsInvList);
    lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
    lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
    lv_label_set_text(empty, "Loading investigations...");
}

void ui::notifyBitsInvestigationsRefreshed() {
    if (!s_bitsInvList) return;
    lv_obj_clean(s_bitsInvList);

    const std::vector<dd::BitsInvestigation>& investigations = dd::lastBitsInvestigations();
    if (investigations.empty()) {
        lv_obj_t* empty = lv_label_create(s_bitsInvList);
        lv_obj_set_style_text_font(empty, &outfit_thin_14, 0);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, 0);
        lv_label_set_text(empty, "No investigations yet.\nTrigger one from a monitor's detail screen.");
        return;
    }

    for (const dd::BitsInvestigation& inv : investigations) {
        lv_obj_t* row = buildTableRow(s_bitsInvList, true, bitsStatusColor(inv.status), inv.title, inv.status);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        // Stash the investigation id (a String, too big for user_data's
        // void*) — same heap-allocate-a-copy-and-free-on-delete pattern the
        // Declare > Case project picker uses.
        lv_obj_set_user_data(row, (void*)new String(inv.id));
        lv_obj_add_event_cb(row, [](lv_event_t* e) { delete (String*)lv_obj_get_user_data(lv_event_get_target(e)); }, LV_EVENT_DELETE, nullptr);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            String* id = (String*)lv_obj_get_user_data(lv_event_get_target(e));
            portENTER_CRITICAL(&s_pendingMux);
            s_bitsInvestigationDetailRequestId = *id;
            s_bitsInvestigationDetailRequestPending = true;
            portEXIT_CRITICAL(&s_pendingMux);
        }, LV_EVENT_CLICKED, nullptr);
    }
}

bool ui::bitsInvestigationsFetchPending() {
    if (!s_bitsInvestigationsFetchPending) return false;
    portENTER_CRITICAL(&s_pendingMux);
    bool hit = s_bitsInvestigationsFetchPending;
    s_bitsInvestigationsFetchPending = false;
    portEXIT_CRITICAL(&s_pendingMux);
    return hit;
}

bool ui::bitsInvestigationDetailRequestPending(String& outId) {
    if (!s_bitsInvestigationDetailRequestPending) return false;
    portENTER_CRITICAL(&s_pendingMux);
    bool hit = s_bitsInvestigationDetailRequestPending;
    outId = s_bitsInvestigationDetailRequestId;
    s_bitsInvestigationDetailRequestPending = false;
    portEXIT_CRITICAL(&s_pendingMux);
    return hit;
}

static void buildBitsInvestigationDetailIfNeeded() {
    if (s_bitsInvDetailScr) return;
    logMemCheckpoint("before Bits Investigation Detail");
    s_bitsInvDetailScr = lv_obj_create(nullptr);
    styleFullscreen(s_bitsInvDetailScr);   // NOT scrollable — back button/title/status stay fixed

    lv_obj_t* back = lv_btn_create(s_bitsInvDetailScr);
    lv_obj_set_size(back, 44, 28);
    lv_obj_set_pos(back, 8, 8);
    lv_obj_set_style_bg_color(back, COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        if (s_dashScr[DASH_BITS]) lv_scr_load_anim(s_dashScr[DASH_BITS], LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
        s_screen = ui::Screen::Bits;
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLbl = lv_label_create(back);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLbl, COLOR_INK, 0);
    lv_obj_center(backLbl);

    s_bitsInvDetailStatus = lv_label_create(s_bitsInvDetailScr);
    lv_obj_set_style_text_font(s_bitsInvDetailStatus, &outfit_bold_12, 0);
    lv_obj_align(s_bitsInvDetailStatus, LV_ALIGN_TOP_RIGHT, -12, 16);

    s_bitsInvDetailTitle = lv_label_create(s_bitsInvDetailScr);
    lv_obj_set_style_text_font(s_bitsInvDetailTitle, &outfit_bold_16, 0);
    lv_obj_set_style_text_color(s_bitsInvDetailTitle, COLOR_INK, 0);
    lv_obj_set_width(s_bitsInvDetailTitle, SCREEN_WIDTH - 24);
    lv_label_set_long_mode(s_bitsInvDetailTitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_bitsInvDetailTitle, 12, 44);

    // Everything below the fixed header lives in its own scrollable
    // container instead of the screen itself scrolling — the back button/
    // title/status need to stay reachable regardless of how long the
    // conclusion summary runs (a real one ran ~500 chars in testing).
    s_bitsInvDetailBody = lv_obj_create(s_bitsInvDetailScr);
    lv_obj_set_size(s_bitsInvDetailBody, SCREEN_WIDTH - 24, SCREEN_HEIGHT - 96);
    lv_obj_set_pos(s_bitsInvDetailBody, 12, 92);
    lv_obj_set_style_bg_opa(s_bitsInvDetailBody, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bitsInvDetailBody, 0, 0);
    lv_obj_set_style_pad_all(s_bitsInvDetailBody, 0, 0);
    lv_obj_set_scroll_dir(s_bitsInvDetailBody, LV_DIR_VER);

    s_bitsInvDetailConclTitle = lv_label_create(s_bitsInvDetailBody);
    lv_obj_set_style_text_font(s_bitsInvDetailConclTitle, &outfit_bold_14, 0);
    lv_obj_set_style_text_color(s_bitsInvDetailConclTitle, COLOR_PURPLE, 0);
    lv_obj_set_width(s_bitsInvDetailConclTitle, SCREEN_WIDTH - 24);
    lv_label_set_long_mode(s_bitsInvDetailConclTitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_bitsInvDetailConclTitle, 0, 0);

    s_bitsInvDetailConclSummary = lv_label_create(s_bitsInvDetailBody);
    lv_obj_set_style_text_font(s_bitsInvDetailConclSummary, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_bitsInvDetailConclSummary, COLOR_MUTED, 0);
    lv_obj_set_width(s_bitsInvDetailConclSummary, SCREEN_WIDTH - 24);
    lv_label_set_long_mode(s_bitsInvDetailConclSummary, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_bitsInvDetailConclSummary, 0, 40);
    logMemCheckpoint("after Bits Investigation Detail");
}

void ui::showBitsInvestigationDetail(const dd::BitsInvestigationDetail& detail, const String& err) {
    buildBitsInvestigationDetailIfNeeded();
    lv_obj_scroll_to_y(s_bitsInvDetailBody, 0, LV_ANIM_OFF);

    if (detail.detailOk) {
        lv_label_set_text(s_bitsInvDetailTitle, detail.title.c_str());
        lv_label_set_text(s_bitsInvDetailStatus, detail.status.c_str());
        lv_obj_set_style_text_color(s_bitsInvDetailStatus, bitsStatusColor(detail.status), 0);
        lv_label_set_text(s_bitsInvDetailConclTitle, detail.conclusionTitle.c_str());
        lv_label_set_text(s_bitsInvDetailConclSummary, detail.conclusionSummary.c_str());
    } else {
        lv_label_set_text(s_bitsInvDetailTitle, "Couldn't load investigation");
        lv_label_set_text(s_bitsInvDetailStatus, "");
        lv_label_set_text(s_bitsInvDetailConclTitle, "");
        lv_label_set_text(s_bitsInvDetailConclSummary, err.c_str());
    }

    s_screen = ui::Screen::BitsInvestigationDetail;
    lv_scr_load_anim(s_bitsInvDetailScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void buildSloDetailIfNeeded() {
    if (s_sloDetailScr) return;
    logMemCheckpoint("before SLO Detail");
    s_sloDetailScr = lv_obj_create(nullptr);
    styleFullscreen(s_sloDetailScr);

    lv_obj_t* back = lv_btn_create(s_sloDetailScr);
    lv_obj_set_size(back, 44, 28);
    lv_obj_set_pos(back, 8, 8);
    lv_obj_set_style_bg_color(back, COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        if (s_dashScr[DASH_SLO]) lv_scr_load_anim(s_dashScr[DASH_SLO], LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
        s_screen = ui::Screen::Slo;
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLbl = lv_label_create(back);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLbl, COLOR_INK, 0);
    lv_obj_center(backLbl);

    s_sloDetailName = lv_label_create(s_sloDetailScr);
    lv_obj_set_style_text_font(s_sloDetailName, &outfit_bold_18, 0);
    lv_obj_set_style_text_color(s_sloDetailName, COLOR_INK, 0);
    lv_obj_set_width(s_sloDetailName, SCREEN_WIDTH - 24);
    lv_label_set_long_mode(s_sloDetailName, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_sloDetailName, 12, 12);

    // ~140px arc, 12px stroke, green->yellow->red budget fill (DESIGN.md).
    s_sloDetailArc = lv_arc_create(s_sloDetailScr);
    lv_obj_set_size(s_sloDetailArc, 140, 140);
    lv_obj_align(s_sloDetailArc, LV_ALIGN_CENTER, 0, 4);
    lv_arc_set_rotation(s_sloDetailArc, 270);
    lv_arc_set_bg_angles(s_sloDetailArc, 0, 360);
    lv_arc_set_range(s_sloDetailArc, 0, 100);
    lv_obj_set_style_arc_width(s_sloDetailArc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_sloDetailArc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_sloDetailArc, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_remove_style(s_sloDetailArc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(s_sloDetailArc, LV_OBJ_FLAG_CLICKABLE);

    s_sloDetailPct = lv_label_create(s_sloDetailScr);
    lv_obj_set_style_text_font(s_sloDetailPct, &outfit_thin_48, 0);
    lv_obj_align(s_sloDetailPct, LV_ALIGN_CENTER, 0, 4);

    s_sloDetailMeta = lv_label_create(s_sloDetailScr);
    lv_obj_set_style_text_font(s_sloDetailMeta, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_sloDetailMeta, COLOR_MUTED, 0);
    lv_obj_align(s_sloDetailMeta, LV_ALIGN_BOTTOM_MID, 0, -12);
    logMemCheckpoint("after SLO Detail");
}

void ui::showSloDetail(const dd::SloSummary& summary, const dd::SloStatus& status, bool statusOk) {
    buildSloDetailIfNeeded();
    lv_label_set_text(s_sloDetailName, summary.name.c_str());

    if (statusOk) {
        // Budget remaining can go negative once breached (Datadog's own
        // definition, confirmed live) — clamp to the 0-100 arc range but
        // keep the real number in the caption underneath.
        double remaining = status.errorBudgetRemaining;
        int arcVal = (int)(remaining < 0 ? 0 : (remaining > 100 ? 100 : remaining));
        lv_arc_set_value(s_sloDetailArc, arcVal);

        lv_color_t fillColor = COLOR_OK;
        if (remaining < 20) fillColor = COLOR_ALERT;
        else if (remaining < 50) fillColor = COLOR_WARN;
        lv_obj_set_style_arc_color(s_sloDetailArc, fillColor, LV_PART_INDICATOR);

        lv_label_set_text(s_sloDetailPct, (String((int)remaining) + "%").c_str());
        lv_obj_set_style_text_color(s_sloDetailPct, fillColor, 0);

        String meta = "Budget remaining - target " + String(summary.target, 1) + "% over " + summary.timeframe +
                      "\nSLI: " + String(status.sliValue, 2) + "% - " + status.state;
        lv_label_set_text(s_sloDetailMeta, meta.c_str());
    } else {
        lv_arc_set_value(s_sloDetailArc, 0);
        lv_label_set_text(s_sloDetailPct, "--");
        lv_label_set_text(s_sloDetailMeta, "Couldn't load SLO status.");
    }

    s_screen = ui::Screen::SloDetail;
    lv_scr_load_anim(s_sloDetailScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

// ---- Incident Detail ----

static void buildIncidentDetailIfNeeded() {
    if (s_incDetailScr) return;
    logMemCheckpoint("before Incident Detail");
    s_incDetailScr = lv_obj_create(nullptr);
    styleFullscreen(s_incDetailScr);

    lv_obj_t* back = lv_btn_create(s_incDetailScr);
    lv_obj_set_size(back, 44, 28);
    lv_obj_set_pos(back, 8, 8);
    lv_obj_set_style_bg_color(back, COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        if (s_dashScr[DASH_INCIDENTS]) lv_scr_load_anim(s_dashScr[DASH_INCIDENTS], LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
        s_screen = ui::Screen::Incidents;
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLbl = lv_label_create(back);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLbl, COLOR_INK, 0);
    lv_obj_center(backLbl);

    // Same fixed-height-zone fix as Monitor Detail: each element reserves
    // its own space up front so a long title/meta block can't grow into
    // whatever's positioned after it.
    s_incDetailBadge = lv_label_create(s_incDetailScr);
    lv_obj_set_style_text_font(s_incDetailBadge, &outfit_bold_14, 0);
    lv_obj_set_pos(s_incDetailBadge, 58, 14);

    s_incDetailTitle = lv_label_create(s_incDetailScr);
    lv_obj_set_style_text_font(s_incDetailTitle, &outfit_bold_16, 0);
    lv_obj_set_style_text_color(s_incDetailTitle, COLOR_INK, 0);
    lv_obj_set_size(s_incDetailTitle, SCREEN_WIDTH - 24, 40);
    lv_label_set_long_mode(s_incDetailTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_incDetailTitle, 12, 40);

    s_incDetailMeta = lv_label_create(s_incDetailScr);
    lv_obj_set_style_text_font(s_incDetailMeta, &outfit_thin_14, 0);
    lv_obj_set_style_text_color(s_incDetailMeta, COLOR_MUTED, 0);
    lv_obj_set_size(s_incDetailMeta, SCREEN_WIDTH - 24, 90);
    lv_label_set_long_mode(s_incDetailMeta, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_incDetailMeta, 12, 84);

    // Single action button: cycle state. Datadog incident states are
    // org-configurable (BARKBOARD_PLAN.md §3.1) — this uses the documented
    // Active -> Stable -> Resolved fallback since there's no Settings screen
    // yet to surface an org-specific override.
    lv_obj_t* advBtn = lv_btn_create(s_incDetailScr);
    lv_obj_set_size(advBtn, 140, 32);
    lv_obj_set_pos(advBtn, SCREEN_WIDTH - 12 - 140, 188);
    lv_obj_set_style_bg_color(advBtn, COLOR_PURPLE, 0);
    lv_obj_set_style_border_width(advBtn, 0, 0);
    lv_obj_add_event_cb(advBtn, [](lv_event_t*) {
        s_incidentAdvanceId = s_incDetailId;
        s_incidentAdvancePending = true;
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* advLbl = lv_label_create(advBtn);
    lv_obj_set_style_text_font(advLbl, &outfit_bold_14, 0);
    lv_label_set_text(advLbl, "ADVANCE STATE");
    lv_obj_center(advLbl);

    s_incDetailResult = lv_label_create(s_incDetailScr);
    lv_obj_set_style_text_font(s_incDetailResult, &outfit_thin_12, 0);
    lv_obj_set_pos(s_incDetailResult, 12, 198);
    logMemCheckpoint("after Incident Detail");
}

void ui::showIncidentDetail(const dd::Incident& inc) {
    buildIncidentDetailIfNeeded();
    s_incDetailId = inc.id;
    s_incDetailState = inc.state;
    s_incDetailSeverity = inc.severity;

    lv_label_set_text(s_incDetailBadge, (inc.severity + " - " + inc.state).c_str());
    lv_obj_set_style_text_color(s_incDetailBadge, severityColor(inc.severity), 0);
    lv_label_set_text(s_incDetailTitle, inc.title.c_str());

    String services = inc.services.empty() ? "(none)" : "";
    for (size_t i = 0; i < inc.services.size(); ++i) {
        if (i) services += ", ";
        services += inc.services[i];
    }
    String commander = inc.commander.length() ? inc.commander : "(unassigned)";
    String meta = "Commander: " + commander + "\nServices: " + services + "\nCreated: " + inc.createdAt;
    lv_label_set_text(s_incDetailMeta, meta.c_str());
    lv_label_set_text(s_incDetailResult, "");

    s_screen = ui::Screen::IncidentDetail;
    lv_scr_load_anim(s_incDetailScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

bool ui::incidentAdvancePending(String& outId, String& outNewState) {
    if (!s_incidentAdvancePending) return false;
    s_incidentAdvancePending = false;
    outId = s_incidentAdvanceId;
    outNewState = dd::nextIncidentState(s_incDetailState);
    return true;
}

void ui::applyIncidentAdvanceResult(bool ok, const String& newState, const String& msg) {
    if (ok) {
        s_incDetailState = newState;
        if (s_incDetailBadge) lv_label_set_text(s_incDetailBadge, (s_incDetailSeverity + " - " + newState).c_str());
    }
    if (!s_incDetailResult) return;
    lv_label_set_text(s_incDetailResult, msg.c_str());
    lv_obj_set_style_text_color(s_incDetailResult, ok ? COLOR_OK : COLOR_ALERT, 0);
}

// ---- Settings ----
// BARKBOARD_PLAN.md §4 "Status/Settings screen (gear icon, same as
// original)". The free-text "which teams/monitors matter" scope picker
// needs a keyboard widget that isn't enabled in lv_conf.h (LV_USE_TEXTAREA/
// LV_USE_KEYBOARD are both 0) — deferred; this covers everything else:
// connection info, re-detect site, chirp mute, factory reset.

static void buildSettingsIfNeeded() {
    if (s_settingsScr) return;
    logMemCheckpoint("before Settings");
    s_settingsScr = lv_obj_create(nullptr);
    styleFullscreen(s_settingsScr);

    lv_obj_t* back = lv_btn_create(s_settingsScr);
    lv_obj_set_size(back, 44, 28);
    lv_obj_set_pos(back, 8, 8);
    lv_obj_set_style_bg_color(back, COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, [](lv_event_t*) {
        if (s_dashScr[s_dashIdx]) lv_scr_load_anim(s_dashScr[s_dashIdx], LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
        s_screen = s_screenBeforeSettings;
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLbl = lv_label_create(back);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLbl, COLOR_INK, 0);
    lv_obj_center(backLbl);

    lv_obj_t* title = lv_label_create(s_settingsScr);
    lv_obj_set_style_text_font(title, &outfit_bold_18, 0);
    lv_obj_set_style_text_color(title, COLOR_INK, 0);
    lv_label_set_text(title, "Settings");
    lv_obj_set_pos(title, 58, 14);

    s_settingsInfo = lv_label_create(s_settingsScr);
    lv_obj_set_style_text_font(s_settingsInfo, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_settingsInfo, COLOR_MUTED, 0);
    lv_obj_set_pos(s_settingsInfo, 12, 46);

    s_settingsSite = lv_label_create(s_settingsScr);
    lv_obj_set_style_text_font(s_settingsSite, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_settingsSite, COLOR_MUTED, 0);
    lv_obj_set_width(s_settingsSite, SCREEN_WIDTH - 24);
    lv_label_set_long_mode(s_settingsSite, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_settingsSite, 12, 118);

    lv_obj_t* redetectBtn = lv_btn_create(s_settingsScr);
    lv_obj_set_size(redetectBtn, 150, 30);
    lv_obj_set_pos(redetectBtn, 12, 150);
    lv_obj_set_style_bg_color(redetectBtn, COLOR_PURPLE, 0);
    lv_obj_set_style_border_width(redetectBtn, 0, 0);
    lv_obj_add_event_cb(redetectBtn, [](lv_event_t*) { s_redetectSitePending = true; }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* redetectLbl = lv_label_create(redetectBtn);
    lv_obj_set_style_text_font(redetectLbl, &outfit_bold_14, 0);
    lv_label_set_text(redetectLbl, "RE-DETECT SITE");
    lv_obj_center(redetectLbl);

    // Chirp mute — a plain toggling button rather than lv_switch, which is
    // disabled in lv_conf.h (LV_USE_SWITCH 0).
    s_settingsChirp = lv_btn_create(s_settingsScr);
    lv_obj_set_size(s_settingsChirp, 150, 30);
    lv_obj_set_pos(s_settingsChirp, 12, 190);
    lv_obj_set_style_bg_color(s_settingsChirp, COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(s_settingsChirp, 0, 0);
    lv_obj_add_event_cb(s_settingsChirp, [](lv_event_t*) {
        s_chirpMuted = !s_chirpMuted;
        lv_label_set_text(lv_obj_get_child(s_settingsChirp, 0), s_chirpMuted ? "CHIRP: MUTED" : "CHIRP: ON");
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* chirpLbl = lv_label_create(s_settingsChirp);
    lv_obj_set_style_text_font(chirpLbl, &outfit_bold_14, 0);
    lv_label_set_text(chirpLbl, "CHIRP: ON");
    lv_obj_center(chirpLbl);

    s_settingsReset = lv_btn_create(s_settingsScr);
    lv_obj_set_size(s_settingsReset, 150, 30);
    lv_obj_align(s_settingsReset, LV_ALIGN_BOTTOM_LEFT, 12, -14);
    lv_obj_set_style_bg_color(s_settingsReset, COLOR_ALERT, 0);
    lv_obj_set_style_border_width(s_settingsReset, 0, 0);
    lv_obj_add_event_cb(s_settingsReset, [](lv_event_t*) {
        if (s_resetConfirmArmed) {
            s_factoryResetPending = true;
        } else {
            s_resetConfirmArmed = 1;
            lv_label_set_text(lv_obj_get_child(s_settingsReset, 0), "TAP AGAIN TO CONFIRM");
        }
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* resetLbl = lv_label_create(s_settingsReset);
    lv_obj_set_style_text_font(resetLbl, &outfit_bold_14, 0);
    lv_label_set_text(resetLbl, "FACTORY RESET");
    lv_obj_center(resetLbl);

    s_settingsResult = lv_label_create(s_settingsScr);
    lv_obj_set_style_text_font(s_settingsResult, &outfit_thin_12, 0);
    lv_obj_align(s_settingsResult, LV_ALIGN_BOTTOM_RIGHT, -12, -20);
    logMemCheckpoint("after Settings");
}

void ui::showSettings() {
    buildSettingsIfNeeded();
    s_screenBeforeSettings = s_screen;
    s_resetConfirmArmed = 0;
    lv_label_set_text(lv_obj_get_child(s_settingsReset, 0), "FACTORY RESET");
    lv_label_set_text(s_settingsResult, "");

    // barkboard.local resolves to the same address as the IP line above via
    // mDNS (netcfg::begin() registers it) — worth surfacing directly since
    // it's what the README and setup flow actually tell people to type,
    // rather than making them come back here to read off a raw IP first.
    String info = "WiFi: " + WiFi.SSID() + "\nIP: " + WiFi.localIP().toString() +
                  "\nSetup: barkboard.local" +
                  "\nRSSI: " + String(WiFi.RSSI()) + " dBm" +
                  "\nFree heap: " + String(ESP.getFreeHeap() / 1024) + " KB";
    lv_label_set_text(s_settingsInfo, info.c_str());

    String site = storage::getSite();
    lv_label_set_text(s_settingsSite, ("Datadog site: " + (site.length() ? site : String("(not detected)"))).c_str());

    s_screen = ui::Screen::Settings;
    lv_scr_load_anim(s_settingsScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

bool ui::chirpMuted() { return s_chirpMuted; }

bool ui::redetectSitePending() {
    if (!s_redetectSitePending) return false;
    s_redetectSitePending = false;
    return true;
}

void ui::applyRedetectResult(bool ok, const String& site, const String& err) {
    if (!s_settingsResult) return;
    if (ok) {
        lv_label_set_text(s_settingsSite, ("Datadog site: " + site).c_str());
        lv_label_set_text(s_settingsResult, "Re-detected OK");
        lv_obj_set_style_text_color(s_settingsResult, COLOR_OK, 0);
    } else {
        lv_label_set_text(s_settingsResult, ("Fail: " + err).c_str());
        lv_obj_set_style_text_color(s_settingsResult, COLOR_ALERT, 0);
    }
}

bool ui::factoryResetPending() {
    if (!s_factoryResetPending) return false;
    s_factoryResetPending = false;
    return true;
}

// ---- Bits idle screen ----
// The "tamagotchi" screen (BARKBOARD_PLAN.md §4 Easter egg / idle
// screensaver) — reached by tapping the Bits mark in any status bar. Built
// from the single existing bits_icon_big asset via LVGL's procedural zoom/
// angle transforms rather than new sprite-frame art (per §4's own note that
// "convincing motion from ~4 source frames plus a few animation timelines"
// works — here it's zero new frames, just transforms on the one we have).

static lv_obj_t* s_bitsIdleScr  = nullptr;
static lv_obj_t* s_bitsIdleStat = nullptr;
static lv_obj_t* s_bitsIdleDate = nullptr;

static void bitsIdleTick(lv_timer_t*) {
    if (!s_bitsIdleStat || lv_scr_act() != s_bitsIdleScr) return;

    uint32_t s = millis() / 1000;
    uint32_t h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    String stat = "Up " + String(h) + "h " + String(m) + "m " + String(sec) + "s";
    lv_label_set_text(s_bitsIdleStat, stat.c_str());

    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm tmu; localtime_r(&now, &tmu);   // localtime_r, not gmtime_r — see main.cpp's clock tick comment
        char buf[24];
        strftime(buf, sizeof(buf), storage::getTimeFormat24h() ? "%a %b %d - %H:%M" : "%a %b %d - %I:%M %p", &tmu);
        lv_label_set_text(s_bitsIdleDate, buf);
    }
}

// 15-frame "looking around" cycle (assets/bits_look_0..14.png, sliced from
// the user-supplied assets/source/bits-looking-around-sheet.png 4x4 grid —
// frame 15 was blank, dropped). Center -> look right -> center -> look left
// -> center. Downscaled with LANCZOS (not NEAREST): this source art is
// clean shaded cartoon linework, not blocky pixel art, so smooth resampling
// looks right here unlike the walk-cycle sprite it replaced.
static const void* s_dogFrames[] = {
    &bits_look_0,  &bits_look_1,  &bits_look_2,  &bits_look_3,
    &bits_look_4,  &bits_look_5,  &bits_look_6,  &bits_look_7,
    &bits_look_8,  &bits_look_9,  &bits_look_10, &bits_look_11,
    &bits_look_12, &bits_look_13, &bits_look_14,
};

static void buildBitsIdleIfNeeded() {
    if (s_bitsIdleScr) return;
    s_bitsIdleScr = lv_obj_create(nullptr);
    styleFullscreen(s_bitsIdleScr);

    lv_obj_t* back = lv_btn_create(s_bitsIdleScr);
    lv_obj_set_size(back, 44, 28);
    lv_obj_set_pos(back, 8, 8);
    lv_obj_set_style_bg_color(back, COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, [](lv_event_t*) { rotateTo(s_dashIdx, false); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLbl = lv_label_create(back);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLbl, COLOR_INK, 0);
    lv_obj_center(backLbl);

    lv_obj_t* dog = lv_animimg_create(s_bitsIdleScr);
    lv_obj_align(dog, LV_ALIGN_TOP_MID, 0, 16);
    lv_animimg_set_src(dog, (const void**)s_dogFrames, 15);
    lv_animimg_set_duration(dog, 3600);   // 15 frames over ~3.6s, ~240ms/frame — a slower, contemplative look-around
    lv_animimg_set_repeat_count(dog, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(dog);

    // Info block stacked directly under the 130px-tall animation (which ends
    // ~y=146): wordmark, then version/uptime/date as a compact "about" group —
    // this is the one screen in the app with real "about" framing.
    lv_obj_t* titleLbl = lv_label_create(s_bitsIdleScr);
    lv_obj_set_style_text_font(titleLbl, &outfit_bold_18, 0);
    lv_obj_set_style_text_color(titleLbl, COLOR_INK, 0);
    lv_label_set_text(titleLbl, "BarkBoard");
    lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, 150);

    // BARKBOARD_VERSION is stamped in at build time from `git describe`
    // (tools/get_version.py, see platformio.ini's extra_scripts).
    lv_obj_t* versionLbl = lv_label_create(s_bitsIdleScr);
    lv_obj_set_style_text_font(versionLbl, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(versionLbl, COLOR_MUTED, 0);
    lv_label_set_text(versionLbl, BARKBOARD_VERSION);
    lv_obj_align(versionLbl, LV_ALIGN_TOP_MID, 0, 176);

    s_bitsIdleStat = lv_label_create(s_bitsIdleScr);
    lv_obj_set_style_text_font(s_bitsIdleStat, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_bitsIdleStat, COLOR_MUTED, 0);
    lv_obj_set_style_text_align(s_bitsIdleStat, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_bitsIdleStat, LV_ALIGN_TOP_MID, 0, 194);

    s_bitsIdleDate = lv_label_create(s_bitsIdleScr);
    lv_obj_set_style_text_font(s_bitsIdleDate, &outfit_thin_12, 0);
    lv_obj_set_style_text_color(s_bitsIdleDate, COLOR_MUTED, 0);
    lv_obj_align(s_bitsIdleDate, LV_ALIGN_TOP_MID, 0, 212);

    lv_timer_create(bitsIdleTick, 1000, nullptr);
}

void ui::showBitsIdle() {
    buildBitsIdleIfNeeded();
    s_screen = ui::Screen::BitsIdle;
    lv_scr_load_anim(s_bitsIdleScr, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
}

// ---- Rotation + gestures ----

static void setActiveDots(int idx) {
    for (int i = 0; i < DASH_COUNT; ++i) {
        if (s_dashDots[idx][i]) {
            lv_obj_set_style_bg_color(s_dashDots[idx][i], (i == idx) ? COLOR_PURPLE : COLOR_BORDER, 0);
        }
    }
}

// Sets whichever screen's fetch-pending flag corresponds to idx — shared by
// rotateTo() (on navigating there) and refreshCurrentDash() (re-fetching in
// place, no navigation, for the swipe-down gesture). Overview has no fetch
// of its own to trigger here: its counters ride the ambient poll in
// netTask(), which is already always running regardless of which screen is
// visible, so there's nothing to force early.
static void triggerFetchForDash(int idx) {
    if (idx == DASH_MONITORS)  requestMonitorsFetch(dd::getMonitorFilter().c_str());
    if (idx == DASH_INCIDENTS) {
        portENTER_CRITICAL(&s_pendingMux);
        s_incidentsFetchPending = true;
        portEXIT_CRITICAL(&s_pendingMux);
    }
    if (idx == DASH_ONCALL) {
        portENTER_CRITICAL(&s_pendingMux);
        s_oncallFetchPending = true;
        portEXIT_CRITICAL(&s_pendingMux);
    }
    if (idx == DASH_SLO) {
        portENTER_CRITICAL(&s_pendingMux);
        s_sloFetchPending = true;
        portEXIT_CRITICAL(&s_pendingMux);
    }
    if (idx == DASH_BITS) {
        portENTER_CRITICAL(&s_pendingMux);
        s_bitsInvestigationsFetchPending = true;
        portEXIT_CRITICAL(&s_pendingMux);
    }
}

static void rotateTo(int idx, bool /*fromUser*/) {
    idx = ((idx % DASH_COUNT) + DASH_COUNT) % DASH_COUNT;
    if (!s_dashScr[idx]) return;
    if (lv_scr_act() == s_dashScr[idx]) return;

    setActiveDots(idx);

    bool onDash = (s_screen == ui::Screen::Overview || s_screen == ui::Screen::Monitors ||
                   s_screen == ui::Screen::Incidents || s_screen == ui::Screen::OnCall ||
                   s_screen == ui::Screen::Slo || s_screen == ui::Screen::Bits);
    if (!onDash) {
        lv_scr_load_anim(s_dashScr[idx], LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    } else {
        lv_scr_load_anim_t anim = (idx > s_dashIdx) ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT;
        lv_scr_load_anim(s_dashScr[idx], anim, 200, 0, false);
    }
    s_dashIdx = idx;
    static const ui::Screen SCR_FOR_IDX[DASH_COUNT] = {
        ui::Screen::Overview, ui::Screen::Monitors, ui::Screen::Incidents, ui::Screen::OnCall, ui::Screen::Slo, ui::Screen::Bits
    };
    s_screen = SCR_FOR_IDX[idx];
    triggerFetchForDash(idx);
}

// Swipe-down-to-refresh: re-fetches whatever's on the CURRENT dashboard
// screen without navigating anywhere — same fetch-pending flags and busy-
// spinner UX triggerFetchForDash()'s caller already gets on first
// navigation, just re-armed on demand. Deliberately not a periodic "refresh
// everything always" poll: that would mean fetching Monitors/On-Call/SLOs/
// Bits Investigations data on a timer regardless of whether anyone's
// looking at them, burning API calls and battery for screens that are
// off-screen — an on-demand gesture for the one screen actually visible is
// the cheaper trade.
static void refreshCurrentDash() {
    triggerFetchForDash(s_dashIdx);
}

// Advances to the next dashboard screen automatically, only when the user's
// turned it on (storage::getAutoRotateEnabled(), off by default) and only
// while actually sitting on one of the six dashboard screens — never fires
// from a detail/settings/idle screen, so it can't yank someone out of
// Monitor Detail or the Bits idle screen mid-read.
static void autoRotateTick(lv_timer_t*) {
    if (!storage::getAutoRotateEnabled()) return;
    bool onDash = (s_screen == ui::Screen::Overview || s_screen == ui::Screen::Monitors ||
                   s_screen == ui::Screen::Incidents || s_screen == ui::Screen::OnCall ||
                   s_screen == ui::Screen::Slo || s_screen == ui::Screen::Bits);
    if (!onDash) return;
    rotateTo(s_dashIdx + 1, false);
}

static void onDashGesture(lv_event_t*) {
    lv_indev_t* in = lv_indev_get_act();
    if (!in) return;
    lv_dir_t d = lv_indev_get_gesture_dir(in);
    if (d == LV_DIR_LEFT)        rotateTo(s_dashIdx + 1, true);
    else if (d == LV_DIR_RIGHT)  rotateTo(s_dashIdx - 1, true);
    else if (d == LV_DIR_BOTTOM) refreshCurrentDash();
}

// Per-screen breakdown of buildDashboard()'s LVGL pool cost — lv_mem_monitor()
// only reports pool-wide aggregates (LVGL 8.x's TLSF allocator has no
// per-object "who owns this" introspection), so this is the finest-grained
// answer available: print the pool's free bytes right after each build*()
// call, so the *drop* since the previous checkpoint is that screen's own
// resident cost. Diagnostic only, core 1 only (same lv_mem_monitor() safety
// note as logLvglMemTrend() below).
static void logMemCheckpoint(const char* label) {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("[ui] mem after %-22s | free=%u used_pct=%u%% frag=%u%%\n",
                  label, mon.free_size, mon.used_pct, mon.frag_pct);
}

static void buildDashboard() {
    logMemCheckpoint("boot (pre-dashboard)");
    for (int i = 0; i < DASH_COUNT; ++i) s_dashScr[i] = makeDashScreen(i);
    logMemCheckpoint("makeDashScreen x6");
    buildOverview();
    logMemCheckpoint("buildOverview");
    buildMonitors();
    logMemCheckpoint("buildMonitors");
    buildIncidents();
    logMemCheckpoint("buildIncidents");
    buildOnCall();
    logMemCheckpoint("buildOnCall");
    buildSlos();
    logMemCheckpoint("buildSlos");
    buildBitsInvestigations();
    logMemCheckpoint("buildBitsInvestigations");
    // Added last so they sit on top in z-order — see the comment in
    // makeDashScreen() for why that ordering matters here.
    for (int i = 0; i < DASH_COUNT; ++i) addNavArrows(s_dashScr[i], i);
    logMemCheckpoint("addNavArrows x6");
    s_dashBuilt = true;

    // Created once, checks storage::getAutoRotateEnabled() every tick —
    // see autoRotateTick()'s doc comment for why it's safe to just let this
    // run continuously rather than starting/stopping it per toggle.
    lv_timer_create(autoRotateTick, AUTO_ROTATE_INTERVAL_MS, nullptr);

    // One-time diagnostic, not per-frame: LVGL's own memory pool
    // (LV_MEM_SIZE, separate from the general ESP32 heap) is what every
    // screen transition draws a small allocation from. If it's running
    // close to full right after building every screen once, that's the
    // thing to raise, not the general heap.
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("[ui] dashboard built | LVGL pool: %u%% used, %u%% frag, %u bytes free (biggest block %u) | ESP heap free: %u\n",
                  mon.used_pct, mon.frag_pct, mon.free_size, mon.free_biggest_size, (unsigned)ESP.getFreeHeap());
}

void ui::showOverview() {
    if (!s_dashBuilt) buildDashboard();
    rotateTo(DASH_OVERVIEW, false);
}

// ============================================================================
// Shared status / lifecycle
// ============================================================================

void ui::begin() {
    lv_obj_set_style_bg_color(lv_scr_act(), COLOR_BG, 0);
    showConnecting("Booting...");
}

ui::Screen ui::currentScreen() { return s_screen; }

void ui::setStatusOnline(bool online) {
    s_online = online;
    if (s_statusDot) lv_obj_set_style_bg_color(s_statusDot, online ? COLOR_OK : COLOR_ALERT, 0);
    for (int i = 0; i < DASH_COUNT; ++i) {
        if (s_dashStatusDot[i]) lv_obj_set_style_bg_color(s_dashStatusDot[i], online ? COLOR_OK : COLOR_ALERT, 0);
    }
}

void ui::setClockText(const String& hhmm) {
    if (s_clock) lv_label_set_text(s_clock, hhmm.c_str());
    for (int i = 0; i < DASH_COUNT; ++i) {
        if (s_dashClock[i]) lv_label_set_text(s_dashClock[i], hhmm.c_str());
    }
}
