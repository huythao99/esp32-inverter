# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Arduino/PlatformIO firmware for an ESP32 that bridges an STM32-based inverter to
the cloud: reads measurements from the STM32 over a software-serial link,
publishes data/status over MQTT, applies setup + schedule + share values from the
backend, and supports OTA (FOTA) updates. Board: `esp32dev`.

## Commands

The PlatformIO CLI is not on `PATH`; use the full path:

```sh
~/.platformio/penv/bin/pio run              # compile firmware (primary build/verify)
~/.platformio/penv/bin/pio run -t upload    # compile + flash the connected board
~/.platformio/penv/bin/pio device monitor   # serial monitor (9600 baud)
```

Host-side unit tests for the pure logic (no ESP32 needed — uses an Arduino
`String` shim, functions copied verbatim from `logic.cpp`):

```sh
g++ -std=c++17 -o /tmp/test_logic test/test_logic/test_main.cpp && /tmp/test_logic
```

There is only an `esp32dev` env in `platformio.ini` (no `native` test env), so run
logic tests with `g++` as above rather than `pio test`.

Always run `pio run` after changing firmware — the build is the real verification.
The IDE's clangd reports "Arduino.h file not found" / "Unknown type name 'String'"
across every file; that is a missing-include-path artifact of clangd, NOT a build
error. Ignore clangd diagnostics and trust the `pio run` result.

## Architecture: dual-core, single-writer

The firmware is split across two FreeRTOS cores to keep blocking network I/O off
the real-time path. Understanding this split is essential before changing anything.

- **Core 1 (Arduino `loop()` in `main.cpp`)** — everything time-sensitive:
  `mqttClient.loop()`, the STM32 software-serial read/write, `applyCurrentValue()`,
  the async web server, and all MQTT publishing. Must never block.
- **Core 0 (worker task in `worker.cpp`)** — ALL blocking HTTP and OTA
  (`net_http.cpp`). Blocks freely; a stuck fetch or a multi-minute OTA download
  cannot freeze Core 1.

Three RTOS primitives connect them (declared in `shared_state.h`):

- `jobQueue` (Core 1 → Core 0): Core 1 enqueues `Job`s via the `request*()` /
  `trackLog()` helpers in `worker.h`. All enqueues are non-blocking and drop
  silently if the queue is full. **Core 1 must never call the `net_http.cpp`
  functions directly** — enqueue a job instead.
- `otaStatusQueue` (Core 0 → Core 1): the worker's `publishOTAStatus()` enqueues a
  JSON line; `drainOtaStatus()` on Core 1 does the actual `mqttClient.publish()`.
  This keeps *all* `mqttClient` access on Core 1, so no MQTT mutex is needed.
- `stateMutex`: guards `lastSetupValue` and `schedules[]` (written by Core 0
  fetches, read by Core 1's `applyCurrentValue()`). Held only for microseconds
  (assignments), never across an HTTP call.

### The single writer

`applyCurrentValue()` (`logic.cpp`) is the ONLY place that writes to the STM32.
It runs every ~1s on Core 1 and picks the effective value by priority
**share > schedule > setting**, writing on change plus a periodic keepalive so a
rebooted STM32 always re-receives the current value. Do not add other
`testSerial.write()` call sites — route new inputs into this priority instead.

Value format sent to the STM32 is `*<Pset>@<Vset>#`, produced by
`convertSetupValue()` from an 8-digit `HHHHLLLL` string.

### Event / offline behaviour

- Server pushes real-time changes via retained-ish MQTT command topics
  (`cmd/settings`, `cmd/schedule`, `share`); the callback only sets debounced
  flags, and `loop()` turns them into jobs. 60s backstop polls cover missed msgs.
- After an MQTT reconnect, `loop()` re-enqueues settings+schedule fetches because
  messages sent while offline are lost (clean session).
- See `MQTT_RETAIN_NOTES.md` for what the **backend** must publish with
  `retain=true` and the firmware-update publish→clear pattern.

## Module map (`src/`)

`config.h` (defines/creds/pins/DBG) · `shared_state.*` (cross-core globals +
RTOS handles + `Job`/`ScheduleItem`) · `storage.*` (EEPROM + NVS + `getUid`) ·
`logic.*` (schedule/decision logic + `applyCurrentValue`, Core 1) · `net_http.*`
(HTTP + OTA, Core 0 only) · `net_mqtt.*` (connect/callback/`drainOtaStatus`, Core 1)
· `worker.*` (Core 0 task + queue + enqueue helpers) · `main.cpp` (setup/loop/web).

## Gotchas

- **Debug logging blocks the loop.** `DBG_*` macros print to USB Serial at 9600
  baud (~90ms per long line). `config.h` ships with `#define DEBUG 1`; set it to
  `0` for release. Heavy logging in hot paths starves the SoftwareSerial STM32
  link — keep new logs out of `applyCurrentValue()` and the MQTT callback.
- **Do not subscribe to topics the device publishes** (`status`, `data`). Doing so
  echoes every outgoing message back into the callback (self-flood).
- **`flash_device.py` is currently out of sync with the refactor.** It edits
  `src/main.cpp` for `#define WIFI_BROADCAST_SSID "GTIControl<N>"` and
  `#define DEBUG`, but those defines now live in `src/config.h`. Update the
  script's `MAIN_CPP_PATH` (or its regex targets) to point at `config.h` before
  relying on it. The script auto-increments the SSID number and **edits the file
  on every run** — commit/stash first.
- Each unit's WiFi AP SSID (`GTIControl<N>`) is persisted to NVS on first boot and
  then never changes, even across firmware uploads.
- **Keep the STM32 debug-frame compatibility (`convertDebugFrame()` in
  `main.cpp`).** Some boards in the field (first seen: GTIControl1218) run a
  test STM32 build that cannot be reflashed. It sends every 10 s
  `energy_import_wh <Wh>#<10 fields>*` then `energy_gen_wh <Wh>#*`; the ESP
  merges them into the standard 12-field frame `<10 fields>#<gen>#<import>`.
  Never publish the 10-field part alone (the backend would add its last two
  numbers to the daily energy). Devices using it send `STM_DEBUG_COMPAT` once
  per boot. Do not send STM32 FOTA to these boards.
- **`cmd/uart-debug`** `{"minutes":N}` streams every raw STM32 line to
  `.../debug/uart` for N minutes (1-30) — use it before guessing about a board
  whose `UART_STATS` shows `ok=0`.
```
