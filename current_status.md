# DALI-ESP Current Status

**Last updated:** 2026-06-09 (rev 22)
**Framework:** ESP-IDF v6.0.1 native CMake
**Hardware target:** ESP32-DevKitC-VE / ESP32-WROVER-E + MikroE DALI-2 Click
**Timer:** GPTIMER, 104 us alarm, 4x oversampling of the DALI half-bit

## Status Summary

The pre-hardware software stack is complete enough to move into hardware
bring-up:

- `DaliFrame`, error types, timing constants, and stats exist.
- SPSC RX ring buffer exists and is host-tested.
- PHY TX encode path and RX Manchester decode path exist and are host-tested;
  TX now guards for bus idle and suppresses RX self-echo / settle-period edges.
- Scheduler queue/state machine exists with retry, reply timeout, send-twice,
  fixed-size sequences, 8-bit reply gating, task-context trace hooks, raw
  24-bit unsolicited event routing, and host mock tests.
- Protocol builders and response parser dispatch exist for standard/common
  16-bit commands plus standard DALI-2 device/instance discovery commands;
  single-frame special command builders exist for commissioning, DTR,
  enable-device-type, and memory opcodes; parser helpers cover packed
  fade-time/rate and MSB-first multi-byte input values.
- Generic DALI-2 input-device helpers in `dali_input_device` classify standard
  instance types without applying vendor profiles: type 3 occupancy/motion and
  type 4 light are marked usable from standard type, while generic/unknown
  values remain unverified.
- Reusable `dali_discovery` helpers provide scheduler-agnostic short-address
  scan/inventory storage and generic DALI-2 input-device instance discovery;
  native diagnostic `scan`, `discover`, `inventory`, and `instances` use this
  layer through a thin scheduler adapter.
- Vendor/profile helpers are separate from the generic protocol layer:
  Lunatone-only instance scaling queries live in `dali_lunatone`, and the
  Steinel HF 360 II instance profile/conversions live in `dali_steinel`.
- Static mapping helpers in `dali_mapping` validate configured outputs and
  input instances without assigning room/entity semantics.
- Shared protocol limits for frame lengths, address ranges, instance ranges,
  broadcast bytes, and DAPC levels live in `dali_frame`.
- `dali_control` translates short/group/broadcast targets and Home
  Assistant-style brightness values through the protocol builders. It covers
  DAPC, normal output-level instructions through `GO TO SCENE`, recall
  min/max, `QUERY STATUS`, and generic metadata-backed addressed 16-bit
  queries/configuration commands.
- Native diagnostic CLI exists with `help`, `stats`, `trace on/off`, `read`,
  `reset`, scheduler-routed `raw`, `dtr`, `special`, `special-list`,
  named output helpers
  `level/off/up/down/step-up/step-down/step-off/on-step/dapc-seq/last/scene`,
  `max/min/status`, generic `query <target> <name> [param]`, `query-list`,
  generic `config <target> <name> [param]`, sequenced
  `config-dtr0 <target> <name> <value> [param]`, `config-list`, `scan`,
  `discover`, `inventory`, generic `instances <addr>`, and `identify`.
- Added `sdkconfig.defaults` for ESP32-WROVER-E bring-up: 8 MB flash,
  240 MHz CPU, 1000 Hz FreeRTOS tick, and ISR-adjacent GPIO/GPTIMER/FreeRTOS
  IRAM options; fresh generated config keeps `esp_timer_get_time()` in IRAM.
- ESPHome integration is intentionally still a stub.

Latest known verification:

- `idf.py build` passes as of 2026-06-09.
- Fresh build from `sdkconfig.defaults` passes and generates 8 MB flash image
  arguments as of 2026-06-05.
- Host tests pass as of 2026-06-09: 10 suites, 173 `RUN_TEST` cases.
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
DALI entity mapping / release integration (mapping helpers ready, release future)
DALI discovery / inventory helpers        (implemented, host-tested)
DaliControl                               (draft implemented)
DaliProtocol                              (core implemented, hardware sensor polling pending)
DaliScheduler                             (implemented, host-tested)
DaliPhy                                   (software implemented, hardware pending)
DaliRingBuf                               (implemented, host-tested)
DALI-2 Click
DALI bus
```

## Immediate Priorities

1. Validate native firmware on the ESP32 with serial CLI and logic analyzer.
2. Compare real DALI traffic against Lunatone DALI Cockpit captures.
3. Bring up known control gear first, then the Steinel HF 360 II DALI-2 sensor.
4. Only after native diagnostics are reliable, build the ESPHome-flashable
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

Steinel conversions available in `dali_steinel` after input-value reads work:

- Temperature: `T_C = (binValue * 0.1) - 5`
- Humidity: `H_percent = binValue * 0.5`

## Open Decisions / Questions

| Topic | Status |
|---|---|
| GPIO wiring for MikroE DALI-2 Click | Needs hardware confirmation. Do not use GPIO 16/17 on WROVER-E. |
| HA brightness 0 behavior | Prefer explicit `OFF`; confirm on hardware. |
| Scheduler RX handling | Raw 8-bit reply vs 24-bit event routing implemented; protocol-level event parsing pending. |
| DALI-2 instance discovery | Generic count/type CLI implemented; value polling and profile application pending. |
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

IDE compile database:

- Native ESP-IDF configure/build generates `build/compile_commands.json`.
- Host-test configure with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` generates
  `test/build/compile_commands.json`.
