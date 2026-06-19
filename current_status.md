# DALI-ESP Current Status

**Last updated:** 2026-06-19
**Framework:** ESP-IDF v6.0.1 native CMake
**Hardware target:** ESP32-DevKitC-VE / ESP32-WROVER-E + MikroE DALI-2 Click

## Where Things Stand

The software stack is now the most complete it has ever been. PHY, scheduler,
protocol, commissioning, discovery, control, memory, and device-type layers are
all implemented and covered by 17 host test suites (180+ individual test cases).

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
bidirectional DALI communication with Lunatone is confirmed: passive RX decodes
correctly and Lunatone confirms ESP-originated TX. The MikroE DALI-2 Click
optocoupler inversion is handled in the PHY.

**The next milestone is 8-bit device replies.** Once reply RX is solid, the
Steinel HF 360 II sensor and DALI-2 instance discovery follow.

### Terminology note

"Pushbutton coupler" is the correct term for devices that inject DALI bus
commands when a physical button is pressed. The word "switch" is avoided because
Home Assistant uses "switch" for a binary on/off toggle entity.

## Immediate Priorities

1. TX pattern sweep with Lunatone only: `max b`, `min b`, `raw 0xFE80 len=16`,
   `raw 0xFF05 len=16`.
2. Add one known DALI control gear; run `scan` and confirm 8-bit replies decode.
3. Compare ESP frames and replies against Lunatone DALI Cockpit captures.
4. Bring up the Steinel HF 360 II after control gear replies are stable.
5. Only after native diagnostics are reliable: ESPHome-flashable diagnostic
   firmware (`todo_esphome_release.md`).

## Architecture

```text
ESPHome / Home Assistant integration      (stub)
DALI entity mapping / release integration (mapping helpers ready, release future)
DALI discovery / inventory helpers        (implemented, host-tested)
DaliControl                               (implemented)
DaliProtocol                              (implemented, hardware sensor polling pending)
DaliScheduler                             (implemented, host-tested)
DaliPhy                                   (implemented, hardware-verified for TX/RX baseline)
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

## Host Test Suites (17 total)

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
