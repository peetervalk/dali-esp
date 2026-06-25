# DALI-ESP Current Status

**Last updated:** 2026-06-26 (evening)
**Framework:** ESP-IDF v6.0.1 native CMake
**Hardware target:** ESP32-DevKitC-VE / ESP32-WROVER-E + MikroE DALI-2 Click

## Project Aim

1. **CLI completeness** — if it can be done using DALI commands, the CLI should be
   able to do it.
2. **ESP32 DALI controller** — leverage that capability to build a working DALI
   controller firmware for ESP32, with primary emphasis on ESPHome / Home Assistant
   integration.

These are targets being worked towards. The sections below describe current state.

## Where Things Stand

The software stack is now the most complete it has ever been. PHY, scheduler,
protocol, commissioning, discovery, control, memory, device-type, and headless
dispatch layers are all implemented and covered by 18 host test suites.

### Discovery enrichment

`scan` now performs a full per-device enrichment pass:

- **Status and group membership** — 16-bit bitmask.
- **Multi device type** — primary type via `QUERY_DEVICE_TYPE`, then repeated
  `QUERY_NEXT_DEVICE_TYPE` until `0xFF`; up to 4 types per device stored.
- **DALI version and actual level.**
- **Number of input instances** — automatic input-device detection.
- **Memory bank 0** — GTIN, firmware version, serial number.
- **Memory bank 1** — hardware major/minor version (superset of bank 0).
- **Scene levels** — all 16 scenes queried; `0xFF` = MASK (not configured).
- **DT6 enrichment** — failure status and features if device has type 6.
- **DT8 enrichment** — gear features, colour status, colour type features if
  device has type 8.

`kind` in JSON is `control_gear`, `input_device`, or `unknown`.

### Device-type modules

| Module | Standard | Description |
|---|---|---|
| `dali_gear_dt6` | IEC 62386-207 | LED control gear: failure status, features, dimming curve, fast fade |
| `dali_gear_dt8` | IEC 62386-209 | Colour control gear: XY, Tc, RGBWAF; 16-bit colour value read helper |

DT8 includes a transport-level helper `dali_dt8_read_colour_value_16()` that
performs the full 4-step QueryColourValue + QueryContentDTR0 sequence.

**DT1 (fluorescent gear) is not implemented.** DT1 shares the same opcode space
(0xE0–0xFF) but is a legacy device type with limited real-world relevance in new
installations. It can be added following the same pattern as DT6 when needed.

### Input device configuration

`dali_input_config` provides frame builders for:
- Generic IEC 62386-103 instance configuration (priority, enable/disable, groups,
  event scheme, filter, timers).
- DT301 push-button type-specific commands.
- DT303 occupancy sensor type-specific commands.
- DT304 light sensor type-specific commands.

All configuration commands require `send_twice = true` from the caller, per the
DALI-2 standard. These are kept in their own module (not discovery) because the
discovery mock asserts `send_twice = false` on all frames.

### On hardware

The ESP32 boots cleanly, the diagnostic shell is reachable over COM6, and
full bidirectional DALI communication is confirmed on a live installation bus.

**Session 2026-06-21 — major hardware milestones reached:**

- **8-bit reply RX working.** Root cause of 0-device scan was `DALI_SETTLE_MS = 7`
  (equal to the DALI spec minimum answer time). The PHY suppress window was armed
  in task context, adding FreeRTOS scheduling latency and pushing the deadline past
  the 7 ms boundary. Fix: reduced to 2 ms and moved suppress-window setup into the
  TX ISR for precise timing. `rx_settle_suppressed` counter now stays at 0.

- **Full bus scan confirmed.** `scan` + `export inventory` enumerated 16 control
  gear on a live installation (addresses 0–15). Address 0 = DT6 LED driver;
  addresses 1–15 = generic DALI-1 gear (device type 0xFF, no DT classification).
  Six devices lit at level 254 (status 0x04 = lamp arc on), remainder off. Reply
  timing clean at ~13–14 ms.

- **`export inventory` stack overflow fixed.** `DaliDiscoveryInventory` (~5–6 KB)
  was stack-allocated in `cmd_export` and `cmd_inventory`, overflowing the 8 KB
  diag task stack. Fixed by making both locals `static`.

- **Legacy pushbutton couplers detected.** `find switches` passive-listen found
  7 physical switches (14 unique frames) from two Lunatone/Tridonic DALI MC PB
  couplers with BF6 function. Each switch targets a DALI group with a `recall-max`
  (on) and `off` frame. Groups in use: 0, 2, 3, 4, 5, 6, 7. Improvement backlog
  in `todo_pb_couplers.md`.

- **Multi-master note.** The ESP32 has no collision detection and must be the sole
  master on any bus it controls. Remove existing controllers before connecting.

**Next milestone: Steinel HF 360 II.** Connect to a bus where the existing
controller has been removed. Run `scan`; expect the sensor at a new address with
`kind: "input_device"` and 4 instances (brightness, motion, temperature, humidity).

**Session 2026-06-24 — Headless dispatch layer added:**

- **`dali_dispatch.{c,h}`** — new pure-C module in `components/dali/`. Static
  dispatch table maps unsolicited bus frames to state tracking or
  `dali_control_*` calls. Supports: `OBSERVE` (infer state without TX for
  direct-control BF6 couplers), `MIRROR` (re-issue same legacy opcode for
  phantom-address translation), `RECALL_MAX`, `RECALL_MIN`, `OFF`, `DIM_UP`,
  `DIM_DOWN`, `GO_TO_LAST`, `SCENE N`, and `TOGGLE` (stateful on/off bitmask
  for DALI-2 push buttons).
- **`dali_headless.cpp`** — installation-specific mapping table in the ESPHome
  component tree. Current config: BF6 OBSERVE entries for groups 0/2/3/4/5/6/7.
  Comments in the file walk through the phantom-address and DALI-2 migration
  paths. The table is inert unless the YAML explicitly sets
  `headless_dispatch: true`.
- **Wired into `DaliComponent`** — setup registers an unsolicited-RX callback that
  pushes frames to `DaliInputEventQueue`; task drains the queue after each
  `dali_sched_run()` to avoid re-entrancy. Table loaded via weakly-linked
  `dali_headless_get_table()`; builds without the opt-in table get a null default
  automatically.
- **`test_dispatch.c`** — 25 host tests; all 18 suites green.

**Headless dispatch — what it can and cannot do:**

Can do:
- Lights respond to physical button presses with no HA or Wi-Fi dependency — dispatch
  runs entirely on Core 1, independent of the network stack.
- All standard lighting actions: `RECALL_MAX`, `RECALL_MIN`, `OFF`, `DIM_UP`,
  `DIM_DOWN`, `GO_TO_LAST`, `GO_TO_SCENE N`, `TOGGLE` (stateful bitmask).
- `OBSERVE` tracks direct BF6 group commands without re-transmitting them; the
  physical coupler remains the controller and the ESP32 only updates local/HA
  state. DAPC dimming frames are observed as brightness levels.
- `MIRROR` passes through any legacy opcode including repeated `UP`/`DOWN` for
  phantom-address mode, where the coupler sends to an unused short address and
  the ESP32 must translate that into the real output target. DAPC levels are
  translated too for phantom dimming.
- Phantom-address remapping (Approach B from `todo_pb_couplers.md`) requires only a
  `dali_headless.cpp` edit — the engine already handles short-address keys.
- DALI-2 push buttons: add `INPUT_24BIT` entries with `TOGGLE` action.

Cannot do yet:
- **No double-press from BF6 couplers.** The DALI-2 24-bit double-press event
  code (0x03) requires a DALI-2 input device; BF6 couplers are DALI-1 and cannot
  produce it.
- **No hold-to-dim or multi-step sequences.** One button press → one DALI command.
  No press-and-hold progression, scene-cycle, or timed fade logic.
- **Toggle state does not persist across power cycles.** Resets to all-off on boot
  (correct for BF6 since coupler sends explicit ON/OFF; matters for DALI-2 TOGGLE
  entries).

**Session 2026-06-24 (continued) — HA state sync and deferred query:**

- **Bus snooping** — `DaliDispatchResult` carries inferred on/off + level for
  deterministic actions (RECALL_MAX, OFF, TOGGLE). Core 1 calls `notify_lights()`
  after each dispatch; `mark_state_from_bus()` stores state atomically; Core 0
  `loop()` calls `apply_bus_state()` which pushes a `LightCall` with
  `skip_next_write_` armed so ESPHome does not re-issue the DALI command.
- **Boot query** — `start_refresh()` is called once on first `loop()` invocation;
  issues `QUERY_ACTUAL_LEVEL` on each light entity that has `query_address` set
  and seeds the toggle bitmask from the reply via `dali_dispatch_seed_toggle()`.
- **On-demand refresh button** — `type: refresh` button in the `dali` button
  platform calls `start_refresh()` on press.
- **Periodic polling** — optional `poll_interval` (seconds) on the `dali:`
  component; calls `start_refresh()` on the configured cadence.
- **Deferred query after dim/scene** — `DaliDispatchResult.matched` flag added to
  distinguish "no table entry found" from "matched but state is indeterminate"
  (DIM_UP, DIM_DOWN, SCENE, GO_TO_LAST, RECALL_MIN). When Core 1 dispatches one
  of these, it sets `s_deferred_query_pending_`; Core 0 arms a 600 ms one-shot
  timer (re-arms on each set, so continuous dimming extends the window); fires
  `start_refresh()` when elapsed.
- **`DaliBusLight` abstract interface** — defined in `dali_component.h`; exposes
  `mark_state_from_bus()`, `apply_bus_state()`, `get_query_address()`. The
  `DaliComponent` registry stores `DaliBusLight*` so `dali_component.cpp` never
  includes any `light/` subdirectory header (eliminates the cross-directory include
  that broke the dali-diag build).
- **`proto_dali_dispatch.c`** — added to `esphome/components/dali/`; the thin
  wrapper that ESPHome's GLOB_RECURSE picks up to compile `dali_dispatch.c` as C.
  Was missing because `dali_dispatch` was added to the protocol stack after the
  other wrappers were written.

**Session 2026-06-26 (evening) — command console completeness pass:**

- **`DaliBusSensor` abstract base class** — `dali_component.h` now declares
  `DaliBusSensor` (mirrors `DaliBusLight` pattern); `dali_component.cpp` uses it
  instead of including `sensor/dali_input_sensor.h` directly. Fixes a build failure
  where the sensor/ subdirectory was not copied to the PlatformIO source tree for
  builds that didn't use the sensor platform (dali-1k, dali-diag).

- **`dali_1k.yaml` updated** — now equivalent to `dali_2k.yaml` for console
  features: `command_result` text sensor and `text:` DALI Command entity added.
  `refresh: 5min` changed to `0s` to match dali-2k.

- **`dali_2k.yaml` sensor tuning:**
  - Lux `scale: 0.01` — calibrated against phone ambient light sensor (raw 7200
    → ~72 lx, consistent with ceiling-height reflected measurement).
  - Occupancy raw sensor hidden (`internal: true`); new `text_sensor: template`
    entity "Zone 2 Occupancy" maps DT303 values to human-readable strings:
    0→Free, 85→Movement, 170→Present, 255→Present+Moving.

- **`iquery` verb added** to `execute_command()`. Syntax: `iquery a<N>:<inst> <name>`.
  Result appears in "DALI Command Result" text sensor. Names match the `iconfig`
  SET counterparts:

  | iquery name | reads back |
  |---|---|
  | `hold-timer` | DT303 hold timer (Steinel default 900 s) |
  | `deadtime` | DT303 deadtime |
  | `hysteresis` | generic input hysteresis |
  | `deadtime-gen` | generic deadtime timer |
  | `report-timer` | generic report timer |
  | `instance-type` | device type (3=DT303, 4=DT304, …) |
  | `resolution` | bit resolution |
  | `instance-enabled` | yes/no — whether instance is active |
  | `instance-status` | status byte |

- **Command console symmetry fixes:**
  - `query`: renamed misleading `power-on` → `power-on-flag` (yes/no event flag,
    not a level) and `power-failure` → `power-fail-flag`. Added `power-on-level`
    (`QUERY_POWER_ON_LEVEL`) and `failure-level` (`QUERY_SYSTEM_FAILURE_LEVEL`) as
    the correct read counterparts for `set-power-on-dtr0` / `set-failure-dtr0`.
  - `config`: added `set-operating-mode` (DTR0 = mode byte).
  - `iconfig`: added `enable-instance` and `disable-instance` (no DTR0 argument;
    handler now detects `needs_dtr0` flag and builds a 1-step sequence).
  - `iquery`: renamed `deadtime-timer` → `deadtime-gen` to match `iconfig`'s
    `set-deadtime-gen`.

- **`dali_input_config.h/.c` extended** — query frame builders added for all
  instance parameter types: generic 103 (`instance-type`, `resolution`,
  `hysteresis`, `deadtime-timer`, `report-timer`, `instance-enabled`,
  `instance-status`), DT303 (`hold-timer`, `deadtime`), DT304 (`hysteresis`,
  `deadband`).

- **`dali_2k.yaml` verified on hardware**: all 4 Steinel sensor entities confirmed
  working in HA after flashing via ESPHome device builder (OTA). Lux, temperature,
  humidity, and occupancy state all publishing correctly.

**Session 2026-06-26 — DALI-2 sensor platform and HA command console:**

- **`DALI_BUS_IDLE_TIMEOUT_US` fix** — was `DALI_BIT_US * 4` (~3.3 ms), shorter than
  a 24-bit DALI-2 frame (~10.8 ms). Steinel HF 360 II spamming unsolicited events
  caused `DALI_ERR_BUS_STUCK` on every scan TX. Fixed to `DALI_BIT_US * 40` (~33 ms)
  in `dali_frame.h`. Scan now completes cleanly with the sensor on the bus.

- **`dali_2k.yaml`** — ESPHome config for Bus 2: 5 control gear (addrs 0/1/2/3/5,
  all in group 0), Steinel HF 360 II at addr 0, DALI-2 PB coupler (BF6 mode) at
  addr 1. `headless_dispatch: true` covers group 0 coupler frames. `query_address: 2`
  uses a plain lamp for level polling.

- **ESPHome `sensor/` platform** (`esphome/components/dali/sensor/`):
  - `DaliInputSensor` — polls `QUERY_INPUT_VALUE` via the async scheduler (no
    blocking). 1-byte and 2-byte instances handled via callback chaining
    (MSB → enqueue LATCH → LSB → combine → publish). Core 1→Core 0 via atomic
    dirty flag + `apply_value()` drain in `loop()`.
  - Config: `address`, `instance`, `poll_interval` (seconds), `value_bytes` (1 or 2),
    `scale`, `offset`.
  - All 4 Steinel instances configured in `dali_2k.yaml`: lux (30 s, 2-byte),
    occupancy (10 s, 1-byte, raw 0/85/170/255 = free/movement/hold/present+moving),
    temperature (60 s, 2-byte, scale=0.1 offset=-5 → °C),
    humidity (60 s, 1-byte, scale=0.5 → %).

- **ESPHome `text/` platform** (`esphome/components/dali/text/`):
  - `DaliCommandText` — text entity in HA; `control(value)` calls
    `DaliComponent::execute_command()`. Result appears in a `command_result`
    text sensor defined under the `dali:` component.
  - `execute_command()` parses CLI-style commands: `off/max/min/level <target>`,
    `query <target> <name>`, `config <target> <name> [dtr0]`,
    `iconfig a<N>:<inst> <name> <dtr0>`. Targets: `a<N>`/`s<N>` = short,
    `g<N>` = group, `b` = broadcast.
  - `iconfig` supports DT303 occupancy config: `set-hold-timer`, `set-deadtime`;
    generic: `set-hysteresis`, `set-report-timer`, `set-deadtime-gen`.
  - Replaces the need to flash CLI firmware just to tune sensor parameters.
    Hold timer and sensitivity can be adjusted from HA: e.g.
    `iconfig a0:1 set-hold-timer 20`.

- **Steinel HF 360 II hardware verified** on Bus 2:
  - All 4 instances polled successfully via CLI `sensor poll 0`.
  - Occupancy states 0x00 (free), 0xAA (hold/present), 0xFF (present+moving) confirmed.
  - Temperature formula `T = raw × 0.1 − 5` verified (raw=276 → 22.6°C).
  - Humidity formula `H = raw × 0.5` verified (raw=104 → 52%).
  - Light sensor raw=7107 at time of measurement.
  - Default hold timer ~900 s (15 min) — tune via HA command console.

**Session 2026-06-25 — headless state sync verified on live bus:**

- **Boot/flash behavior** — newest `dali_1k.yaml` firmware no longer turns the
  installation off during startup or after flashing.
- **BF6 on/off observation** — physical coupler `off` frames are observed without
  retransmission and update the matching HA light entity immediately.
- **BF6 hold-dim sync** — physical dimming sends `on-step` plus repeated
  `up`/`down` frames, which are state-indeterminate. The firmware now re-arms a
  600 ms deferred refresh during the dim stream, then sends
  `QUERY_ACTUAL_LEVEL` after the stream stops. Live trace: group 3 dimmed from
  off, queried representative short address 13, received level 91, and HA
  updated to 36% brightness.
- **HA-origin brightness writes** — ESPHome default transition streaming was
  disabled for DALI outputs; writes now command the requested target value and
  suppress duplicate known-state commands.
- **Remaining refinement** — deferred refresh currently polls all configured
  DALI light entities with `query_address`. This is acceptable for the 7-group
  installation, but can be narrowed to the affected target if bus traffic or log
  noise becomes a problem.

**Session 2026-06-22 — ESPHome component built and both firmwares compile:**

- **`esphome/components/dali/`** — external ESPHome component:
  - `__init__.py` — component schema (tx_pin, rx_pin, optional scan_status text
    sensor); `AUTO_LOAD = ["text_sensor"]`
  - `CMakeLists.txt` — IDF registration for native builds; `file(GLOB)` compiles
    protocol `.c` files as C with correct REQUIRES. **Not used by ESPHome's
    PlatformIO build** (ESPHome 2026.6 ignores it and auto-discovers sources).
  - `proto_dali_*.c` — 17 thin one-line wrapper `.c` files (one per protocol
    source file). PlatformIO picks these up automatically and compiles each `.c`
    file as C in its own translation unit — avoiding the static-name clashes and
    C99/C++ incompatibilities that made a unity `.cpp` build unworkable.
  - `dali_component.h/.cpp` — `DaliComponent` ESPHome class; `setup()` calls
    `dali_phy_init()` + `dali_sched_init_device()`, registers the unsolicited-RX
    callback, loads the headless dispatch table, then starts the DALI FreeRTOS
    task pinned to Core 1 (App CPU) at priority 10
  - `dali_headless.cpp` — installation-specific dispatch table; user edits this
    file to configure button→light mappings. Direct BF6 entries observe only;
    phantom-address entries use `MIRROR` to translate inert frames into real
    DALI commands. It only emits the strong table when the YAML sets
    `headless_dispatch: true`.
  - `dali_scan.h/.cpp` — scan task spawned on demand (Core 1, priority 9);
    synchronous transport via `ulTaskNotifyTake`; logs a draft ESPHome YAML
    snippet line-by-line prefixed with `YAML|` (extractable via PowerShell
    `Select-String`); JSON inventory at DEBUG level only
  - `button/__init__.py` + `button/dali_scan_button.h` — diagnostic button
    platform: scan, refresh, identify, find couplers, and target control actions
  - `light/__init__.py` — light platform schema; `LightType.BRIGHTNESS_ONLY`
    (required in ESPHome 2026.6)
  - `light/dali_light_output.h/.cpp` — `DaliLightOutput` maps ESPHome brightness
    float 0–1 to DALI arc level 1–254; off → `dali_control_off()`
  - `sensor/__init__.py` + `sensor/dali_input_sensor.h` — `DaliInputSensor` polls
    `QUERY_INPUT_VALUE` on a configurable interval; 1- or 2-byte async reads with
    callback chaining; `scale`/`offset` for unit conversion
  - `text/__init__.py` + `text/dali_command_text.h` — `DaliCommandText` exposes a
    HA text input that parses CLI-style DALI commands; result in `command_result`
    text sensor; supports gear control, queries, config, and instance config
- **`dali_diag.yaml`** — diagnostic/discovery firmware; scan/status/result text
  sensors, bus monitor, target address number, scan/refresh/identify/find-couplers
  buttons, target on/off/max/min controls; WiFi AP + captive portal for field
  access without pre-configured Wi-Fi
- **`dali_1k.yaml`** — first-floor control firmware; 7 group light entities
  (groups 0/2/3/4/5/6/7) matching the live scan; explicit `headless_dispatch: true`
  for local BF6 coupler fallback; WiFi AP + captive portal fallback
- **`secrets.yaml`** — gitignored; contains Wi-Fi credentials, API key, OTA and
  AP passwords

**Both `dali_diag.yaml` and `dali_1k.yaml` compile successfully** against
ESPHome 2026.6.2 / IDF 5.5.4 (PlatformIO wrapper).

Notable ESPHome 2026.6 API fixes applied during this session:
- `text_sensor.TEXT_SENSOR_SCHEMA` → `text_sensor.text_sensor_schema()`
- `button.BUTTON_SCHEMA` → `button.button_schema(Cls)` + `button.new_button()`
- `light.light_schema(Cls)` → `light.light_schema(Cls, LightType.BRIGHTNESS_ONLY)`
- `cg.add_global_arg()` removed — include paths handled by per-file relative paths
- `LightTraits.set_supports_brightness()` removed → `set_supported_color_modes({ColorMode::BRIGHTNESS})`

Thread-safety prerequisite confirmed in previous session: `dali_sched_enqueue()`
uses a `portMUX_TYPE` spinlock → safe to call from ESPHome loop task (Core 0)
while DALI task runs on Core 1.

### Terminology note

"Pushbutton coupler" is the correct term for devices that inject DALI bus
commands when a physical button is pressed. The word "switch" is avoided because
Home Assistant uses "switch" for a binary on/off toggle entity.

## Immediate Priorities

1. **Tune Steinel hold timer** — `iquery a0:1 hold-timer` to read current value,
   then `iconfig a0:1 set-hold-timer <N>` to set. Target: shortest delay where
   the light reliably stays on while the room is in use.
2. **Investigate Steinel detection range** — light activates at shorter distance
   than expected for an HF sensor. May need sensitivity or deadtime adjustment via
   `iconfig a0:1 set-deadtime <N>` and/or Steinel-specific configuration.
3. **Flash `dali_1k.yaml`** — now has the command console; OTA via ESPHome device
   builder.
4. Legacy pushbutton coupler zone grouping — see `todo_pb_couplers.md`.

`dali_diag.yaml` is working: scan, Find Couplers, Identify, state commands,
Group Map sensor, and YAML extraction via PowerShell `Select-String` all
verified on live bus (16 devices, 7 groups).

## Architecture

```text
ESPHome / Home Assistant integration      (component scaffold + test YAML ✓)
Headless dispatch (dali_dispatch + dali_headless)  (implemented, host-tested ✓)
DALI entity mapping / release integration (mapping helpers ready, release future)
DALI discovery / inventory helpers        (implemented, hardware-verified ✓)
DaliControl                               (implemented)
DaliProtocol                              (implemented, hardware sensor polling pending)
DaliScheduler                             (implemented, host-tested)
DaliPhy                                   (implemented, hardware-verified TX+RX ✓)
DaliRingBuf                               (implemented, host-tested)
DALI-2 Click
DALI bus
```

### Source layout

```
components/dali/          reusable protocol stack (no app dependencies)
  dali_phy                PHY: Manchester encode/decode, UART, GPIO
  dali_ringbuf            ISR-safe ring buffer for RX frames
  dali_scheduler          TX/RX sequencer, retries, send-twice timing
  dali_protocol           Frame builders, command table, address encoding
  dali_control            Control-gear command API
  dali_input_device       Input-device query API
  dali_input_config       Input-device configuration and query frame builders
                            (IEC 62386-103, DT301/303/304); SET commands require
                            send_twice from caller; QUERY commands are single-send
  dali_input_poll         Input-device polling helpers
  dali_event              Event routing
  dali_discovery          Bus scan, per-device enrichment, inventory
  dali_commissioning      Short-address assignment
  dali_memory             Memory bank 0 and bank 1 identity reads
  dali_gear_dt6           LED gear (IEC 62386-207) frame builders and parsers
  dali_gear_dt8           Colour gear (IEC 62386-209) frame builders, parsers,
                            Kelvin/Mirek conversion, 16-bit colour value read
  dali_mapping            Static endpoint mapping helpers (see note below)
  dali_dispatch           Headless dispatch engine: unsolicited-frame → control
                            action; supports MIRROR, TOGGLE, fixed actions, scenes
  dali_lunatone           Lunatone vendor extensions
  dali_steinel            Steinel vendor extensions

main/                     ESP-IDF application entry point
  main.c                  App init, task creation, scheduler wiring
  dali_diag.c/.h          Diagnostic serial CLI (app-specific; moved from
                            components/dali because nothing else depends on it)
```

**`dali_mapping` note:** This module is currently in `components/dali` for
convenience but is application-specific configuration glue. If the component is
ever published separately or used in a second firmware target, `dali_mapping`
should be moved to `main/` alongside `dali_diag`.

## Host Test Suites (18 total)

| Suite | Tests | Coverage |
|---|---:|---|
| test_ringbuf | — | ISR-safe ring buffer |
| test_phy_encode | — | Manchester encoder |
| test_phy_decode | — | Manchester decoder |
| test_protocol | — | Frame builders, command table, address encoding |
| test_input_device | — | Input-device query API |
| test_input_poll | — | Input-device polling helpers |
| test_event | — | Event routing |
| test_discovery | — | Bus scan, enrichment, inventory |
| test_commissioning | — | Short-address assignment |
| test_vendor | — | Lunatone / Steinel extensions |
| test_mapping | — | Endpoint mapping helpers |
| test_memory | — | Memory bank 0 and bank 1 reads |
| test_gear_dt6 | — | DT6 frame builders, failure-status parser |
| test_gear_dt8 | — | DT8 frame builders, parsers, Kelvin/Mirek, 16-bit colour read |
| test_input_config | — | IEC 62386-103 + DT301/303/304 config frame builders |
| test_control | — | Control-gear command API |
| test_scheduler | — | TX/RX sequencer |
| test_dispatch | 25 | Headless dispatch: OBSERVE, MIRROR, DAPC dimming, TOGGLE, actions, key matching |

## Known Target Sensor

Steinel HF 360 II DALI-2 IPD UP — EAN `4007841064280`, DALI Alliance ID `3742`.
Parts: IEC 62386-101, -103, -303, -304.

Expected instances:

| Instance | Type | Meaning |
|---:|---:|---|
| 0 | 4 | Brightness |
| 1 | 3 | Motion |
| 2 | 0 | Temperature (`T_C = binValue × 0.1 − 5`) |
| 3 | 0 | Humidity (`H_percent = binValue × 0.5`) |

## Open Questions

| Topic | Status |
|---|---|
| HA brightness 0 behavior | Prefer explicit `OFF`; confirm on hardware. |
| ESPHome component packaging | In-tree custom component first; external later. |
| DALI-2 firmware update / DFU | Out of scope. |

## Build Commands

ESPHome (from project root, `secrets.yaml` must be filled):

```powershell
esphome compile dali_diag.yaml   # diagnostic/discovery firmware
esphome compile dali_1k.yaml     # first-floor control firmware

esphome run dali_diag.yaml       # compile + flash + open log
esphome logs dali_diag.yaml      # attach to running device log
```

Native ESP-IDF:

```powershell
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
idf.py build
idf.py -p COM6 flash monitor
```

Host tests:

```powershell
cd test
C:\msys64\ucrt64\bin\mingw32-make.exe --directory build
C:\msys64\ucrt64\bin\mingw32-make.exe --directory build test CTEST_OUTPUT_ON_FAILURE=1
```
