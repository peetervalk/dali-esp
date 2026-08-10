# DALI-ESP Current Status

**Last updated:** 2026-08-10

**Known-working deployment/component baseline:** `v1.0.1` (`0302d70`), for
the two recorded site configurations; this is not a conformance claim.

**Status:** The ESPHome controller is operational on the two known installations.
The repository has a strong reusable C foundation, but CLI completeness, several
DALI-2 protocol paths, and general-purpose ESPHome reliability remain active work.

This file is the source of truth for current verified state and open work. Completed
defect narratives belong in Git history or a changelog, not in this file.

## Project Aims

1. **CLI completeness:** if an operation can be performed with DALI commands, the
   native CLI should expose a reliable typed workflow for it. The CLI is the
   reference tool for proving protocol behavior on a real bus before it reaches
   the controller layer.
2. **ESP32 DALI controller:** use the shared CLI/protocol stack to provide a
   working DALI controller for ESP32, primarily through ESPHome and Home Assistant.

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

## Goal Assessment

| Aim | Current state |
|---|---|
| CLI completeness | **Partial.** Common Part 102 gear control, diagnostics, discovery, commissioning, capture, and polling are useful, but shared DT6/DT8, memory, input-configuration, vendor, control-device, and atomic/send-twice workflows are not fully exposed as typed native CLI verbs. |
| ESP32 / ESPHome controller | **Working installation-grade baseline.** The two known sites have operational brightness control, observation, sensor polling, diagnostics, and discovery. It is not yet a general, fully state-correct DALI controller. |
| Protocol separation | **Directionally strong.** The reusable C stack is independent of ESPHome, but several protocol sequences and raw opcode paths still need to move out of `dali_component.cpp`. |
| Standards confidence | **Selected workflows verified, not complete conformance.** Host tests and recorded hardware results provide useful evidence, but some tests repeat implementation constants rather than independent standard-derived vectors. The project is not DALI Alliance certified. |

The next development phase should prioritize protocol and state correctness before
adding broad new device support.

## Verification Baseline

### Verified locally on 2026-08-10

- The audit began from commit `0302d70` (tag `v1.0.1`); all three working-tree
  deployment YAMLs now reference that tag.
- All 19 host test executables pass.
- The native ESP-IDF firmware builds successfully with ESP-IDF 6.0.1.
- All three pinned YAMLs pass `esphome config` with ESPHome 2026.6.2 and
  resolve the published `v1.0.1` external component.
- The current `dev` worktree compiles and links as a local ESPHome 2026.6.2
  external component through `_local/dali_diag_local.yaml`; dispatch schema boundary
  and backwards-compatibility cases also pass.
- The ESPHome protocol wrapper set matches the 19 reusable C source files.
- The input-device configuration opcode audit is complete against Part 103:2022,
  Part 301:2017, and the Part 303/304 2017+AMD1:2024 command tables. Incorrect
  generic timer aliases and non-standard Part 301/304 commands have been removed
  from the supported surface; independent golden vectors cover the corrected
  command frames.
- Control-gear device-type discovery now distinguishes the DALI-2 single,
  multiple, and no-type/end replies, rejects malformed enumeration, and reports
  fixed-list truncation. Host vectors cover the sentinel and capacity boundaries;
  this path has not been re-verified on hardware.
- Part 102 control-gear identity discovery now reads the DALI-2 Bank 0 identity
  fields at `0x03..0x14`, including hardware version, without touching reserved
  location `0x01`. The unsupported duplicate Bank 1 identity model is removed,
  and invalid short addresses are rejected before bus traffic. Independent host
  vectors cover the layout and address boundaries; hardware is not re-verified.
- ESPHome control-device `memwrite` now queues its seven dependent logical steps
  as one contiguous scheduler entry, including adjacent expansion of both
  send-twice commands. Host tests cover nine-frame ordering, queue-boundary
  admission, and execution before a following local command; the ESPHome build
  is verified, but the write path has not been re-verified on hardware.
- Tracked source and documentation files are valid UTF-8; no active mojibake
  cleanup is required.

### Recorded hardware state

These results predate the 2026-08-10 static audit and were not re-tested during it:

- Hardware target: ESP32-DevKitC-VE / ESP32-WROVER-E with MikroE DALI-2 Click.
- Known wiring: TX on GPIO18, RX on GPIO19.
- GPIO16 and GPIO17 must not be used on WROVER-E because they are connected to
  PSRAM.
- Bidirectional DALI traffic works on the two installed buses.
- Existing DALI-1 pushbutton couplers in BF6/direct-control mode operate the
  lamps directly; the ESP32 observes their traffic for Home Assistant state.
- Native and ESPHome diagnostics have been used successfully for control, queries,
  discovery, inventory, capture, and sensor investigation.

### Not yet verified

- ESPHome schema/C++ compatibility across the stated supported ESPHome versions.
- Independent conformance of every protocol constant, timing boundary, memory
  layout, discovery path, and commissioning collision case.
- Hardware round-trip behavior of all input-device configuration writes, DT6/DT8
  helpers, memory operations, and vendor helpers.

## Current Software State

### Shared DALI stack

- `components/dali` contains the reusable plain-C PHY, RX ring buffer, scheduler,
  protocol/control APIs, discovery, control-gear commissioning, memory helpers,
  DT6/DT8 builders, input-device support, events, mapping, and dispatch.
- Static allocation and the buffer-first ISR model are preserved in the timing
  and protocol layers.
- Part 103 events are decoded into canonical source fields, reject command and
  reserved frames, preserve all ten event-information bits, and use the sparse
  Part 301/type 1 event values. Independent vectors cover all five normal source
  schemes; additional standard-derived cases cover power notifications and
  malformed data.
- Part 103 generic instance configuration plus Part 301/type 1, Part 303/type 3,
  and Part 304/type 4 configuration/query builders have an independently audited
  opcode surface. This is software-level evidence only; configuration writes have
  not yet completed hardware read/write/read-back validation.
- The Part 102 memory helper reads the common Bank 0 identity block, including
  firmware and hardware versions. Discovery performs this 16-bit read only for
  confirmed control gear; pure Part 103 devices require a future typed 24-bit
  memory path. Generic byte access to optional Bank 1 remains available.
- DT6 and DT8 builder APIs exist and are host-tested, but are not yet fully
  surfaced through the native CLI or comprehensively hardware-verified.
- DT1 and other specialized/legacy device types remain intentionally unimplemented.

### Native diagnostic CLI

- `main/main.c` and `main/dali_diag.c` provide the ESP-IDF serial diagnostic
  application.
- Common gear commands and queries, DTR/config operations, scan/discovery,
  control-gear commissioning, inventory/export, input polling, capture, and
  coupler-finding workflows are present.
- Native `raw` sends one arbitrary 16- or 24-bit frame. It is a diagnostic escape hatch, not a
  substitute for typed atomic workflows; unlike the ESPHome console it has no
  `raw2` send-twice verb.
- Native event capture/export reports canonical Part 103 source fields and full
  event information. Device/Instance push-button events are typed from the
  discovery cache rather than guessed from their event value.
- The CLI parser/help/dispatch layer currently has no host-test coverage.

### ESPHome component

- Active component: `esphome/components/dali`.
- `dali_diag.yaml` provides discovery and diagnostics, including scan, identify,
  find-couplers, target controls, bus monitoring, group-map output, and generated
  YAML log lines.
- Light entities currently expose brightness only. Shared DT8 support is not yet
  mapped to Home Assistant colour-temperature, XY, or RGB controls.
- The command console includes `raw`, `raw2`, memory, DTR, instance-query, and
  instance-configuration helpers. These do not imply equivalent native CLI or
  hardware validation.
- Control-device `memwrite` is one scheduler-contiguous sequence and cannot be
  partially enqueued. Its `OK` result means queued, not device-acknowledged or
  read-back verified, and it does not exclude another physical bus master.
- Matching Device/Instance events request an immediate authoritative sensor
  poll; event information is never published as a generic sensor value.
- Bus monitoring and Find Couplers retain and format the canonical Part 103
  source scheme, selectors, and full event information.
- ESPHome exposes discovery, not a guarded commissioning workflow.
- `esphome/dali_esphome.h` is an unused legacy placeholder.

### Next-release API migrations

The corrected input-configuration surface intentionally removes invalid generic
timer/hysteresis/deadtime aliases and non-standard Part 301/304 APIs. C callers
must migrate to the explicit `pb`, `occ`, and `light` type-specific builders.
ESPHome callers must use the `pb-*` and `light-*` names; the established Part 303
occupancy names remain available. This is a source/API migration, not evidence of
hardware write verification.

The corrected memory identity API removes `DaliMemoryBank1Identity`,
`dali_memory_read_bank1_identity()`, the Bank 1 identity-layout macros, and the
`has_bank1`/`bank1` discovery fields. It also removes
`DALI_MEMORY_BANK0_OFFSET_INDICATOR` and `DALI_MEMORY_BANK_IMPLEMENTED`;
`DALI_MEMORY_BANK0_OFFSET_SERIAL` remains as an alias but changes from `0x0A` to
the correct `0x0B`. `DaliMemoryBank0Identity` gains `hw_major`/`hw_minor`; its
existing `serial` member remains the standard eight-byte identification number.
This changes the layouts of `DaliMemoryBank0Identity`, `DaliDiscoveryDeviceInfo`,
and `DaliDiscoveryInventory`. Callers using the removed typed Bank 1 model must
migrate to generic bank access.

`DALI_SEQUENCE_MAX_STEPS` increases from 4 to 7 so the control-device memory
write fits in one queue entry. This changes the public `DaliSequence` layout and
requires all callers to rebuild. In the ESP32 build, the active-sequence and
16-entry scheduler queue storage increase by 612 bytes in total.

## Installation State

The three active firmware configurations pin their external component to
`v1.0.1`, the current known-working release:

| Configuration | Role |
|---|---|
| `dali_diag.yaml` | Tracked diagnostic/discovery firmware |
| `_local/dali-1k.yaml` | First-floor site firmware; 16 control gear and group entities for groups 0/2/3/4/5/6/7 |
| `_local/dali-2k.yaml` | Second-floor site firmware; group 0 lighting, HA console, and Steinel HF 360 II polling |

The entire `_local` directory is deliberately ignored by Git. This checkout also
contains `_local/dali_diag_local.yaml`, a compile-test copy of the tracked diagnostic
configuration, and `_local/secrets.yaml`, whose values are explicitly marked
dummy/compile-only and must not be deployed. Inspect and back up any real site
files separately; normal `git status` does not show changes under `_local`.

### Observed Steinel instance layout

| Instance | Type | Meaning | Authoritative handling |
|---:|---:|---|---|
| 0 | 4 | Light/lux | Two-byte poll, `scale: 0.01` |
| 1 | 3 | Occupancy | One-byte poll and HA mapping |
| 2 | 0 | Temperature | Two-byte poll, `T_C = raw * 0.1 - 5` |
| 3 | 0 | Humidity | One-byte poll, `H_percent = raw * 0.5` |

Polling is authoritative for occupancy. A matching Device/Instance event requests
an immediate poll, but the event information itself is not treated as a sensor
value. This preserves the observed Steinel telemetry without confusing it with
the debounced occupancy state returned by `QUERY INPUT VALUE`.

## Prioritized Work

### P0 — Protocol correctness and conformance

- Enforce send-twice and interframe timing boundaries. Strengthen commissioning
  RANDOMIZE timing, collision handling, equal-random-address recovery, and the
  separation of gear and control-device address spaces.

### P0 — Transaction and runtime reliability

- Add a scheduler-level atomic transaction/session facility for DTR operations,
  ENABLE DEVICE TYPE sequences, memory access, DT8 multi-byte queries,
  send-twice commands, discovery, and commissioning.
- Make scans exclusive or explicitly pause/reject normal polling and HA traffic.
- Handle every enqueue result. Never report `OK` for a dropped command; pace full
  refresh, retry missed entries, and expose queue depth/high-water/drop diagnostics.
- Fix the cross-task light-state mailbox so on/off and level are published as one
  coherent update without losing a newer notification.
- Update light-command deduplication only after confirmed transmission, or
  invalidate its cached state after TX/bus errors, so a failed command can be retried.
- Correct observed-state semantics for RECALL MAX, STEP DOWN AND OFF, TOGGLE, and
  broadcast commands. Query actual level whenever the exact state is not known.

### P1 — CLI completeness

- Maintain a capability matrix with: shared API, native CLI verb, independent host
  vector, real-bus verification, and ESPHome exposure.
- Add typed native verbs for memory, DT6, DT8, input-device query/configuration,
  vendor helpers, and Part 103 control-device workflows.
- Add native `raw2` with enforced send-twice timing; do not rely on two manually
  entered `raw` commands.
- Fill remaining common Part 102 gaps, including Continuous Up/Down and DAPC MASK.
- Add host tests for CLI parsing, dispatch, help/command-table parity, validation,
  response formatting, and multi-frame workflow behavior.

### P1 — ESPHome correctness and architecture

- Default a short-address light's query target to its control target; boot and
  deferred refresh must retry queue failures.
- Replace the blanket ten-second startup write suppression with logic that
  distinguishes restore/default writes from intentional user commands.
- Extend the compact Part 103 dispatch key if a site needs to distinguish
  Device-Group from Instance-Group sources or match instance type; the canonical
  event/capture path retains these fields, but the five-field rule key does not.
- Replace the 128-byte Find Couplers summary with a paged/exportable result; all
  canonical captures are logged, but the Home Assistant aggregate can truncate.
- Move raw opcodes and memory/DTR sequences out of `dali_component.cpp` into
  reusable typed C APIs.
- Use board-aware ESPHome GPIO schemas, reject TX=RX and invalid output pins, and
  honor the documented WROVER-E restrictions.
- Check task-creation results and remove or validate hard-coded Core 1 assumptions,
  particularly for single-core ESP32 targets.
- Define recovery after DALI bus faults and bus-only power cycles. Distinguish
  current fault/availability from cumulative fault history.

### P1 — Release and verification quality

- Add CI that compiles a clean external-component checkout from the release tag and
  validates the Python schema, ESPHome C++ layer, YAML, and wrapper packaging.
- Add the native ESP-IDF build to CI alongside the existing 19-suite host workflow.
- Keep one intentional ESPHome source-inclusion path. Remove the unused component
  `CMakeLists.txt` path and stale fallback references if the Python-copy/wrapper
  route remains authoritative.
- Enforce or document the actual ESP-IDF and ESPHome version requirements; the
  current `manifest.json` is not enforcement for normal external components.
- Synchronize `dali_command_reference.md` with implemented native and ESPHome
  verbs, and keep local filenames hyphenated in all documentation.
- Complete release provenance: project SPDX identifiers and the full vendored
  Unity MIT license/third-party notice.
- Document the next-release C migration for the intentionally changed
  `DaliInputEvent` and `DaliDispatchKey` field names/layout before tagging it.
- Clarify the bus topology: direct-control couplers also transmit. Their current
  coexistence is field-tested, but it is not equivalent to collision-safe
  single-master arbitration.

### P2 — Further development

- Map DT8 to Home Assistant colour-temperature, XY, and appropriate RGB/RGBWAF
  light traits after native CLI and hardware validation.
- Add typed Part 303/type 3 occupancy/binary-sensor and Part 304/type 4
  illuminance profiles.
- Add guarded commissioning to ESPHome only after shared commissioning is robust;
  otherwise retain commissioning as a native diagnostic workflow.
- Improve scene/fade UX, targeted deferred refresh, and post-transition final
  readback.
- Add capture replay, parser fuzzing, scheduler timing tests, and hardware-in-loop
  tests for collisions, queue pressure, bus faults, and power restoration.
- Add DT1 and other device types only when an installation requires them, following
  the shared DT6/DT8 module pattern.

## Operational Constraints

- Do not imply DALI Alliance certification or complete IEC 62386 coverage.
- The controller has no proven collision-detection/arbitration strategy. Existing
  direct-control couplers work on the installed buses, but simultaneous
  transmissions remain a risk.
- Treat input-device configuration writes as experimental until real-bus
  read/write/read-back validation is complete.
- Use COM6 only for hardware work. If COM6 is unavailable, stop.
- Do not use GPIO16 or GPIO17 on the WROVER-E target.

## Source Layout

| Path | Role |
|---|---|
| `components/dali` | Reusable C protocol, scheduler, PHY, discovery, dispatch, memory, and device-type stack |
| `main/main.c` | Native ESP-IDF diagnostic application entry point |
| `main/dali_diag.c/.h` | App-specific serial CLI |
| `esphome/components/dali` | Active ESPHome external component |
| `dali_diag.yaml` | Tracked diagnostic/discovery firmware |
| `_local/dali_diag.yaml` | Ignored compile-test copy of the diagnostic firmware |
| `_local/secrets.yaml` | Ignored dummy compile-only secrets; never deploy |
| `_local/dali-1k.yaml` | Ignored first-floor site firmware |
| `_local/dali-2k.yaml` | Ignored second-floor site firmware |
| `dali_command_reference.md` | Protocol and command catalog |
| `esphome_verb_readme.md` | ESPHome console examples and notes |
| `steinel_bank2_reference.md` | Installation-specific Steinel memory observations |

## Build and Test Commands

ESPHome configuration/build:

```powershell
esphome compile dali_diag.yaml
esphome compile _local/dali_diag.yaml  # ignored compile-test copy; dummy secrets
esphome compile _local/dali-1k.yaml
esphome compile _local/dali-2k.yaml
```

For an uncommitted/dev component compile, change only the ignored
`_local/dali_diag.yaml` source to `type: local` with
`path: ../esphome/components`, then restore it from the tracked YAML. Keep the
tracked deployment configuration pinned to the release tag.

Native ESP-IDF:

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
idf.py build
idf.py -p COM6 flash monitor
```

Host tests:

```powershell
cd test
C:\msys64\ucrt64\bin\mingw32-make.exe --directory build
C:\msys64\ucrt64\bin\mingw32-make.exe --directory build test CTEST_OUTPUT_ON_FAILURE=1
```

## Documentation Policy

- Keep this file limited to current verified state, constraints, and open work.
- Record completed changes in Git history or a changelog; remove them from the
  active backlog.
- Keep protocol/command detail in `dali_command_reference.md`.
- Label claims as host-tested, hardware-verified, or still unverified.
- Do not add session-log TODO files; merge active work into the prioritized list
  above.
