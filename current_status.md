# DALI-ESP Current Status

**Last updated:** 2026-06-05 (rev 5)
**Framework:** ESP-IDF v6.0.1 native CMake
**Hardware target:** ESP32-DevKitC-VE / ESP32-WROVER-E + MikroE DALI-2 Click
**Timer:** GPTIMER, 104 us alarm, 4x oversampling of the DALI half-bit

## Status Summary

The core software stack is implemented enough for pre-hardware cleanup and
bring-up:

- `DaliFrame`, error types, timing constants, and stats exist.
- SPSC RX ring buffer exists and is host-tested.
- PHY TX encode path and RX Manchester decode path exist and are host-tested.
- Scheduler queue/state machine exists with retry, reply timeout, send-twice,
  and host mock tests.
- Protocol builders and response parser dispatch exist for standard 16-bit
  commands plus draft DALI-2 instance commands.
- `dali_control` is a first command-translation layer for short/group/broadcast
  targets and Home Assistant-style brightness values.
- Native diagnostic CLI exists with `stats`, `trace on/off`, `reset`, `raw`,
  `scan`, and `query`.
- ESPHome integration is intentionally still a stub.

Latest known verification:

- `idf.py build` passes as of 2026-06-05.
- Host tests pass: 6 suites, 78 tests.
- Real hardware flashing, timing, loopback, and device communication are still
  pending.

## Current Direction

There are two distinct workflows:

1. **Developer stack debugging**
   - Native ESP-IDF diagnostic firmware.
   - Serial CLI.
   - Logic analyzer / oscilloscope.
   - Lunatone DALI USB / DALI Cockpit as the main reference tool.
   - This path validates PHY timing, scheduler behavior, protocol frames, and
     real DALI bus behavior before ESPHome is involved.

2. **End-user discovery and release**
   - A prebuilt ESPHome-flashable diagnostic/discovery firmware is the first
     user-facing release artifact.
   - The user flashes it through ESPHome Web / ESP Web Tools, discovers DALI
     addresses, groups, instances, and switch inputs, then fills final ESPHome
     YAML from that discovery result.
   - The final ESPHome firmware exposes explicit configured entities. Discovery
     assists configuration but does not guess room names or entity semantics.

## Architecture

```text
ESPHome / Home Assistant integration      (stub)
DALI entity mapping / release integration (future)
DaliControl                               (draft implemented)
DaliProtocol                              (core implemented, sensor work pending)
DaliScheduler                             (implemented, host-tested)
DaliPhy                                   (software implemented, hardware pending)
DaliRingBuf                               (implemented, host-tested)
DALI-2 Click
DALI bus
```

## Immediate Priorities

1. Finish pre-hardware software cleanup from `todo_pre_hardware.md`.
2. Validate native firmware on the ESP32 with serial CLI and logic analyzer.
3. Compare real DALI traffic against Lunatone DALI Cockpit captures.
4. Bring up known control gear first, then the Steinel HF 360 II DALI-2 sensor.
5. Only after native diagnostics are reliable, build the ESPHome-flashable
   diagnostic/discovery firmware described in `todo_esphome_release.md`.

## Known Target Sensor

Steinel HF 360 II DALI-2 IPD UP:

- EAN / GTIN: `4007841064280`
- Article number: `064280`
- DALI Alliance product ID: `3742`
- DALI parts: IEC 62386-101, -103, -303, -304

Expected non-ECO instances:

| Instance | Type | Meaning |
|---:|---:|---|
| 0 | 4 | Brightness measuring |
| 1 | 3 | Motion detection |
| 2 | 0 | Generic temperature |
| 3 | 0 | Generic humidity |

Steinel conversions to implement after input-value reads work:

- Temperature: `T_C = (binValue * 0.1) - 5`
- Humidity: `H_percent = binValue * 0.5`

## Open Decisions / Questions

| Topic | Status |
|---|---|
| GPIO wiring for MikroE DALI-2 Click | Needs hardware confirmation. Do not use GPIO 16/17 on WROVER-E. |
| HA brightness 0 behavior | Prefer explicit `OFF`; confirm on hardware. |
| Scheduler RX handling | Needs solicited-reply vs unsolicited-event split. |
| DALI-2 instance discovery | Manual/profile-driven first; automatic discovery later. |
| ESPHome component packaging | In-tree custom component first; external component later. |
| DALI-2 firmware update / DFU | Out of scope. |

## Build Commands

Native ESP-IDF:

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
