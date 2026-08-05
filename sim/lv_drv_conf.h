// lv_drivers config for the BarkBoard simulator only — enables the SDL2
// display/mouse backend (LVGL 8.x's lv_drivers "sdl" driver, NOT LVGL 9's
// built-in lv_sdl_window API) at the CYD panel's native 320x240, zoomed 2x
// so a screen recording is legible. This file is sim-only; it has no
// counterpart in the real ESP32 firmware, which uses TFT_eSPI/XPT2046
// instead (see src/display.cpp).
#ifndef LV_DRV_CONF_H
#define LV_DRV_CONF_H

#include "lv_conf.h"

#define LV_DRV_DELAY_INCLUDE  <stdint.h>
#define LV_DRV_DELAY_US(us)
#define LV_DRV_DELAY_MS(ms)

#define LV_DRV_DISP_INCLUDE         <stdint.h>
#define LV_DRV_DISP_CMD_DATA(val)
#define LV_DRV_DISP_RST(val)
#define LV_DRV_DISP_SPI_CS(val)
#define LV_DRV_DISP_SPI_WR_BYTE(data)
#define LV_DRV_DISP_SPI_WR_ARRAY(adr, n)
#define LV_DRV_DISP_PAR_CS(val)
#define LV_DRV_DISP_PAR_SLOW
#define LV_DRV_DISP_PAR_FAST
#define LV_DRV_DISP_PAR_WR_WORD(data)
#define LV_DRV_DISP_PAR_WR_ARRAY(adr, n)

#define LV_DRV_INDEV_INCLUDE     <stdint.h>
#define LV_DRV_INDEV_RST(val)
#define LV_DRV_INDEV_IRQ_READ    0
#define LV_DRV_INDEV_SPI_CS(val)
#define LV_DRV_INDEV_SPI_XCHG_BYTE(data)    0
#define LV_DRV_INDEV_I2C_START
#define LV_DRV_INDEV_I2C_STOP
#define LV_DRV_INDEV_I2C_RESTART
#define LV_DRV_INDEV_I2C_WR(data)
#define LV_DRV_INDEV_I2C_READ(last_read)    0

#define USE_SDL 1
#define USE_SDL_GPU 0
#if USE_SDL || USE_SDL_GPU
#  define SDL_HOR_RES     320
#  define SDL_VER_RES     240
#  define SDL_ZOOM        2
#  define SDL_DOUBLE_BUFFERED 0
// sdl2-config's -I flag (see Makefile's SDL_CFLAGS) already points inside
// the SDL2/ folder itself (Homebrew installs headers at
// /opt/homebrew/include/SDL2/SDL.h), so the bare form resolves correctly on
// macOS. If you vendor SDL2 differently, this may need to be <SDL2/SDL.h>.
#  define SDL_INCLUDE_PATH    <SDL.h>
#  define SDL_DUAL_DISPLAY    0
#endif

#define USE_MONITOR 0

#endif /* LV_DRV_CONF_H */
