# BarkBoard — build log

BarkBoard leans on an LLM-driven coding workflow for most of the implementation, with real hardware in the loop the whole way — every fix below was verified against an actual board, not just "it compiles." A running list of the bugs that were interesting enough to write down, in the order they came up. See [ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit together.

## The UI froze, but the network kept working

**Symptom:** tabbing to the SLOs or On-Call screen would occasionally freeze the whole UI solid — touch stopped responding, no crash, no reboot — while the serial log showed Datadog API calls still firing normally in the background.

That last detail was the key clue. If the network task (core 0) were stuck, the API calls would've stopped too. They didn't — only the UI task (core 1) had died, silently, without anyone noticing.

The board's memory pool for the UI library (LVGL) is a fixed 64KB carved out of a much smaller total SRAM budget (this board has no PSRAM). When that pool runs out, the library's default behavior — completely reasonably, for a library that assumes you're checking allocation failures yourself — is to spin in an infinite loop rather than corrupt memory further. With logging turned off (the default, to save flash and CPU), that loop is entirely silent. And this specific board's watchdog timer only checks whether _core 0's_ idle task is running, not core 1's — so nothing ever caught it and reset the board.

The actual leak: a scrolling "marquee" effect on long list-item titles, which needed two extra style properties per label. Multiply that by every row across six screens kept resident in memory at once, and it was just enough to tip a pool that turned out to already be running near its ceiling. Turning on LVGL's logging (at a quiet warning level) surfaced an explicit `Out of memory` message right before the freeze — confirming the theory in one line, instead of guessing. The fix was to drop the marquee scroll for plain text truncation. Simpler, and it doesn't cost any extra memory per row.

The board's desktop simulator, notably, could never have caught this — it's deliberately given a much larger memory pool since desktop RAM doesn't have the same constraint. Some bugs only exist because of a $10 chip's actual limits.

## A JSON filter that silently returned nothing

**Symptom:** an API response that should have contained real data came back completely empty — no error, no exception, just zero results, indistinguishable from "there's genuinely nothing here."

To keep memory usage down, BarkBoard uses a JSON parsing library feature that filters out anything you don't need _while_ parsing, rather than parsing the whole response and then discarding fields. For a few specific response shapes — deeply nested arrays of objects — that filter quietly matched zero items instead of erroring, even though it was built exactly the way the library's own documentation describes.

Rather than chase the exact mechanism through the library's internals, the fix was pragmatic: parse those specific responses without the filter. It costs a little more memory for those particular calls, but "definitely correct" beat "efficient but silently wrong" — especially for a bug that gives no error to grep for.

## An HTTP buffer had an undocumented ceiling

**Symptom:** a working request started failing intermittently after making it fetch bigger pages — parse errors that looked like a JSON syntax problem, with no changes to how the JSON itself was built.

The actual issue lived one layer down, in how the HTTP client grows its receive buffer as data streams in. Past a certain response size, that growth started failing outright — a buffer allocation problem, not a network or JSON problem, but the resulting error messages looked exactly like corrupted JSON. Once traced to the actual layer, the fix was straightforward: keep individual page sizes small, and lean harder on filtering server-side (by team, by tag) so responses stay small in the first place rather than fetching a big page and trimming it down after the fact.

## Two different ways a "monitor" can define its own query

**Symptom:** chart data worked fine for metric-based monitor alerts, but came back empty for monitors built on logs, traces, or user-session data.

It turned out Datadog monitors can express their underlying search two structurally different ways depending on when/how they were created — one style embeds the search directly as a string inside the monitor's query field, the other stores it in a separate structured field entirely. A monitor that used the newer style simply had no embedded string to extract at all, so the code went looking in the wrong place and came up empty. Once both real shapes were confirmed against actual monitors in a live org, the fix was to check the structured field first and fall back to the older embedded style — covering both eras.

## A missing character in the font, times a few thousand

**Symptom:** after turning on debug logging to chase the freeze above, one specific warning appeared over and over, dozens of times a second — a "glyph not found" message for a single character.

The board renders text using a small, pre-baked bitmap font rather than a general-purpose one (again, memory) — so only a chosen subset of characters actually exists as drawable glyphs. One label used a typographic em dash rather than a plain hyphen, and every single time that label redrew — which, mid-animation, is a lot — the renderer logged a warning for the character it didn't have a picture for. Harmless to functionality, real as noise; a good reminder that "looks fine in an editor" and "exists in this specific compiled font" are two different questions on hardware this constrained.
