# DALI-ESP Agent Guide

This file is the short orientation guide for coding agents. Read `current_status.md`
first for the active project state, then use the focused planning files below.

## Project Aim

Build an ESP32-based DALI-2 master/controller using the MikroE DALI-2 Click board.
The project is a learning and development project, with low-level protocol access
kept available for debugging and experimentation.

Long-term goals:

- Communicate with DALI and DALI-2 devices.
- Support control gear, DALI-2 input devices, instances, and 24-bit frames.
- Provide native diagnostic firmware for stack bring-up.
- Provide an ESPHome/Home Assistant integration after the core stack is proven.

## Context Files

- `current_status.md` - current overview, verified state, immediate priorities.
- `todo_pre_hardware.md` - software cleanup before hardware bring-up.
- `todo_hardware_bringup.md` - ESP-IDF, logic analyzer, and Lunatone validation flow.
- `diagnostic_discovery_plan.md` - bus discovery, inventory, identify, switch training.
- `todo_esphome_release.md` - end-user diagnostic firmware and final ESPHome flow.
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
`esphome/dali_esphome.h` is a stub until hardware validation is complete.

Do not put protocol, timing, sensor, or addressing logic into ESPHome entities.
The ESPHome layer should map configured entities to `DaliTarget` values and call
the control/protocol APIs.

## Timing And ISR Rules

- GPTIMER tick: 104 us.
- DALI bit period: about 833.3 us.
- DALI half-bit period: about 416.7 us.
- TX-to-RX settle: 7 ms.
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
C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe -B build -G "MinGW Makefiles"
C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe --build build
C:\Espressif\tools\cmake\4.0.3\bin\ctest.exe --test-dir build --output-on-failure
```
