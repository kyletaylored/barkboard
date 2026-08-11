# BarkBoard — how it works

A quick tour of what's actually running on the board, for anyone poking around the source. For setup/usage, see the [README](../README.md).

## Hardware

ESP32-2432S028R — the "Cheap Yellow Display" (CYD). 320×240 ILI9341 panel, resistive touch, an RGB LED, and a piezo buzzer, all driven from a single ESP32-WROOM module with **no PSRAM**. That last point matters more than it sounds like it should: every screen, every list row, every animation lives in a 64KB static memory pool carved out of ~320KB of total SRAM, and that ceiling shapes a lot of the decisions below.

## The two-core split

The ESP32 is dual-core, and BarkBoard uses that deliberately:

- **Core 0** runs a dedicated FreeRTOS task that does all networking — WiFi, TLS, and every Datadog API call.
- **Core 1** runs LVGL (the UI library) and touch input, via the normal Arduino `loop()`.

These never call into each other directly. LVGL's internal state isn't thread-safe, so touching a UI widget from the network task is a reliable way to freeze or crash the board. Instead, the network task writes results into a couple of small, mutex-guarded structs, and the UI task drains them each frame. It's a small amount of extra bookkeeping in exchange for a UI that never stalls waiting on a slow HTTPS handshake.

## Setup flow

No SSID/password/API-key fields to type on a touchscreen with no keyboard. Instead:

1. First boot broadcasts an open WiFi network (`BarkBoard-XXXX`). Join it from a phone or laptop.
2. A captive portal (a hand-rolled `WebServer` + `DNSServer`, not a third-party library — those turned out to be flaky on this exact board) lets you pick your home network.
3. Once online, a small web form at that device's own `barkboard-<4-hex-chars>.local` address (unique per device — MAC-suffixed so two boards on one LAN don't collide; shown on the panel, or scan the QR code in Settings) asks for just two things: a Datadog **API Key** and **Application Key**.
4. The board itself figures out which of Datadog's ~9 regional sites (US1, US3, EU, etc.) those keys belong to, by probing `validate_keys` against each one in turn. You never have to know or type your org's region.

Everything after that — team scope, on-call team, clock format, LED style, opt-in device metrics — lives on the same settings page, served from the board itself.

## Screens

Six rotating dashboards (swipe or tap the page dots), each with its own drill-down detail view:

| Screen                  | What it shows                                                                                                                                           |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Overview**            | ALERT/WARN/OK monitor counts, open incident count, quick-nav tiles                                                                                      |
| **Monitors**            | Filterable list; tap in for a live chart (metric, log, trace, or RUM — whichever the monitor actually queries), mute/unmute, declare a case or incident |
| **Incidents**           | Active incidents with severity badges; tap to see detail and advance state                                                                              |
| **On-Call**             | Who's on call right now, plus the escalation policy behind them                                                                                         |
| **SLOs**                | Configured SLOs; tap one for an error-budget arc gauge                                                                                                  |
| **Bits Investigations** | Datadog's AI-driven root-cause investigations, filtered to your team                                                                                    |

## Talking to Datadog

Every list-fetching call is scoped server-side (team/tag filters, small page sizes) rather than pulling everything and filtering on the device — both because Datadog orgs can be large, and because the board's tiny memory pool can't hold much of a response anyway.

Monitor charts are the one place where "which API do I even call" isn't obvious: a monitor's query can be a plain metric query, or it can be built on logs, APM traces, or RUM events — each of those needs a completely different Datadog endpoint and a different way of extracting the underlying search from the monitor definition. BarkBoard detects which kind a monitor is and routes to the matching Aggregate API automatically.

## Self-monitoring (opt-in)

BarkBoard can report its own health back to the same Datadog org it's dashboarding — heap, per-task stack headroom, WiFi signal, uptime, and hardware facts as tags, plus a Datadog Event if the board's previous boot ended in a crash or watchdog reset rather than a clean restart. Off by default (custom metrics and Events are billable Datadog usage), one toggle away in Settings.

## Want the deeper story?

The [build log](CHALLENGES.md) walks through the specific bugs (an LVGL memory pool exhaustion, a silent JSON-parsing filter bug, an HTTP buffer size ceiling, and more) that shaped several of the decisions above.
