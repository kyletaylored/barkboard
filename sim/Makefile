# BarkBoard LVGL desktop simulator — a brand-new, separate build, NOT wired
# into platformio.ini and not part of the `pio run -e cyd` ESP32 build.
# Renders the exact same src/ui.cpp screens LVGL 8.3.11 + lv_drivers' SDL2
# backend so they can be screen-recorded for demos without the physical CYD
# panel. See README.md for prerequisites and usage.

CXX := c++
CC  := cc

ROOT        := ..
SRC_DIR     := $(ROOT)/src
SHIM_DIR    := arduino_shim
LVGL_DIR    := vendor/lvgl
LVDRV_DIR   := vendor/lv_drivers

SDL_CFLAGS  := $(shell sdl2-config --cflags)
SDL_LIBS    := $(shell sdl2-config --libs)

INCLUDES := \
    -I. \
    -I$(SHIM_DIR) \
    -I$(SRC_DIR) \
    -I$(LVGL_DIR) \
    -I$(LVDRV_DIR) \
    $(SDL_CFLAGS)
# NOTE: deliberately NOT adding $(LVDRV_DIR)/sdl to the include path — macOS's
# default case-insensitive filesystem makes `#include "sdl.h"` / `<SDL.h>`
# ambiguous with SDL2's own SDL.h once that directory is searchable, and
# clang silently picks whichever one appears first. main_sim.cpp includes
# lv_drivers' sdl.h via its full relative path instead (see below) to avoid
# the collision entirely; sdl.c/sdl_common.c resolve their own "sdl.h" via
# the same-directory quoted-include rule, which doesn't need this path either.

COMMON_DEFS := -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE

CXXFLAGS := -std=c++17 -g -O0 -Wall -Wno-unused-parameter $(COMMON_DEFS) $(INCLUDES)
CFLAGS   := -std=c11   -g -O0 -Wall -Wno-unused-parameter $(COMMON_DEFS) $(INCLUDES)

BUILD_DIR := build

# ---- BarkBoard sources reused verbatim from ../src -------------------------
CXX_SRCS := \
    $(SRC_DIR)/ui.cpp \
    datadog_sim.cpp \
    storage_sim.cpp \
    main_sim.cpp \
    $(SHIM_DIR)/WString.cpp \
    $(SHIM_DIR)/stdlib_noniso.cpp \
    $(SHIM_DIR)/arduino_shim.cpp

C_SRCS := \
    $(SRC_DIR)/assets_gen.c \
    $(wildcard $(SRC_DIR)/fonts/*.c) \
    $(SHIM_DIR)/millis.c

# ---- LVGL 8.3.11 core (vendored at sim/vendor/lvgl, matches lvgl@~8.3.11 in
# platformio.ini) ------------------------------------------------------------
LVGL_C_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c')

# ---- lv_drivers' SDL backend (vendored at sim/vendor/lv_drivers,
# release/v8.3 branch — the LVGL 8.x sdl display+mouse driver, distinct from
# LVGL 9's built-in lv_sdl_window). sdl_gpu.c is excluded: it needs
# LV_USE_GPU_SDL/SDL_gpu, which we don't enable (USE_SDL_GPU=0 in
# lv_drv_conf.h already guards its body out, but its header pulls in
# SDL_gpu.h unconditionally, which most SDL2 installs don't have). ----------
LVDRV_C_SRCS := $(LVDRV_DIR)/sdl/sdl.c $(LVDRV_DIR)/sdl/sdl_common.c

ALL_C_SRCS   := $(C_SRCS) $(LVGL_C_SRCS) $(LVDRV_C_SRCS)
ALL_CXX_SRCS := $(CXX_SRCS)

# Strip the leading "../" from ../src/... sources before remapping under
# $(BUILD_DIR) — without this, patsubst would ask mkdir to create
# build/../src, which (while harmless — it stays inside sim/) is confusing
# to read; this keeps every object cleanly nested under sim/build/.
toobj_c   = $(BUILD_DIR)/$(subst ../,,$(patsubst %.c,%.o,$(1)))
toobj_cpp = $(BUILD_DIR)/$(subst ../,,$(patsubst %.cpp,%.o,$(1)))

C_OBJS   := $(foreach f,$(ALL_C_SRCS),$(call toobj_c,$(f)))
CXX_OBJS := $(foreach f,$(ALL_CXX_SRCS),$(call toobj_cpp,$(f)))

TARGET := $(BUILD_DIR)/barkboard_sim

.PHONY: all clean run
all: $(TARGET)

$(TARGET): $(C_OBJS) $(CXX_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(C_OBJS) $(CXX_OBJS) -o $@ $(SDL_LIBS)

$(BUILD_DIR)/src/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR)
