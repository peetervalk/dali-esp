# DALI-ESP Current Status

**Last updated:** 2026-06-29 (session 2)
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
- The ESPHome HA command console includes `raw` and `raw2` for direct 16/24-bit
  frame entry; use these for Steinel Bank 2 diagnostics if helper verbs produce
  suspect output.
- Steinel helper verbs for `memread`, `memwrite`, `dtrcheck`, and DT303
  occupancy timer `iquery`/`iconfig` now use the verified 24-bit Part 103
  control-device frames.
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

- Steinel HF 360 II DALI-2 IPD is working through ESPHome.
- Occupancy (instance 1, 1-byte) now receives immediate push updates from
  unsolicited 24-bit event frames; the 5 s poll_interval is a fallback only.
- Lux (instance 0, 2-byte) and temperature/humidity are still polled — event
  frames only carry 8 bits of event_code, insufficient for a 16-bit value.
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
| P3 | `raw` command parser | `parse_raw_len` bounds `parse_uint` to `[16, 24]` then immediately checks `!= 16 && != 24` — the first range silently accepts 17-23 before the second rejects them. Not a bug, but misleading. Either tighten the `parse_uint` bounds to remove the second check, or widen them so the intent of the second check is clear. | open |
| P3 | `memwrite` DTR1 lifetime | seq2 relies on DTR1 surviving from seq1 (bank select set in seq1 step 0). No intermediate command should reset it, but this is an implicit dependency. If the scheduler ever interleaves unrelated commands between the two sequences this would silently write to bank 0. Worth adding a DTR1 reset at the start of seq2 as a defensive measure. | open |
| P2 | `input_24bit` dispatch instance matching | `headless_dispatch` accepts `frame_kind: input_24bit`, but `dali_dispatch` currently compares YAML `instance` directly with the raw DALI-2 event instance byte. Bus monitor decodes that byte into `inst=<N>` and `scheme=<N>`, so YAML `instance: 0` may not match a real decoded instance 0 event. Before using Lunatone MC+ event-message mode seriously, make dispatch compare against the decoded instance number, update comments/docs, and add host tests. Until then use `instance: any` for 24-bit dispatch experiments. | open |

## Active Field/Config Tasks

- Tune Steinel occupancy behavior from HA:
  - read first: `iquery a0:1 hold-timer`
  - write cautiously: `iconfig a0:1 set-hold-timer <N>`
  - read back: `iquery a0:1 hold-timer`
- Investigate Steinel detection range and deadtime only after readback is proven.
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

## Pre-1.0 Release Assessment

This section records the findings from a full codebase review done 2026-06-29, before the first public release. Items are grouped by urgency. Work through them in order before tagging 1.0.

### Must Fix Before 1.0

#### ✅ 1. Two-byte input sensor reads can produce corrupt values

**What happens:** Sensor polling for 2-byte input instances (e.g. lux) schedules two separate
enqueue operations — one for the MSB reply and one for the LSB reply. If the first enqueue
(MSB) succeeds but the second enqueue (LSB) fails due to a full scheduler queue, the old MSB
byte is left stranded in the accumulator. The next successful poll then combines the stale old
MSB with a fresh new LSB and publishes a garbage 16-bit value. This is not a rare edge case:
it can happen under normal bus load if the scheduler queue is momentarily full, and the result
is a sensor value that is wrong by an arbitrary amount with no indication that anything went
wrong.

**Where it is:** `esphome/components/dali/dali_component.cpp` around lines 77–100, in the
input poll completion handler. The pattern is: enqueue MSB read, on success enqueue LSB read,
on success combine. The failure path of the LSB enqueue does not cancel the MSB or mark the
accumulator dirty.

**Fix required:** Make the MSB and LSB reads a contiguous two-step scheduler sequence so that
both are either queued together or neither is queued. Alternatively, if only atomic enqueue of
a pair is not available, skip the entire poll cycle if either enqueue fails and log a warning.
Do not leave a half-updated accumulator.

---

#### ✅ 2. Bus-stuck condition is defined but never surfaced

**What happens:** `DALI_ERR_BUS_STUCK` is defined in the protocol stack to indicate that the
DALI bus line is being held low — which happens when a coupler fails short, there is a wiring
fault, or a device is wedging the bus. The error code exists but nothing in the ESPHome layer
checks for it or publishes it anywhere. If the bus goes stuck, the user observes silence: all
commands stop working, all queries return nothing, and there is no indication in Home Assistant
or in the ESPHome logs that a hardware-level fault has occurred. The user's only debug path is
to guess.

**Where it is:** `components/dali/dali_frame.h` defines the error. The ESPHome component in
`esphome/components/dali/dali_component.cpp` has many places that handle scheduler enqueue
return codes but none of them distinguish bus-stuck from other error conditions or propagate
the state to any HA-visible entity.

**Fix required:** In the ESPHome component, detect `DALI_ERR_BUS_STUCK` return codes and
publish the fault state to a diagnostic text sensor (or a binary sensor) that is visible in
Home Assistant. At minimum, log it at `ESP_LOGE` level so that it appears prominently in
ESPHome logs. A reasonable enhancement is to suspend TX after repeated stuck errors and require
a manual reset or a bus-clear command.

---

#### ✅ 3. Scene, step, and fade commands leave HA light state stale

**Verification result (2026-06-29):** The mechanism is already correctly implemented. No code
change needed. The full chain works as follows:

- `dali_dispatch()` calls `result_unknown()` for SCENE, DIM_UP, DIM_DOWN, RECALL_MIN,
  GO_TO_LAST, and OBSERVE with those opcodes. This sets `matched = true, has_state = false`.
- `notify_lights()` (called for every dispatch result from the DALI task) sees
  `matched && !has_state` and sets `s_deferred_query_pending_` (atomic, Core 1 → Core 0).
- `loop()` detects the flag, arms a 600 ms timer (`deferred_query_armed_`), and fires
  `start_refresh()` which enqueues QUERY_ACTUAL_LEVEL for all registered lights.
- `on_level_query_reply()` receives the real level and calls `mark_state_from_bus()`, which
  publishes the correct brightness to HA.

**Known limitation:** The 600 ms delay is a fixed window. If a DALI scene has a long fade time
programmed into the gear (longer than 600 ms), the level query catches the lamp mid-fade
rather than at its final value. The periodic `poll_interval_s_` poll (if configured) will
eventually correct this. For installations with long fades, increase the poll interval or
accept a brief mismatch.

---

#### ✅ 4. Release license and provenance are missing

**What happens:** The repository does not yet have a top-level `LICENSE`, SPDX headers, or a
short provenance note for third-party references. That is fine during private development, but
it is a public-release blocker: users need to know whether they can copy, modify, publish, and
ship firmware built from the component. This matters especially because `dali_input_config`
explicitly says some opcodes were cross-referenced with python-dali, and Unity is vendored
under MIT terms in `test/unity`.

**Fix required:** Choose the project license, add a top-level `LICENSE`, add SPDX identifiers
to project-owned source files, and add a short attribution/provenance note. Current
recommendation: Apache-2.0 for the project unless any implementation code was copied from
python-dali. If implementation code was copied or closely translated, pause and do a more
careful LGPL-3.0-or-later compatibility review before licensing.

---

#### 5. ESPHome external-component install path needs clean-tag verification

**What happens:** The active ESPHome component has two source-inclusion paths: `__init__.py`
copies the reusable protocol stack into the ESPHome build tree as `.c.inc` files, while
`esphome/components/dali/CMakeLists.txt` also compiles sources from `../../../components/dali`.
There are also stale comments referencing fallback files that no longer exist. Local builds may
work because the full repo layout is present, but a public user installing the external
component from a git tag is the real release path.

**Fix required:** From a clean checkout, compile `dali_diag.yaml`, `dali_1k.yaml`, and
`dali_2k.yaml` using the same `external_components` git/tag path a user will use. Clean the
stale CMake comments and keep exactly one intentional source-inclusion strategy. The release
should be tagged only after the clean external-component build path is proven.

---

#### ✅ 6. Headless dispatch is hardcoded and fundamentally breaks the ESPHome model

**Fixed 2026-06-29.**

`headless_dispatch` is now YAML-driven. Each entry is declared in the user's YAML as a
list under `headless_dispatch:` (frame_kind, address_kind, address, event_code, instance,
output_type, output_address, action, scene). `__init__.py` validates the list and
code-generates `var.add_dispatch_entry(...)` calls, exactly like lights and sensors.
`dali_component.cpp` owns a static `DaliDispatchEntry[32]` array populated at boot from
those calls. The weak-symbol / `dali_headless.cpp` compile path is gone from the component.
`dali_headless.cpp` remains in the repo inert under its `#ifdef USE_DALI_HEADLESS` guard
(never defined) and can be deleted at any time.

All four active YAML files (`dali_1k`, `dali_1k_local`, `dali_2k`, `dali_2k_local`) have
been converted to explicit entry lists.

---

### Important — Ship With Documentation

These issues are real and visible to users, but each can be described as a known limitation in
a 1.0 release note rather than being a hard blocker. They should become 1.1 targets.

#### ✅ Toggle state is not persisted across power cycles

**Re-analysis (2026-06-29): not a real bug in practice.** The original concern assumed
toggle state would default to "all off" and stay wrong. In reality the boot query corrects it:

- At the first `loop()` call, `start_refresh()` fires and enqueues `QUERY_ACTUAL_LEVEL` for
  every registered light entity.
- `on_level_query_reply` calls `dali_dispatch_seed_toggle()` with the queried on/off state,
  seeding the toggle map from real bus state within ~350 ms of boot.
- DALI gear restores to its programmed power-on level after a power cut, so the query sees
  the actual post-restore state. Couplers (BF6, DALI-2 push buttons) need several seconds to
  reboot themselves after a power cut, so no dispatch event can arrive before the boot query
  completes.

**One genuine edge case:** if a dispatch TOGGLE entry targets a group that has no
corresponding registered light entity, that group is not queried at boot and its toggle state
starts at "off". This is a misconfiguration issue rather than a framework bug — every
dispatched target should have a light entity.

---

#### ✅ Queue overflow is silent — commands drop without any log entry

When the scheduler queue is full (16 slots) and a new command arrives, the enqueue returns an
error that is checked in many places in the ESPHome component, but the handling is typically
just a return without any log output. From the user's perspective, commands disappear silently.
In normal single-bus operation the queue rarely fills, but under a command storm (many HA
automations firing at once, a scan in progress while normal operation continues) commands can
be lost with no indication.

**Where it is:** `esphome/components/dali/dali_component.cpp` — multiple enqueue call sites,
most with a bare early return on failure.

**Plan for 1.1:** Add `ESP_LOGW` at every enqueue failure site indicating which command was
dropped and what the queue state was. Consider adding a diagnostic counter sensor exposed to HA
so persistent queue pressure is visible on the dashboard.

---

#### ✅ Input device SET (iconfig write) paths still need hardware round-trip validation

The `iconfig` write path for configuring input device parameters (hold timer, deadtime,
hysteresis, report timer) is implemented in both the protocol stack and the ESPHome command
console. However, as of 2026-06-29, these writes have not been fully validated on hardware
with a round-trip read-back for every supported parameter. The current_status.md Active Field
Tasks section already calls this out. The risk is that a write with wrong encoding or wrong
bank/instance addressing silently does nothing, or worse, corrupts an adjacent parameter.

**Plan for 1.1:** Validate every iconfig parameter with the read → write → read-back sequence
on the Steinel HF 360 II. Document the verified-good parameter list in the command reference.
Until then, the command reference should warn users to always read back after every write.

---

#### ✅ Generated CTest files are tracked

`test/Testing/Temporary/CTestCostData.txt` and `test/Testing/Temporary/LastTest.log` are
generated build/test artifacts. They do not affect firmware behavior, but they should not be
part of a public release tree because normal test runs can churn them and make the repo look
dirty for no useful reason.

**Plan for 1.0 cleanup:** Stop tracking generated CTest temporary files and make sure
`.gitignore` excludes `test/Testing/`.

---

#### Public example YAMLs should not require private GitHub credentials

The deployment YAMLs use `username: !secret github_username` and
`password: !secret github_token` for the `external_components` git source. That is fine for a
private workflow, but public examples should not imply that a GitHub token is required to use a
public release. If these files remain as site-specific deployment configs, document them as
such and provide release instructions that use a public git tag without credentials.

---

#### ✅ Add CI for host tests

The host test suite is already the canonical portable harness and currently has 18 suites. For
a public release, a small GitHub Actions workflow that builds and runs those tests would make
release quality reproducible instead of depending on a local manual run.

**Plan for 1.0 cleanup:** Add CI for `test/build` configure/build plus `ctest`, with warnings
as errors preserved.

---

#### Avoid certification/compliance overclaims

The project implements and validates useful parts of IEC 62386/DALI-2 behavior, but it is not
a DALI Alliance certified product. Public release notes and documentation should say
"implements", "tested with", or "verified on" rather than implying certification or full
standard coverage.

---

### Gaps Compared to python-dali

python-dali is a protocol library with intentionally broad DALI coverage. This project is a
controller with a specific hardware target. Not all gaps are real gaps — some are out of scope.
The following comparison is honest about what is missing and what the user impact is.

| Feature | python-dali | dali-esp | Notes |
|---|---|---|---|
| Gear types beyond DT6/DT8 (for example DT0 fluorescent/Part 201, DT1 emergency/Part 202, DT5 incandescent/Part 205) | Yes | Not implemented | Explicitly deferred. Users with older or specialized gear outside DT6/DT8 should not expect full device-type support at 1.0. This is the most visible coverage omission and should be stated clearly in the release note. |
| DALI-2 event push (unsolicited input frames) | Yes | Implemented for 1-byte sensors | 1-byte sensors (e.g. occupancy type 3) receive immediate value pushes when a matching unsolicited 24-bit frame arrives; polling continues as a fallback. 2-byte sensors (lux, temperature) still rely on polling — unsolicited event frames carry only 8 bits of event_code, insufficient for a full 16-bit value. |
| Scene: HA state after recall | Yes (query-based) | Implemented via deferred refresh | When the coupler sends a RECALL MAX / SCENE / DIM frame, the dispatch table fires a 600 ms deferred `start_refresh()` which queries ACTUAL_LEVEL for all registered lights and updates HA state. Verified working — no code gap. |
| Unaddressed gear (broadcast excluding short addresses) | Yes | Not exposed | Not a common use case in real installations but worth noting. |
| Store scene / remove scene config | Yes | Native CLI yes; ESPHome console no | Native `config` exposes `set-scene` and `remove-scene`; the ESPHome HA command console does not currently expose scene config names. |
| Fade time / fade rate configuration | Yes | Basic DTR0 setters exposed | Native CLI and ESPHome console expose fade time/rate DTR0 setters, but there is no higher-level UX for human fade presets or validation beyond numeric ranges. |

The gear-type coverage gap beyond DT6/DT8 is the only one worth prominent mention in a release
note. All others are either unusual use cases or easily added in 1.1.

---

### Post-1.0 (Genuinely Deferrable)

These are real improvements but none block a working installation.

- **NVS toggle state persistence** — described above under Important items.
- **Broadcast scene recall validation** — the DALI spec forbids scene recall to broadcast
  address. The control layer currently does not validate this. It is a spec-compliance issue
  but not a safety issue; real buses simply ignore the frame.
- **Discovery timeout tuning** — the 25 ms response timeout in `dali_frame.h` line 21 may
  cause slower or older ballasts to be missed during bus scan. Consider increasing to 30 ms or
  making it configurable.
- **Scheduler queue depth visibility** — the scheduler does not expose a current depth query.
  This makes it impossible to know how close to saturation the queue is without tracing.
- **Gear types beyond DT6/DT8** — legacy and specialized gear such as fluorescent,
  emergency, and incandescent drivers. Real work, not a quick fix. Scope for a future release.
- **ESPHome scene config names** — native CLI already exposes `set-scene` and
  `remove-scene`; ESPHome command console can add them when needed.
- **Higher-level fade config UX** — native CLI and ESPHome command console expose DTR0
  setters, but human-friendly fade presets/readback can wait.
- **Post-power-cut recovery strategy** — decide and implement whether the ESP32 should query
  all configured lights after a bus power restoration event, and whether it should re-send any
  state. Currently undefined behavior after power cut.

---

### Explicit Release Decisions / Non-Goals

- **No project-specific ESPHome GPIO validation.** ESPHome already validates generic YAML pin
  shape, and this project documents the known-good WROVER-E wiring (`TX=GPIO18`, `RX=GPIO19`).
  Board-specific pin restrictions vary too much to encode one board's constraints into the
  reusable component.
- **Leave `_local` YAML files for now.** They are local workflow files and will be moved or
  removed separately; they are not part of the current Pre-1.0 release cleanup.

---

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
