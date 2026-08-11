# BarkBoard LVGL simulator

A separate, standalone desktop build that runs the **exact same** LVGL screen-building code from `../src/ui.cpp` inside an LVGL 8.3.11 + SDL2 window on your Mac/Linux box, instead of the real ESP32 + TFT_eSPI/XPT2046 hardware. Useful for recording demo videos/screenshots of the dashboard without the physical CYD panel (which is also dim and color-shifts off-axis — see the project's `CLAUDE.md`).

This directory is **not** part of the ESP32 firmware build. It's never referenced by `../platformio.ini`, and nothing here is linked into `pio run -e cyd`.

## What's real vs. fake

- **Real, unmodified**: `../src/ui.cpp` — every screen you see is built by the actual production UI code, fonts (`../src/fonts/`), and the Bits mascot sprite assets (`../src/assets_gen.c`).
- **Fake, simulator-only**:
  - `datadog_sim.cpp` — stands in for `../src/datadog.cpp`. Implements the same `dd::` API declared in `../src/datadog.h`, but returns hardcoded monitors/incidents/SLOs/on-call data instead of making any network call. The real `datadog.cpp` depends on `WiFiClientSecure`/`HTTPClient`/`WiFi.h` and can't be linked into a desktop build at all.
  - `storage_sim.cpp` — stands in for `../src/storage.cpp` (which needs ESP32 NVS via `<Preferences.h>`). In-memory only; resets every run.
  - `main_sim.cpp` — stands in for `../src/main.cpp`'s hardware bring-up (WiFi, captive portal, TFT_eSPI/XPT2046 init). Initializes LVGL + the SDL2 display/mouse drivers, seeds the fake `dd::` data, and calls `ui::begin()` / `ui::showOverview()`.
  - `arduino_shim/` — a minimal Arduino-compatibility layer so `ui.cpp` compiles unmodified on desktop gcc/clang. Contains a vendored, unmodified copy of the ESP32 Arduino core's `WString.h`/`WString.cpp`/`pgmspace.h` (pure C++, no hardware dependency) plus a hand-written `Arduino.h` (millis(), a stand-in `Serial`/`WiFi`/`ESP` for the one status line that reads them) and desktop reimplementations of the non-ISO string/number conversion helpers (`itoa`/`dtostrf`/etc.) that `WString.cpp` calls and that aren't in macOS/BSD libc.

## Prerequisites

- A C++17 compiler (clang or gcc) and `make`.
- SDL2, with `sdl2-config` on your `PATH`. On macOS:
  ```
  brew install sdl2
  ```
On Debian/Ubuntu: `sudo apt install libsdl2-dev`.
- `sim/vendor/lvgl` (LVGL v8.3.11, matching `lvgl@~8.3.11` pinned in `../platformio.ini`) and `sim/vendor/lv_drivers` (the `release/v8.3` branch — LVGL 8.x's SDL driver lives in this separate repo, NOT LVGL 9's built-in `lv_sdl_window`). If `sim/vendor/` is empty (e.g. after a fresh clone that didn't include it), fetch them with:
  ```
  git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git sim/vendor/lvgl
  git clone --depth 1 --branch release/v8.3 https://github.com/lvgl/lv_drivers.git sim/vendor/lv_drivers
  ```

## Build & run

```
cd sim
make -j
./build/barkboard_sim
```

or just `make run`.

A window opens at 320x240 (the CYD panel's native resolution), zoomed 2x (640x480) for legibility — see `SDL_ZOOM` in `lv_drv_conf.h` if you want a different size. It lands on the Overview dashboard screen with canned monitor/incident/SLO/on-call data already populated, and click-and-drag with the mouse works the same way touch does on the real panel (tap the status bar's gear for Settings, tap Bits' mark for the idle screen, etc.) — this is what a screen recorder should capture.

Quit with Ctrl+C in the terminal.

## WebAssembly build (for the web flasher's "Live Demo")

`Makefile.wasm` cross-compiles the exact same sources above with Emscripten
instead of native clang/gcc, targeting a `<canvas>` via Emscripten's own
SDL2 port (`-sUSE_SDL=2`) rather than a real window — no native SDL2
install needed for this build. It's what backs the "Live Demo" section on
the GitHub Pages flasher (`../docs/index.html`).

```
brew install emscripten   # or any emsdk install, as long as emcc is on PATH
cd sim
make -f Makefile.wasm -j
```

Outputs `../docs/sim/barkboard_sim.js` + `.wasm` directly (not `build_wasm/`,
which is just intermediate object files). Those two files are gitignored —
like the root `docs/*.bin`s, they're rebuilt fresh into `docs/sim/` by
`.github/workflows/pages.yml` on every push, not committed.

The one code difference from the native build: `main_sim.cpp`'s event loop
is `#ifdef __EMSCRIPTEN__`-gated to use `emscripten_set_main_loop` instead
of a blocking `while(1)` + `SDL_Delay` — blocking the browser's JS thread
that way would freeze the tab, since this build has no pthreads.

## Known limitations (by design)

- No real network, no real WiFi state, no real Datadog data — everything `dd::` returns is hardcoded in `datadog_sim.cpp`. Edit that file to change what monitors/incidents/SLOs show up.
- `storage_sim.cpp` doesn't persist anything between runs — Settings changes made in the simulator are lost when you quit.
- Touch calibration, on-panel legibility/color-shift, and WiFi reconnect behavior are real-hardware concerns this simulator can't speak to at all — see the "Build / verify" section of the repo root `CLAUDE.md` for why.

## Verification notes from the build that produced this

- `make clean && make -j` builds and links cleanly (a couple of harmless `-Wunused-variable`/`-Wdeprecated-declarations` warnings from vendored third-party code, no errors).
- Running `./build/barkboard_sim` on a real macOS desktop (with a WindowServer present) opens the window, builds the full dashboard once (see the `[ui] dashboard built | LVGL pool: ...` line, which is `ui.cpp`'s own one-time diagnostic), and sits in the SDL event loop idling at ~0% CPU between frames — it does not crash or busy-loop.
- If you're running this in a sandboxed/headless environment with no display server, `SDL_Init(SDL_INIT_VIDEO)` may fail at startup — that's an environment limitation, not a build problem. The binary itself still builds and links fine either way.

