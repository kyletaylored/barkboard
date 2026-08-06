# BarkBoard — convenience wrapper around PlatformIO.
#
# platformio.ini hardcodes upload_port/monitor_port, which drifts every time
# the board is unplugged and replugged (macOS renumbers /dev/cu.usbserial-*).
# PORT here auto-detects the first connected USB-serial device so you don't
# have to hand-edit platformio.ini before every flash; override it explicitly
# if you have more than one serial device attached:
#   make flash PORT=/dev/cu.usbserial-1420

ENV  ?= cyd
PORT ?= $(firstword $(wildcard /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial*))

.PHONY: help build upload monitor flash clean port erase-flash reflash \
        fetch-monitors fetch-incidents fetch-oncall fetch-slos fetch-event \
        fetch-investigations fetch-investigation \
        flash-ledtest flash-3248s035-demo

help:
	@echo "BarkBoard build targets:"
	@echo "  make build       - compile only (pio run -e $(ENV)) — same check CI runs"
	@echo "  make upload      - flash the board (needs PORT set or auto-detected)"
	@echo "  make monitor     - open the serial monitor"
	@echo "  make flash       - upload, then drop into the serial monitor"
	@echo "  make erase-flash - wipe the ENTIRE flash (WiFi creds, dd_api_key/dd_app_key/"
	@echo "                     dd_site, everything in NVS) — use for a clean-slate test."
	@echo "                     Doesn't need the touchscreen, unlike the in-app factory"
	@echo "                     reset gesture / Settings button."
	@echo "  make reflash     - erase-flash, then upload + monitor in one go"
	@echo "  make clean       - remove .pio build artifacts (local only, doesn't touch the board)"
	@echo "  make port        - print the serial port that would be used"
	@echo ""
	@echo "  make flash-ledtest - flash witnessmenow's standalone RGB LED test"
	@echo "                       (tools/ledtest/) instead of BarkBoard itself — no"
	@echo "                       WiFi/LVGL/TFT_eSPI involved, just cycles the LED"
	@echo "                       off/red/green/blue every second. Use this to tell"
	@echo "                       whether an LED problem is BarkBoard's code or the"
	@echo "                       board's hardware. 'make flash' to go back afterwards."
	@echo ""
	@echo "  make flash-3248s035-demo - build+flash the downloaded ESP32-3248S035 BSP's"
	@echo "                       own demo (_reference/ESP32-3248S035/examples/platformio)."
	@echo "                       NOTE: that repo targets a different, larger (3.5\","
	@echo "                       capacitive-touch) Sunton board, not the 2.8\" resistive"
	@echo "                       2432S028R this repo targets — its LCD/touch init will"
	@echo "                       likely fail on our hardware. Its RGB LED code does use"
	@echo "                       the same GPIO4/16/17 + LEDC channels 13/14/15 we tried,"
	@echo "                       though, so this is only useful for cross-checking that"
	@echo "                       specific piece, not as a general demo. 'make flash' to"
	@echo "                       go back to BarkBoard afterwards."
	@echo ""
	@echo "  make fetch-monitors  -- --verify --type \"log alert\"   - inspect monitor"
	@echo "                        queries live (tools/fetch_dd.py), no flash needed"
	@echo "  make fetch-incidents -- --team my-team"
	@echo "  make fetch-oncall    -- --team my-team   (or -- --team-id 12345)"
	@echo "  make fetch-slos      -- --team my-team"
	@echo "  make fetch-event     -- --monitor-id 12345   - full raw JSON of a monitor's"
	@echo "                        most recent alert event (chart/snapshot data, etc.)"
	@echo "  make fetch-investigations -- --team my-team   - search Bits AI investigations"
	@echo "                        (an /api/unstable/ endpoint — see tools/fetch_dd.py's"
	@echo "                        docstring for the compatibility caveat)"
	@echo "  make fetch-investigation -- --id <uuid> [--summary-only]   - one investigation's"
	@echo "                        full detail (documented, stable endpoint)"
	@echo "                        The '--' is required — GNU Make parses '--team' as one"
	@echo "                        of ITS OWN flags otherwise and errors before your script"
	@echo "                        ever runs. (ARGS='--team my-team' still works too, if"
	@echo "                        you'd rather not type --.) All four pull DD_API_KEY/"
	@echo "                        DD_APP_KEY from .env and hit the exact same endpoints/"
	@echo "                        query shapes the firmware does — see"
	@echo "                        tools/fetch_dd.py --help for every flag."
	@echo ""
	@echo "Detected PORT: $(if $(PORT),$(PORT),none — plug in the board or pass PORT=/dev/cu.usbserial-XXXX)"

build:
	pio run -e $(ENV)

port:
	@echo "$(if $(PORT),$(PORT),no USB-serial device found)"

upload: check-port
	pio run -e $(ENV) -t upload --upload-port $(PORT)

monitor: check-port
	pio device monitor -e $(ENV) --port $(PORT)

flash: upload monitor

# Wipes the whole flash chip, not just NVS — the board comes back up as if
# freshly unboxed (no WiFi creds, no Datadog keys, no detected site). You'll
# need to `make upload` again afterwards since this erases the firmware too.
erase-flash: check-port
	pio run -e $(ENV) -t erase --upload-port $(PORT)

reflash: erase-flash upload monitor

clean:
	pio run -e $(ENV) -t clean
	rm -rf .pio

# Standalone sanity check, isolated from BarkBoard's own build (separate
# platformio.ini/src under tools/ledtest/ — see the comment there). Doesn't
# touch NVS/WiFi creds, just overwrites the firmware; `make flash` reflashes
# BarkBoard itself afterwards.
flash-ledtest: check-port
	pio run -d tools/ledtest -t upload --upload-port $(PORT)
	pio device monitor --port $(PORT)

# Downloaded BSP for a *different* Sunton board (3.5" capacitive touch,
# ESP32-3248S035), not the 2.8" resistive 2432S028R this repo targets. Its
# own demo (examples/platformio) inits its full LCD/touch/RGB LED/audio/
# photoresistor stack, so expect the LCD/touch parts to fail or misbehave on
# our hardware — this is only worth flashing to cross-check the RGB LED
# specifically (its BSP uses the same GPIO4/16/17 + LEDC channels 13/14/15
# pattern this project already tried). _reference/ is gitignored/dev-only —
# this target only works if that repo has actually been downloaded there.
flash-3248s035-demo: check-port
	pio run -d _reference/ESP32-3248S035/examples/platformio -t upload --upload-port $(PORT)
	pio device monitor --port $(PORT)

# Debugging aids — hit the real Datadog API directly with the exact same
# endpoints/query construction as src/datadog.cpp (team scope, teams scope,
# tags_query), so you can see what the device would fetch without flashing
# first. Pull DD_API_KEY/DD_APP_KEY from .env.
#
# Extra flags: `make fetch-monitors -- --verify --type "log alert"`. The
# `--` is required — without it, GNU Make tries to parse `--verify` as one
# of ITS OWN command-line options and errors out before tools/fetch_dd.py
# ever runs (this is what "just pulls every monitor" looks like: the flags
# silently failed to reach the script). `ARGS='...'` still works too, for
# anyone who'd rather not remember the --.
#
# $(filter-out $@,$(MAKECMDGOALS)) is what actually captures the words after
# `--` — MAKECMDGOALS is every goal on the command line (including the ones
# make doesn't have a real rule for, like `--verify`), and the catch-all
# `%: @:` rule at the bottom of this file is what stops make from trying to
# build them as targets and failing with "No rule to make target".
fetch-monitors:
	python3 tools/fetch_dd.py monitors $(filter-out $@,$(MAKECMDGOALS)) $(ARGS)

fetch-incidents:
	python3 tools/fetch_dd.py incidents $(filter-out $@,$(MAKECMDGOALS)) $(ARGS)

fetch-oncall:
	python3 tools/fetch_dd.py oncall $(filter-out $@,$(MAKECMDGOALS)) $(ARGS)

fetch-slos:
	python3 tools/fetch_dd.py slos $(filter-out $@,$(MAKECMDGOALS)) $(ARGS)

# A monitor's most recent alert event, full raw JSON — e.g.
# `make fetch-event -- --monitor-id 311012240`. This is the only thing the
# firmware's Events API call (inside triggerBitsInvestigation()) actually
# looks up today, and it only reads two fields out of it — this shows
# everything else too (chart/snapshot data, etc).
fetch-event:
	python3 tools/fetch_dd.py event $(filter-out $@,$(MAKECMDGOALS)) $(ARGS)

# Bits AI investigations — `investigations` searches (an /api/unstable/
# endpoint; see tools/fetch_dd.py's docstring for why that's fine to use but
# not guaranteed stable), `investigation` fetches one by id via the
# documented, stable per-id endpoint.
fetch-investigations:
	python3 tools/fetch_dd.py investigations $(filter-out $@,$(MAKECMDGOALS)) $(ARGS)

fetch-investigation:
	python3 tools/fetch_dd.py investigation $(filter-out $@,$(MAKECMDGOALS)) $(ARGS)

# Swallows the extra command-line words captured above (--verify, --team,
# ese-tola, etc.) so make doesn't try to build them as real targets.
%:
	@:

check-port:
ifeq ($(PORT),)
	$(error No USB-serial device found. Plug in the board, or pass PORT=/dev/cu.usbserial-XXXX explicitly. Run 'ls /dev/cu.*' to see what's attached)
endif
