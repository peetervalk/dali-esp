# DALI-ESP Current Status

**Last updated:** 2026-06-24
**Framework:** ESP-IDF v6.0.1 native CMake
**Hardware target:** ESP32-DevKitC-VE / ESP32-WROVER-E + MikroE DALI-2 Click

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
  dispatch table maps unsolicited bus frames to `dali_control_*` calls. Supports:
  `MIRROR` (re-issue same legacy opcode), `RECALL_MAX`, `RECALL_MIN`, `OFF`,
  `DIM_UP`, `DIM_DOWN`, `GO_TO_LAST`, `SCENE N`, and `TOGGLE` (stateful
  on/off bitmask for DALI-2 push buttons).
- **`dali_headless.cpp`** — installation-specific mapping table compiled into the
  ESPHome firmware. Current config: BF6 MIRROR entries for groups 0/2/3/4/5/6/7.
  Comments in the file walk through the phantom-address and DALI-2 migration paths.
- **Wired into `DaliComponent`** — setup registers an unsolicited-RX callback that
  pushes frames to `DaliInputEventQueue`; task drains the queue after each
  `dali_sched_run()` to avoid re-entrancy. Table loaded via weakly-linked
  `dali_headless_get_table()` — diag firmware gets a null default automatically.
- **`test_dispatch.c`** — 12 new host tests; all 18 suites green.

**Headless dispatch — what it can and cannot do:**

Can do:
- Lights respond to physical button presses with no HA or Wi-Fi dependency — dispatch
  runs entirely on Core 1, independent of the network stack.
- All standard lighting actions: `RECALL_MAX`, `RECALL_MIN`, `OFF`, `DIM_UP`,
  `DIM_DOWN`, `GO_TO_LAST`, `GO_TO_SCENE N`, `TOGGLE` (stateful bitmask).
- `MIRROR` passes through any legacy opcode including repeated `UP`/`DOWN` for
  long-press dimming — if couplers are reconfigured to send dimming sequences on
  hold, no firmware change is needed.
- Phantom-address remapping (Approach B from `todo_pb_couplers.md`) requires only a
  `dali_headless.cpp` edit — the engine already handles short-address keys.
- DALI-2 push buttons: add `INPUT_24BIT` entries with `TOGGLE` action.

Cannot do yet:
- **No HA state sync.** ESPHome light entities don't know a button press happened;
  their state in HA goes stale. Requires: (a) a target→entity reverse map in
  `DaliComponent`, (b) cross-core notification from Core 1 → Core 0 `loop()`,
  (c) knowledge of post-dispatch brightness (easy for RECALL_MAX/OFF, ambiguous
  for scenes without cached scene levels).
- **No double-press from BF6 couplers.** The DALI-2 24-bit double-press event
  code (0x03) requires a DALI-2 input device; BF6 couplers are DALI-1 and cannot
  produce it.
- **No hold-to-dim or multi-step sequences.** One button press → one DALI command.
  No press-and-hold progression, scene-cycle, or timed fade logic.
- **Toggle state does not persist across power cycles.** Resets to all-off on boot
  (correct for BF6 since coupler sends explicit ON/OFF; matters for DALI-2 TOGGLE
  entries).

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
    file to configure button→light mappings; weakly-linked default returns null
    so the diag firmware compiles without any mappings
  - `dali_scan.h/.cpp` — scan task spawned on demand (Core 1, priority 9);
    synchronous transport via `ulTaskNotifyTake`; logs full JSON inventory and a
    draft ESPHome YAML snippet
  - `button/__init__.py` + `button/dali_scan_button.h` — "Scan DALI Bus" button
  - `light/__init__.py` — light platform schema; `LightType.BRIGHTNESS_ONLY`
    (required in ESPHome 2026.6)
  - `light/dali_light_output.h/.cpp` — `DaliLightOutput` maps ESPHome brightness
    float 0–1 to DALI arc level 1–254; off → `dali_control_off()`
- **`dali_diag.yaml`** — diagnostic/discovery firmware; scan_status text sensor +
  "Scan DALI Bus" button; WiFi AP + captive portal for field access without
  pre-configured Wi-Fi
- **`dali_1k.yaml`** — first-floor control firmware; 7 group light entities
  (groups 0/2/3/4/5/6/7) matching the live scan; WiFi AP + captive portal fallback
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

1. **Flash and test `dali_1k.yaml` with headless dispatch.** Confirm BF6 coupler
   presses appear in ESP32 logs and lights respond independently of HA.
2. **Flash and test `dali_diag.yaml`.** Press "Scan DALI Bus" in HA; verify JSON
   inventory and draft YAML appear in logs. Confirm scan_status sensor updates.
3. Bring up the Steinel HF 360 II: connect to a bus without an existing master,
   run `scan`, confirm 4 instances decode correctly.
4. Add `set-system-failure-dtr0` to diag config spec table (small gap, one line).
5. Legacy pushbutton coupler zone grouping — see `todo_pb_couplers.md`.

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
  dali_input_config       Input-device configuration frame builders (IEC 62386-103,
                            DT301/303/304); requires send_twice from caller
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
| test_dispatch | 12 | Headless dispatch: MIRROR, TOGGLE, actions, key matching |

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
