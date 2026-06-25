# DALI-ESP Agent Guide

This file is the short orientation guide for coding agents. Read `current_status.md`
first for the active project state, then use the focused planning files below.

## Project Aim

Two goals, in order:

1. **CLI completeness** — if it can be done using DALI commands, the CLI should be
   able to do it. The CLI is the reference tool for verifying real-bus behaviour
   and the proving ground for every protocol feature before it reaches the
   controller layer.

2. **ESP32 DALI controller** — leverage the proven CLI/protocol stack to build a
   working DALI controller firmware for ESP32, with primary emphasis on ESPHome and
   Home Assistant integration.

These are targets the project is working towards, not a description of current state.
See `current_status.md` for what is implemented and what is next.

## Context Files

- `current_status.md` - current overview, verified state, immediate priorities.
- `todo_hardware_bringup.md` - ESP-IDF, logic analyzer, and Lunatone validation flow.
- `diagnostic_discovery_plan.md` - bus discovery, inventory, identify, switch training.
- `todo_esphome_release.md` - end-user diagnostic firmware and final ESPHome flow.
- `todo_input_devices.md` - DALI-2 input-device ESPHome integration plan.
- `dali_command_reference.md` - command catalog and parser implementation reference.

## Non-Negotiable Architecture

Keep protocol logic independent of ESPHome:

```text
ESPHome / HA integration
DALI integration and entity mapping
DaliControl
DaliProtocol
DaliScheduler
DaliPhy
DALI-2 Click
DALI bus
```

`components/dali` is the reusable protocol, scheduler, and PHY stack.
`main/main.c` is the native ESP-IDF diagnostic entry point.
`main/dali_diag.c/.h` is the diagnostic serial CLI — app-specific, deliberately
not in `components/dali` because nothing in the reusable stack depends on it.
`esphome/dali_esphome.h` is a stub until hardware validation is complete.

Do not put protocol, timing, sensor, or addressing logic into ESPHome entities.
The ESPHome layer should map configured entities to `DaliTarget` values and call
the control/protocol APIs.

### What lives where

| Module | Location | Reason |
|---|---|---|
| Protocol, PHY, scheduler, discovery, memory, DT6, DT8, input config | `components/dali` | Reusable, no app dependencies |
| `dali_mapping` | `components/dali` | Generic enough to stay; move to `main/` if the component is published separately |
| `dali_dispatch` | `components/dali` | Headless dispatch engine; pure C, no ESPHome dependency |
| `dali_headless` | `esphome/components/dali` | Installation-specific dispatch table; edit per site; active only with `headless_dispatch: true` |
| `dali_diag` | `main/` | App-specific CLI; no component depends on it |
| `main.c` | `main/` | App entry point |

### Device type coverage

| DT | Standard | Status |
|---:|---|---|
| DT6 | IEC 62386-207 | Implemented (`dali_gear_dt6`) |
| DT8 | IEC 62386-209 | Implemented (`dali_gear_dt8`) |
| DT1 | IEC 62386-202 | **Not implemented** — fluorescent gear, limited new-install relevance; add when needed following the DT6/DT8 pattern |

Input device instance types DT301 (push button), DT303 (occupancy), and DT304
(light sensor) are covered by `dali_input_config` for configuration commands.
Query commands for all instance types are in `dali_input_device`.

## Timing And ISR Rules

- GPTIMER tick: 104 us.
- DALI bit period: about 833.3 us.
- DALI half-bit period: about 416.7 us.
- TX-to-RX settle suppression: 2 ms; DALI reply window opens at 7 ms.
- Reply timeout: 25 ms.
- Send-twice window: 100 ms.
- GPIO 16 and GPIO 17 are connected to WROVER-E PSRAM and must not be used.

ISR code must remain minimal:

- No logging.
- No allocation.
- No protocol parsing.
- No ESPHome calls.
- Use counters for ISR-visible errors.
- All ISR-called functions must be `IRAM_ATTR`.

RX must follow the buffer-first model:

```text
ISR -> fixed ring buffer -> task-context frame decoder -> scheduler/protocol
```

## Coding Guidance

- Preserve fixed-size/static allocation in PHY, scheduler, and protocol layers.
- Support both 16-bit and 24-bit DALI frames; do not assume 16-bit-only traffic.
- Use `DaliFrame`, `DaliError`, and explicit enums instead of bare integers.
- Use `uint8_t`, `uint16_t`, and `uint32_t` in protocol and PHY surfaces.
- Keep host tests in `test/` as the canonical portable test harness.
- Use Lunatone DALI USB / DALI Cockpit as the main external reference tool for
  real-bus behavior.

## Collaboration And Hardware Rules

- Documentation-only updates may be made directly.
- Suggest software-stack changes first and implement them only after explicit
  go-ahead.
- Touch hardware/serial only after explicit go-ahead.
- Use COM6 only. If COM6 is unavailable, stop and notify.

## Build And Test

Native firmware:

```powershell
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
idf.py build
idf.py -p COM<N> flash monitor
```

Host tests:

```powershell
cd test
C:\msys64\ucrt64\bin\mingw32-make.exe --directory build
C:\msys64\ucrt64\bin\mingw32-make.exe --directory build test CTEST_OUTPUT_ON_FAILURE=1
```
