# BarkBoard

A Datadog dashboard for the **ESP32-2432S028R v2** — the ~$10 "Cheap Yellow Display" (CYD) board — showing live monitors, incidents, on-call, and SLOs on a 2.8" touchscreen sitting on your desk. It's modeled on [justynroberts/pagerduty-cyd](https://github.com/justynroberts/pagerduty-cyd), reusing its LVGL/touch UI patterns and hardware setup but pointed at the Datadog API instead of PagerDuty's — with its own dedicated core-0 FreeRTOS task for every Datadog API call, so the touchscreen stays responsive while a fetch is in flight.

This README is the fast on-ramp; the fuller architecture/screen-by-screen spec and the color/typography/layout tokens exist as internal design docs alongside the project's dev-only reference material, not published in this repo.

## Flash it in your browser (no toolchain)

The easiest way to try BarkBoard: flash it straight from Chrome or Edge at this repo's GitHub Pages URL (**Settings → Pages** on GitHub shows the exact address once Pages is enabled — typically `https://<owner>.github.io/<repo>/`) — no PlatformIO, no compiling, just plug in a USB-C cable and click a button. The page lives at [`docs/index.html`](./docs/index.html), built on [ESP Web Tools](https://esphome.github.io/esp-web-tools/), and its firmware binaries are rebuilt automatically from `main` by [`.github/workflows/pages.yml`](./.github/workflows/pages.yml). Requires desktop Chrome or Edge (WebSerial isn't available in Firefox, Safari, or on mobile).

## What you'll need

- An **ESP32-2432S028R v2 "CYD"** board (the USB-C revision) and a USB-C cable that carries data, not just power.
- A **Datadog org** (a free trial works) with:
  - an **API Key** (_Organization Settings → API Keys_)
  - a **scoped Application Key** (_Organization Settings → Application Keys_) with read access to **Monitors, Incidents, On-Call, SLOs, and Hosts**. Datadog's scope list can shift — double-check the exact names in the UI when you create the key rather than trusting this list verbatim.
- **PlatformIO** (CLI or the VS Code extension) — only if you want to build and flash locally instead of using the browser flasher above.

## Quick start (local build via the Makefile)

```sh
make build   # compile only — pio run -e cyd, same check CI runs
make flash   # upload to the board, then drop into the serial monitor
```

`platformio.ini` hardcodes a default `upload_port`/`monitor_port`, which drifts every time the board is unplugged (macOS renumbers `/dev/cu.usbserial-*`). The Makefile auto-detects the first connected USB-serial device, so you usually don't need to touch it — override explicitly if you have more than one serial device attached:

```sh
make flash PORT=/dev/cu.usbserial-1420
```

Other useful targets: `make monitor` (serial monitor only), `make erase-flash` (wipe the whole flash chip — WiFi creds, Datadog keys, detected site, everything in NVS — for a clean-slate test), `make reflash` (erase-flash + upload + monitor in one go), and `make clean`. Run `make help` for the full list, including `fetch-monitors`/`fetch-incidents`/ `fetch-oncall`/`fetch-slos` — debugging targets that hit the live Datadog API with the same query shapes the firmware uses, no flashing required.

## First boot: setup flow

1. **Join the setup network.** On first boot (or after a factory reset), the board broadcasts an open WiFi network named **`BarkBoard-XXXX`** (last 4 hex digits of its MAC address) — no password. Join it from your phone or laptop.
2. **Pick your home WiFi.** A captive portal opens automatically (or visit `http://192.168.4.1` directly) with a scanned list of nearby 2.4GHz networks. Choose one and enter its password. The board disconnects its own AP and joins your network.
3. **Open the setup page.** Once connected, reach the device at `http://barkboard.local` (mDNS) or the IP address shown on the panel.
4. **Enter your Datadog keys.** The setup page asks for just two fields — **API Key** and **Application Key** — with a show/hide toggle for pasting from a password manager. There's no site/region dropdown: the device probes the known Datadog site hosts itself (`GET /api/v2/validate_keys` against each) and stores whichever one validates. Typical case (US1 or EU) resolves in a couple of seconds; worst case (mistyped keys, or an org on the last host tried) can take up to ~25 seconds while it works through the list — the setup page shows progress rather than going blank.
5. **Optional preferences**, same page: a team filter (scopes Monitors/Incidents/SLOs to one team/tag instead of showing a whole org — leave it blank and the device auto-detects your team from your API key, same as On-Call's own team picker below it), 12h/24h clock format, and a status-LED style (breathing cyan vs. solid green when healthy — the board's red LED channel doesn't light on this hardware batch, confirmed across multiple physical units, so mood colors are built from green/blue only for now).

A **"Forget WiFi & keys"** button on the same setup page resets both WiFi credentials and Datadog keys and reboots into a fresh portal session — the in-app equivalent of holding the touchscreen ~2s at boot.

## Screens

Five rotating dashboards (swipe or tap the page dots, auto-rotates every 10s), plus drill-in detail views:

| Screen        | What's on it                                                                                                         |
| ------------- | -------------------------------------------------------------------------------------------------------------------- |
| **Overview**  | Big ALERT/WARN/OK monitor counts, open incident count + highest severity                                             |
| **Monitors**  | Filterable list (`ALL / ALERT / WARN / NO DATA`); tap a row for detail — live metric sparkline, mute/unmute (long-press to unmute), declare a case/incident, trigger a Bits investigation |
| **Incidents** | Active incidents with SEV-1..5 badges; tap for detail and to cycle incident state                                    |
| **On-Call**   | Current on-call + escalation roster for your team, auto-detected from your API key (or picked on the web Settings page if your key belongs to more than one team) |
| **SLOs**      | Configured SLOs; tap one for an arc gauge showing remaining error budget                                             |

Plus a Settings screen (WiFi/site info, re-detect tenant, factory reset) and an animated Easter-egg/idle-screensaver screen. Full detail, API endpoints, and payload shapes for each screen live in the internal design doc mentioned above.

## Hardware gotchas

- **Panel driver is `ILI9341_2_DRIVER`, not `ST7789_DRIVER`.** Two separate physical boards booted clean (WiFi, portal, NVS all fine) but showed a permanently blank white screen until this was corrected — verified against [witnessmenow/ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)'s maintained `User_Setup.h`, not older writeups (including this repo's own `_reference/pagerduty-cyd`).
- **`USE_HSPI_PORT` is required.** Without it, TFT_eSPI's default VSPI on plain ESP32 collides with the touch controller's explicit `SPIClass(VSPI)` — a real hardware conflict, not a config nit.
- **The panel is dim and color-shifts off-axis.** View it straight-on; don't judge contrast or legibility from an angle. The color palette leans toward higher contrast/saturation specifically to cope with this.
- **The onboard RGB status LED's red channel (GPIO4) doesn't light**, confirmed across multiple physical boards using the board vendor's own bare-metal test sketch — independent of anything in this firmware. Mood colors are built from green/blue only until a hardware fix is found; see `tools/ledtest/` for the isolated test.

Both driver flags are already set correctly in `platformio.ini` — this is here so you don't "fix" them back to what older CYD guides assume.

## Verifying changes

`pio run -e cyd` is the only thing a sandboxed coding agent (or this repo's CI, [`.github/workflows/build.yml`](./.github/workflows/build.yml)) can actually check — it cannot flash, exercise touch/WiFi/display behavior, or call the live Datadog API. See [`CLAUDE.md`](./CLAUDE.md) for the full set of house rules and what still needs a human on real hardware before calling anything "done."

## License

See [`LICENSE`](./LICENSE).
