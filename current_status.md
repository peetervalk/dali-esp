# DALI-ESP Current Status

**Last updated:** 2026-06-26
**Status:** ESPHome firmware works on live hardware. This file is now the active
project state and to-do list. Old session-log planning files have been folded
into this file and removed.

## Project Aim

1. **CLI completeness**: if it can be done using DALI commands, the CLI
   should be able to do it. The CLI is the reference tool for verifying real-bus
   behavior and for proving protocol features before they reach the controller
   layer.
2. **ESP32 DALI controller**: leverage the proven CLI/protocol stack to build a
   working DALI controller firmware for ESP32, with primary emphasis on ESPHome and
   Home Assistant integration.

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

## Verified State

### Shared DALI stack

- `components/dali` contains the reusable C stack: PHY, ring buffer, scheduler,
  protocol builders/parsers, control API, discovery, commissioning, memory,
  DT6, DT8, input-device queries/config, event parsing, mapping, and dispatch.
- The native CLI remains the reference implementation for protocol behavior.
- Host tests cover the stack with 18 suites, including dispatch and input config.
  Re-run them after any protocol, parser, discovery, or dispatch change.
- DT6 and DT8 are implemented. DT1 is not implemented and can wait until needed.

### Hardware

- Hardware target: ESP32-DevKitC-VE / ESP32-WROVER-E + MikroE DALI-2 Click.
- Known wiring: TX -> GPIO18, RX -> GPIO19.
- GPIO16 and GPIO17 must not be used on WROVER-E because they are connected to
  PSRAM.
- Full bidirectional DALI communication is confirmed on live buses.

### Native diagnostic CLI

- The native diagnostic shell is available over serial and remains the safest
  place to prove new protocol features before exposing them through ESPHome.
- Bus scan/discovery, inventory, identify, command/query/config, input polling,
  capture, and coupler-find workflows are implemented.
- Native JSON inventory/export is useful for debugging. For the ESPHome
  diagnostic firmware, generated YAML in logs is the primary output.

### ESPHome component

- Active component: `esphome/components/dali`.
- `dali_diag.yaml` is the diagnostic/discovery firmware. It exposes scan,
  identify, find-couplers, target-control buttons, bus monitor, group-map text
  sensors, and YAML snippets in logs prefixed with `YAML|`.
- `dali_1k.yaml` is the first-floor control firmware: 16 control gear, group
  entities for groups 0/2/3/4/5/6/7, and headless BF6 observation.
- `dali_2k.yaml` is the second-floor/control-sensor firmware: group 0 lighting,
  HA command console, and Steinel HF 360 II sensor polling.
- `esphome/dali_esphome.h` is only a legacy placeholder. Do not add new logic
  there.

### Pushbutton couplers

- Existing DALI-1 PB couplers in BF6/direct-control mode work well and should
  stay unchanged for now.
- In current mode, the couplers command the lamps directly. ESP32 observes the
  same frames, updates local/HA state, and does not need to retransmit them.
- Headless dispatch is active only when YAML sets `headless_dispatch: true`.
- Phantom-address mapping remains a future option, not an active task.
- Power-cut behavior and post-power-cut actions need separate consideration and are future work.

### DALI-2 input devices

- Steinel HF 360 II DALI-2 IPD is working through ESPHome polling.
- Instance layout in use:

| Instance | Type | Meaning | Current handling |
|---:|---:|---|---|
| 0 | 4 | Light/lux | 2-byte poll, `scale: 0.01` |
| 1 | 3 | Occupancy | 1-byte poll, raw hidden sensor plus HA text mapping |
| 2 | 0 | Temperature | 2-byte poll, `T_C = raw * 0.1 - 5` |
| 3 | 0 | Humidity | 1-byte poll, `H_percent = raw * 0.5` |

- `iquery` readback exists for common instance parameters.
- `iconfig`/`dali_input_config` SET paths are implemented but still need
  cautious hardware validation. Always read the current value first, write one
  parameter, then read it back before doing broader changes.

## Active Code Work

| Priority | Area | Need | Status |
|---|---|---|---|
| P1 | ESPHome command console | Add strict numeric parsing/range checks for `level`, `config`, `iquery`, and `iconfig`; reject invalid text and overflow instead of silently using 0 or wrapped values. | ✅ done |
| P1 | Headless dispatch | Extend dispatch keys to include DALI-2 instance and legacy address selector where appropriate, so input instances and DAPC/command frames cannot collide. | ✅ done |
| P1 | Discovery/export | Model hybrid short addresses that are both control gear and input device. Current YAML/JSON paths can collapse these into only `input_device`. | ✅ done |
| P2 | Native CLI display | Ensure pure input devices detected without gear status are visible in human `discover`/`inventory` output, not only in the data model/export. | ✅ done |
| P2 | Cross-core state handoff | Harden ESPHome dirty-flag/string handoff so bus state, sensors, monitor strings, and command results cannot be lost or torn under higher event rates. | ✅ done |
| P2 | Dispatch result semantics | Publish inferred HA light state only after the DALI control command was actually queued/sent successfully. | ✅ done |
| P2 | Sensor polling | Handle scheduler enqueue failures and consider making 2-byte input reads contiguous scheduler sequences if more sensors are added. | ✅ done |
| P3 | Light registry | Set `member_groups` before light registration, or refresh the registry snapshot after codegen sets it. | ✅ done |

## Active Field/Config Tasks

- Tune Steinel occupancy behavior from HA:
  - read first: `iquery a0:1 hold-timer`
  - write cautiously: `iconfig a0:1 set-hold-timer <N>`
  - read back: `iquery a0:1 hold-timer`
- Investigate Steinel detection range and deadtime only after readback is proven.
- Decide whether `dali_1k.yaml` should gain the HA command console, or whether
  the first-floor firmware should stay as a minimal control firmware.
- Startup writes: refine the 10 s startup write suppression so intentional HA
  user commands during boot are not silently ignored.
- Deferred refresh: optionally narrow deferred refresh to the affected DALI target
  instead of polling all configured lights.
- After power cut: decide and implement post-power-cut behavior (e.g. whether the
  ESP32 should query or command lights after a bus power restoration event).
- Clean remaining mojibake/stale comments in YAML and source as encountered.

## Source Layout

| Path | Role |
|---|---|
| `components/dali` | Reusable C protocol, scheduler, PHY, discovery, dispatch, and device-type stack |
| `main/main.c` | Native ESP-IDF diagnostic app entry point |
| `main/dali_diag.c/.h` | App-specific serial diagnostic CLI |
| `esphome/components/dali` | Active ESPHome external component |
| `dali_diag.yaml` | ESPHome diagnostic/discovery firmware |
| `dali_1k.yaml` | Bus 1 control firmware |
| `dali_2k.yaml` | Bus 2 control/sensor firmware |
| `dali_command_reference.md` | Command catalog and parser/protocol reference |

## Documentation Policy

- Keep this file as the source of truth for active state and remaining work.
- Keep `dali_command_reference.md` as the protocol/command catalog.
- Avoid adding new session-log TODO files. If a future note has only a few live
  tasks, add them here instead.

## Build And Test Commands

ESPHome:

```powershell
esphome compile dali_diag.yaml
esphome compile dali_1k.yaml
esphome compile dali_2k.yaml
esphome run dali_diag.yaml
esphome logs dali_diag.yaml
```

Native ESP-IDF:

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
