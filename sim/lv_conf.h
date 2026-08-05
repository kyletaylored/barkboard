#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// --- sim-only deviations from the real include/lv_conf.h ---
// LV_COLOR_16_SWAP: this matters for byte order over SPI to the real ILI9341
// panel; the SDL simulator draws straight into a normal desktop framebuffer,
// so it must be OFF here (leaving it on inverts every color on screen).
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_COLOR_SCREEN_TRANSP 0

#define LV_MEM_CUSTOM 0
// The real board's 64KB ceiling (see include/lv_conf.h's comment) is an
// embedded DRAM constraint that doesn't exist on desktop — sized up here
// just to have slack; this is the sim's own copy, not a change to the real
// board's config.
#define LV_MEM_SIZE (512U * 1024U)
#define LV_MEM_ADR 0
#define LV_MEM_BUF_MAX_NUM 16

#define LV_DISP_DEF_REFR_PERIOD 33
#define LV_INDEV_DEF_READ_PERIOD 33
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#define LV_DPI_DEF 130

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_USE_USER_DATA 1

#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(outfit_bold_14)
#define LV_FONT_DEFAULT &outfit_bold_14

#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX 0

#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
#define LV_USE_THEME_BASIC 1
#define LV_USE_THEME_MONO 0

/* Widgets */
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 0
/* CANVAS: on in this fork (off in the pagerduty-cyd reference) — LVGL's
 * built-in lv_qrcode widget draws onto a canvas internally, and it also
 * backs the host-status-grid screen if that ends up rendered as a canvas
 * rather than a matrix of lv_obj rects. See BARKBOARD_PLAN.md §3.2 / §5. */
#define LV_USE_CANVAS 1
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_LABEL_TEXT_SELECTION 0
#define LV_LABEL_LONG_TXT_HINT 1
#define LV_USE_LINE 1
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 0
#define LV_USE_SWITCH 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 0

/* Extra widgets */
/* ANIMIMG: on in this fork — plays a sequence of lv_img sources on a timer,
 * exactly what the Easter-egg / idle-screensaver animated Bits mascot needs
 * (see BARKBOARD_PLAN.md §4 "Easter egg screen"). Pair with lv_anim on the
 * same image object (zoom/angle) for bounce/wag motion on top of the frame
 * swap. */
#define LV_USE_ANIMIMG 1
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 1
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LIST 1
#define LV_USE_MENU 0
#define LV_USE_METER 0
#define LV_USE_MSGBOX 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 1
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

/* 3rd-party / misc libs */
/* QRCODE: on in this fork — used for the "join this AP" and "open the setup
 * page" QR codes during first-time setup (see BARKBOARD_PLAN.md §3.2).
 * Requires LV_USE_CANVAS above. */
#define LV_USE_QRCODE 1

#define LV_USE_FLEX 1
#define LV_USE_GRID 1

#define LV_USE_ANIMATION 1
#define LV_USE_SHADOW 1
#define LV_USE_BLEND_MODES 0
#define LV_USE_OPA_SCALE 0
#define LV_USE_IMG_TRANSFORM 1
#define LV_USE_GROUP 1
#define LV_USE_GPU 0
#define LV_USE_FILESYSTEM 0

#define LV_USE_API_EXTENSION_V6 0
#define LV_USE_API_EXTENSION_V7 0

#endif /* LV_CONF_H */
