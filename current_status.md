# DALI-ESP Current Status

**Last updated:** 2026-09-03, against `dev` `7e9ee6e`, after the 2k bus pass.

Annex to `AGENTS.md`. That file holds the architecture, the layer rules, the ISR
and timing constraints, and the native/host build commands; this one holds what
is true right now — what works, what is proven, and what is open.

Everything dated lives elsewhere: verification history, investigations, and the
accumulated unreleased-change list are in `project_log.md`. Verb and argument
detail is in `dali_commands.md`, frame and opcode detail in `dali_protocol.md`,
per-capability API/verb/vector/hardware status in `dali_capability_matrix.md`.

## Release And Deployment State

| | |
|---|---|
| Latest tag | `v1.3.0` (`ce0a72c`), which is what `main` points at |
| Last hardware-tested release | `v1.1.1`. Nothing since has been flashed to a bus as a tagged build |
| `dev` vs `main` | 25 commits ahead, 0 behind |

`dev` carries the post-scan verification, VERIFY-based duplicate detection,
mixed-device work, the backup/restore path, and control-device commissioning —
none of which any tag has. `dali-starter.yaml` and the README example both pin
`ref: v1.3.0`, so an operator who wants that work must move the pin
deliberately.

**What is deployed is not recorded here, and this is not the place to look.**
The authoritative site configurations live in Home Assistant. The `_local/`
copies are snapshots taken when convenient, are not kept in step, and are
therefore not evidence of what any device runs or of which ref it was built
from.

**Overall:** the ESPHome controller is operational on the two known
installations. The repository has a strong reusable C foundation, but CLI
completeness, several DALI-2 protocol paths, and general-purpose ESPHome
reliability remain active work. This is not a conformance claim, and the project
is not DALI Alliance certified.

## Goal Assessment

| Aim | Current state |
|---|---|
| CLI completeness | **Surface complete, verification incomplete.** Every shared capability has a typed verb, including memory, DT6, DT8, input-device query/configuration, vendor helpers, control-device memory, gear *and* control-device commissioning, backup/restore, CONTINUOUS UP/DOWN, arc power MASK, and send-twice `raw2`. What is missing is real-bus results for DT6, DT8, memory writes, and input-device configuration. |
| ESP32 / ESPHome controller | **Working installation-grade baseline.** The two known sites have operational brightness control, observation, sensor polling, diagnostics, and discovery. Not yet a general, fully state-correct DALI controller. |
| Protocol separation | **Directionally strong.** The reusable C stack is independent of ESPHome, and every frame the ESPHome layer sends is built by a shared builder in `components/dali`. What stays ESPHome-bound is the wiring — console dispatch, the refresh pump, the entity registries — none of which host tests can reach. |
| Standards confidence | **Selected workflows verified, not complete conformance.** Some tests repeat implementation constants rather than independent standard-derived vectors. |

The next development phase should prioritize protocol and state correctness
before adding broad new device support.

## Verification State

**Last full local pass — 2026-09-03, `dev` `a8c9372` plus the `backup import`
and `restore groups` work, clean tree:**

- 31/31 host suites build and pass (`mingw32-make --directory build test`).
- `dali_test.yaml` passes `esphome config` on ESPHome 2026.8.1. Because that
  config is `type: local`, this validates the Python schema in
  `esphome/components/dali/__init__.py` against the working tree.
- `_local/dali-diag-local.yaml` **compiles** on ESPHome 2026.8.1 — the first
  time the C++ layer and the vendored C stack have been built under the same
  version that validates the schema. The new shell strings are present in
  `firmware.elf`, so this was a real rebuild and not a stale incremental.
- Scope, stated precisely: `esphome config` exercises the Python schema only.
  The compile above is what covers the C++ layer and the vendored C.
- **Not a hardware pass.** No COM6, no flash, no bus.

### Recorded hardware state

**2026-09-03, 2k bus, a `dev` build carrying the reworked backup/restore.** The
first bus result for the commissioning and backup/restore work accumulated
since `v1.1.1`. `backup save`/`status`, `restore plan`/`apply`, `address set`/
`add`/`remove`, `commission unaddressed` with its post-scan, `quiescent`,
`identify`, `meminfo` and `config-dtr0` all produced correct results on real
gear. Most importantly, the `contested` classification met a **genuine physical
two-unit collision** when an unpowered driver holding a4 regained power — the
first real collision behind `RX_ACTIVITY`, which every commissioning safety
claim depends on. Full scope, and what the session did *not* cover, is in
`project_log.md`.

Established on or before the `v1.1.1` flash of 2026-08-14:

- Hardware target: ESP32-DevKitC-VE / ESP32-WROVER-E with MikroE DALI-2 Click.
- Known wiring: TX on GPIO18, RX on GPIO19. GPIO16/17 are PSRAM and unusable.
- Bidirectional DALI traffic works on the two installed buses.
- Existing DALI-1 pushbutton couplers in BF6/direct-control mode operate the
  lamps directly; the ESP32 observes their traffic for Home Assistant state.
- Native and ESPHome diagnostics have been used successfully for control,
  queries, discovery, inventory, capture, and sensor investigation.

### Not yet verified

- **Most of `dev` since `v1.1.1`.** The 2026-09-03 2k pass cleared gear
  commissioning, the backup/restore core, and the contested classification; the
  rest of the protocol work of the last three weeks still has host vectors and
  no bus behind it.
- Hardware round-trip for input-device configuration writes, DT6/DT8 helpers,
  memory operations, and vendor helpers. No write path reads its value back.
- **Equal-random-address handling** — the largest untested slice. The two units
  commissioned on 2026-09-03 drew distinct randoms, so the path never ran.
- Control-device commissioning (`commission devices`) on a real bus.
- `backup import`, `backup export`, and `restore groups` against real gear.
  `backup save`/`status` and `restore plan`/`apply` are now covered.
- The commissioning post-scan audit's own contested path. The scan path met a
  real collision on 2026-09-03; the commission run that followed was clean, so
  the audit's classification is still an inference.
- `manifest.json`'s `>=2026.6.0` ESPHome floor, which is tested against nothing
  but whatever `pip install esphome` last resolved to. The 2026.8.1 end is now
  covered at both stages — schema validation and a full compile of the C++ layer
  and vendored C — but nothing establishes the floor itself.
- Independent conformance of every protocol constant, timing boundary, memory
  layout, discovery path, and commissioning collision case.

Dated evidence for each of these is in `project_log.md`.

## Current Software State

### Shared DALI stack

- `components/dali` is the reusable plain-C PHY, RX ring buffer, scheduler,
  transport, protocol/control APIs, discovery, gear and control-device
  commissioning, snapshot/restore, group mapping, memory helpers, DT6/DT8
  builders, input-device support, events, mapping, dispatch, and vendor helpers.
- `dali_transport` is the single bus abstraction the higher modules share. Only
  the whole-sequence entry point is atomic, and it is optional, so callers that
  require atomicity must ask via `dali_transport_supports_atomic_sequence()`.
- Static allocation and the buffer-first ISR model are preserved throughout the
  timing and protocol layers. `dali_phy_init()` checks every acquisition and
  releases what earlier steps took before returning an error. Core affinity is
  not hardcoded.
- Scheduler sequences run contiguously and report a `DaliSequenceResult` holding
  the overall error, the failing step, the steps attempted, and one backward
  frame per reply-bearing step. **Every multi-frame workflow in the stack is
  built on it** — memory reads, the control-device memory write, discovery's
  ENABLE DEVICE TYPE pairs, group query, commissioning's search/COMPARE probe
  and PROGRAM/VERIFY pair, DT6/DT8 command grouping, and Part 103 DTR loads.
  Only genuine single-frame queries go through `transact` directly.
- Reply attribution uses two window edges. `DALI_REPLY_WINDOW_OPEN_US` (5500 us,
  the standard's minimum) applies to undecodable activity;
  `DALI_REPLY_WINDOW_OPEN_DECODED_US`, derived from `DALI_SETTLE_MS` rather than
  chosen, applies to an observation that decoded as a complete backward frame.
  The asymmetry is the safety: qualified undecodable activity must not be read
  as a reply, because COMPARE maps it to YES and invents gear that is not there.
- Part 103 events decode into canonical source fields, reject command and
  reserved frames, and preserve all ten event-information bits. Independent
  vectors cover all five normal source schemes.
- Part 103 generic instance configuration plus Part 301/type 1, Part 303/type 3,
  and Part 304/type 4 builders have an independently audited opcode surface.
  Software-level evidence only.
- The Part 102 memory helper reads the common Bank 0 identity block. Discovery
  performs this 16-bit read only for confirmed control gear; pure Part 103
  devices need a future typed 24-bit memory path.
- `dali_snapshot` records which *physical* unit (by Bank 0 identification
  number) holds which short address; `dali_restore` turns a snapshot plus a live
  bus into an ordered move list of plain addressed SET SHORT ADDRESS traffic. No
  INITIALISE window, interruptible at any point, and convergent on re-run. The
  blob round-trips both ways as of 2026-09-03: `backup export` prints the
  `backup import` script that reproduces it, so a backup kept off the device can
  be loaded back — which is what the native CLI, having no persistent store,
  needs. `dali_snapshot_decode()` validates a blob in full before writing, so a
  rejected import costs the held backup nothing.
- `dali_restore_plan_groups` is the second, separate planner: it diffs each
  gear's recorded group mask against the one on the bus and emits the ADD/REMOVE
  bits, matched by identification number and addressed to wherever the gear
  answers now. It is deliberately not part of `restore apply` — group membership
  survives a re-address untouched, so this repairs a `RESET` rather than a
  commissioning walk, and it is the one part of a restore that can destroy
  something a restore cannot give back. Both "the backup never read this gear's
  groups" and "this gear's groups will not read back now" are reported and
  skipped rather than written blind.
- `dali_device_commissioning` is the Part 103 counterpart of
  `dali_commissioning` — same walk shape over a different command space, sharing
  the reply classification rather than the encodings. Both directions of the
  cross-part guard exist (`terminate_control_devices` and
  `terminate_control_gear`).
- DT1 and other specialized/legacy device types remain intentionally
  unimplemented.

### Diagnostic shell

- `components/dali/dali_shell.c` is the shell: one session owning every verb,
  the blocking transport, and the caches a workflow accumulates. It is the
  single implementation both front ends run.
- `components/dali/dali_cli.c` is the portable core beneath it — tokenizing, the
  verb tables, argument-count validation, and reply formatting — with no
  ESP-IDF, FreeRTOS, or bus dependency, so the host tests build it.
- `main/dali_diag.c` is the UART0 binding and nothing else;
  `esphome/components/dali/dali_shell_tcp.cpp` is the TCP binding. Both move
  bytes and own a session's lifetime. Neither implements, gates, or rewords a
  verb; a surface that must refuse one declares a `DALI_SHELL_ALLOW_*` policy.
- Dispatch is one table. `dali_cli_resolve()` tokenizes, looks the verb up, and
  checks argument-count bounds before any handler runs, so trailing tokens are
  rejected rather than ignored. Help and `list <table>` are generated from the
  same tables, so neither can describe a command the parser will not accept.
- `raw` sends one arbitrary 16- or 24-bit frame and `raw2` sends one twice
  through the send-twice path. Both are diagnostic escape hatches, not
  substitutes for the typed atomic verbs.
- `identify`, `smoke`, `capture`, and the inventory JSON export have no host
  vectors of their own; they are composed from covered primitives, but their
  output formats are unasserted.

### ESPHome component

- Active component: `esphome/components/dali`. `esphome/dali_esphome.h` is an
  unused legacy placeholder.
- `dali-starter.yaml` provides discovery and diagnostics: scan, identify,
  find-couplers, target controls, bus monitoring, group-map output, and
  generated YAML log lines.
- Light entities expose brightness only. Shared DT8 support is not yet mapped to
  Home Assistant colour-temperature, XY, or RGB controls — deliberately held
  behind hardware verification.
- Brightness maps through the gear's own window and curve, not `[1, 254]`. MIN
  LEVEL, MAX LEVEL, and (for DT6) dimming curve are queried per short address at
  boot, on every refresh, after a scan, and after any command that moves them. A
  group or broadcast entity uses the union of its known members' windows; YAML
  `min_level`/`max_level`/`dimming_curve` override what was read. A reduced
  ceiling therefore reports 100 % at MAX LEVEL.
- The console covers control-gear output and configuration, control-gear and
  control-device memory, DTR loads, non-commissioning special commands, DT6,
  Part 103 instance query and configuration, vendor helpers, quiescent mode,
  `raw`/`raw2`, and the local `queue`/`group` verbs. It is the shell's verb set
  minus what needs a terminal or a blocking transport — so no `backup`,
  `restore`, or `commission`. `dali_capability_matrix.md` lists the exclusions
  with reasons.
- Console `OK` means **queued**, not executed or device-acknowledged. Async
  commands publish `pending`, replace it on completion, and ignore callbacks
  belonging to an older submitted command. Replies are decoded by the same
  shared function the shell prints through.
- Light state crossing from the DALI task to ESPHome is one coherent packed
  update; multiple pending observations coalesce to the latest.
  `DaliInputSensor` uses the same packed mailbox, so neither surface has a
  lost-update window.
- Writes the operator did not issue are identified by what they are, not when
  they arrive: ESPHome's restore/default write is the first `write_state()`
  after `setup()`, and a bus reading pushed back is matched on the exact
  `(is_on, level)` pair `apply_bus_state()` sent. There is no startup time
  window, so a command issued seconds after boot reaches the bus.
- A light entity suppresses a redundant command only against state a scheduler
  *completion* confirmed, never against a successful enqueue. A rejected enqueue
  retains the desired state and retries; a failed transmission invalidates the
  cache and re-arms one bounded retry. One command per light is in flight at a
  time. Confirmation still means transmitted, not device-acknowledged.
- Every component-owned producer observes the scan gate — refresh and due sensor
  polls stay pending, identify pauses, console and diagnostic actions are
  rejected, HA light writes retain their target and retry. The exception is the
  local-only `queue` verb. Headless events are drained so their fixed queue
  cannot overflow, but actions observed during a scan are dropped rather than
  replayed against a stale physical context.
- Queue admission is observable: `dali_sched_queue_stats()` reports depth,
  capacity, high-water, admitted, and rejections split into queue-full and
  reset-barrier. A rejection is dropped work — the scheduler never retries a
  refused submission.
- Only complete group discovery replaces and persists membership. A failed
  optional query or a missed known member retains the prior map and withholds
  generated YAML. `group forget <addr> [group]` retires a departed member
  without touching the bus.
- Sensor readings are one scheduler sequence, so a two-byte instance cannot have
  its latching query and latch read separated by other traffic. Matching
  Device/Instance events request an immediate authoritative poll; event
  information is never published as a sensor value.
- `bus_fault` separates current availability from cumulative history. The
  `tx_frames_ok` PHY counter is the recovery signal: it publishes
  `Bus stuck (N total)` and returns to `OK (N past faults)` once a frame clocks
  out in full.
- The GPIO schema is board-aware, rejects TX equal to RX, and rejects GPIO16/17.
  DALI-task creation is checked and fails the component rather than running
  without a bus task.
- The TCP shell exposes the guarded `commission` verb only when
  `allow_commissioning: true`. The Home Assistant text surface blocks raw
  commissioning primitives and has no dedicated workflow entity.

## Installation State

**The live site configurations are in Home Assistant, not here.** Nothing in
this repository establishes what a site runs or which ref it was built from. To
learn what a device actually runs, ask Home Assistant or the device.

| Configuration | Component source | Tracked | Role |
|---|---|---|---|
| `dali-starter.yaml` | `ref: v1.3.0` | yes | Starter/commissioning firmware; node `dali-starter` |
| `dali_test.yaml` | `type: local` | yes | CI coverage config; fictitious layout, never flashed |
| `_local/dali-1k.yaml` | `ref: v1.1.1` | no | Stale snapshot, 2026-08-14; 16 control gear, groups 0/2–7 |
| `_local/dali-2k.yaml` | `ref: dev` | no | Stale snapshot, 2026-08-14; group 0 lighting, HA console, Steinel HF 360 II polling |
| `_local/dali-diag-local.yaml` | `type: local` | no | Compile-test copy of the diagnostic firmware |

`_local/` also holds underscore-named leftovers (`dali_1k.yaml`,
`dali_2k.yaml`) that predate the hyphenated pair and are not current. The whole
directory is gitignored, so normal `git status` does not show changes under it —
back up any real site files separately.

`_local/secrets.yaml` holds **live credentials**, not dummy values: its six
shared keys are byte-identical to the root `secrets.yaml`, and the 2k device
joins WiFi and accepts OTA using them. Both files are gitignored and untracked;
treat both as real secrets.

The former tracked site copies are gone. Tracking a real deployment to obtain CI
coverage was the wrong trade — they carried an address layout nobody else could
use and needed editing whenever the site changed. `dali_test.yaml` replaces them
and keeps the property that mattered: `type: local` with
`path: esphome/components`, so a configuration and the component it configures
always agree within a commit.

### 1k site: gear that answers just before the attribution window opens

Four of sixteen fixtures settle faster than IEC 62386-101's 5.5 ms minimum
(measured 4.12–5.85 ms) and were being read as silent. Fixed 2026-08-25 by the
decoded-frame window edge; three confirmed on hardware the same day, and the
fourth (a0, group 5) resolved by capture. Group membership now reads from the
bus with no `query_address` anywhere in the YAML and is persisted to flash. The
full investigation — captures, timings, and why a hand-picked margin does not
survive this bus — is in `project_log.md`.

### 2k site: the scan's unattributed-RX note counts occupancy events

Every `discover` on this bus reports `N RX observation(s) fell outside active
reply attribution`, N running 11-39. There is no timing fault. The Steinel at a0
emits on four instances continuously, a 64-address walk is back-to-back TX, and
events arriving outside `SCHED_IDLE`/`SCHED_WAIT_REPLY` are counted as ignored.
Confirmed twice on 2026-09-03: the one scan run under `quiescent on all`
printed no note at all, and HA's recorder shows the zone-2 occupancy sensor
changing state 31 times in the same 18 minutes, tracking the operator standing
in the corridor. The counter has since been split and the scan now reports
control-device events on their own line, in words that do not call them a
fault. What is still owed is a bus that shows the new numbers. Investigation in
`project_log.md`.

### Observed Steinel instance layout

| Instance | Type | Meaning | Authoritative handling |
|---:|---:|---|---|
| 0 | 4 | Light/lux | Two-byte poll, `scale: 0.01` |
| 1 | 3 | Occupancy | One-byte poll and HA mapping |
| 2 | 0 | Temperature | Two-byte poll, `T_C = raw * 0.1 - 5` |
| 3 | 0 | Humidity | One-byte poll, `H_percent = raw * 0.5` |

Polling is authoritative for occupancy. A matching Device/Instance event
requests an immediate poll, but the event information itself is never treated as
a sensor value.

## Prioritized Work

### P0 — Protocol correctness and conformance

- **Hardware validation of the rest of the commissioning work.** The 2026-09-03
  2k pass cleared gear commissioning, `backup save`/`restore apply`, and — with
  a real two-unit collision — the `RX_ACTIVITY` classification the whole safety
  argument rests on. Still unmet by a bus: equal-random-address handling
  (the two units drew distinct randoms, so the path never ran), control-device
  commissioning, `backup import`/`export`, and `restore groups`. The
  single-unaddressed-device envelope no longer stands on nothing, but the
  equal-random path is where it is still an assumption.
- **Prove bus timing beyond the local own-forward-frame guard.** TX-end and
  observation timestamps are exported and host-tested; HIL must validate both
  attribution edges (5.5 ms undecodable, 2 ms decoded) against the 27 ms close,
  physical collision behavior, and external-frame cases. DALI-2 priority/backoff
  remains open, as does a deadline-aware PHY call when a repeat would cross the
  100 ms limit.
- **Cite `DALI_REPLY_WINDOW_OPEN_US` from the standard text.** It was lowered
  from 7,000 to 5,500 us on 2026-08-25 because 7 ms is the *nominal* settling
  time and attributing from it would time out compliant fast gear. The 5.5 ms
  figure is still a recollection of the IEC 62386-101 range rather than a
  reading of the clause, so the citation is owed even though the direction of
  the change is the safe one. Guard against the regression this closed: too high
  a value presents as gear that used to answer going quiet.
  `DALI_REPLY_WINDOW_OPEN_DECODED_US` needs no citation — it is derived.
- **Confirm the post-RANDOMISE settle time against the standard text.**
  `DALI_COMMISSIONING_RANDOMISE_SETTLE_MS` was raised from 15 to 100 ms on the
  strength of Espressif's `esp_dali` citing IEC 62386-102 §11.3; the clause has
  not been read here. If 15 ms was genuinely too short, gear could have been
  unsearchable at the first COMPARE — which presents exactly like the former
  silence/collision inversion, so an apparent improvement here would look like a
  partial fix for that and would not be one.
- **`restore` cannot stage a unit the backup has never seen, and so fails to
  converge after a mixed commissioning accident.** A unit absent from the
  snapshot becomes `UNKNOWN_UNIT`, which marks its address immovable, which
  drops any recorded unit aimed at that address as `TARGET_OCCUPIED`. On
  2026-09-03 this produced a zero-move plan while a free address sat unused and
  the operator hand-executed the three-command staging cycle `dali_restore.c`
  already knows how to compute. Proposed: stage the unknown unit through a free
  address rather than dropping the move, keeping today's refusal when the space
  is full and for `UNIDENTIFIED` units. Reasoning in `project_log.md`.
- **The post-scan verification has only a partial bus result.** Both walks now check
  themselves on every exit that could have written an address — including the
  failure paths, which are the only ones that can leave a short address written
  but unrecorded — and the diff behind it (`dali_commissioning_audit`) is host-
  covered. `RX_ACTIVITY` itself now has a real collision behind it, via the scan
  path on 2026-09-03. What the audit's own contested path still lacks is a bus:
  the commission run that session was clean, so its classification remains an
  inference.

### P0 — Transaction and runtime reliability

- If new scheduler clients are added outside `DaliComponent`, add a true
  scheduler admission reservation for scans. The current quiescence check plus
  component-wide gate covers present local producers but is not an atomic
  check-and-reserve primitive.
- Give the remaining fire-and-forget scheduler clients the confirmed-transmission
  treatment the light entities have. Console `OK`, the refresh pump, and
  headless dispatch still report or act on admission rather than transmission.

### P1 — Verification on real hardware

The typed verb surface is in place; what is missing is evidence. Keep
`dali_capability_matrix.md` current as each row is exercised.

- Run the DT6 and DT8 verbs against real gear. Every DT8 row is host-covered and
  hardware-unverified, and both the P2 colour mapping and DT8's absence from the
  ESPHome console are held behind this. The DT6 console verbs send the same
  frames as the native ones, so one session clears both columns — start with
  `dt6 <a> dimming-curve` and `dt6 <a> failure-status` on the 1k LED drivers,
  where a wrong answer is visible rather than destructive.
- Complete the memory read/write/read-back cycle for both `memread`/`meminfo`
  (Part 102) and `devmem` (Part 103). No write path reads its value back today,
  so `devmem write` reports transmitted, not applied.
- Validate input-device configuration writes with read/write/read-back per
  parameter. `iconfig` success means transmitted; until this is done the whole
  surface stays experimental.
- Add host vectors for `identify`, `smoke`, `capture`, and the inventory JSON
  export, whose output formats are unasserted.
- Nothing in the shared-`dali_cli` migration, or the verb-parity work built on
  it, has been exercised on a real bus.

### P1 — ESPHome correctness and architecture

- **No addressing fault reaches Home Assistant.** `bus_fault` is a PHY liveness
  signal (`"OK"` / `"Bus stuck: N"`) and reports correctly, but nothing surfaces
  a contested short address — the one failure the bus cannot undo remotely. On
  2026-09-03 the sensor read `OK` throughout a real collision, an address wipe,
  and a commissioning walk. The scan already computes `undecodable_count` and
  the per-address flags. Wanted: an addressing-health sensor, or a widened
  `bus_fault` reporting `"Contested: a4"`. The same shape one layer along, group
  membership changes do not reach the integration either, so a group light
  entity cannot know its membership changed underneath it.
- Extend the compact Part 103 dispatch key if a site needs to distinguish
  Device-Group from Instance-Group sources or match instance type. The canonical
  event/capture path retains these fields; the five-field rule key does not. No
  site needs this today, and it changes a C struct layout.
- A paged or exportable Find Couplers result, rather than one
  truncated-with-a-count summary. The log already holds every frame.
- Define recovery after bus-only power cycles, beyond the current/cumulative
  fault split.
- The console's own dispatch has no host vectors. Its parsing, validation, and
  reply decoding are shared code that does, but the wiring in
  `dali_component.cpp` is ESPHome/FreeRTOS-bound and testable only on device.

### P1 — Diagnostic shell correctness

- **Confirm the split ignored-RX counters on the 2k bus.**
  `rx_ignored_outside_reply` carried four unrelated facts and is now the sum of
  seven named buckets: `rx_reply_early`, `rx_reply_late`, `rx_reply_superseded`,
  `rx_event_unroutable`, `rx_event_no_subscriber`, `rx_undecodable_ignored` and
  `rx_ignored_unclassified`. `status` prints the breakdown and the scan note
  names each class in its own words, calling only early/late a timing signal.
  Host-covered and mutation-checked; no bus has seen it. The experiment it was
  built for: on 2k, `rx_event_unroutable` should account for almost all of the
  11-39 the old note reported while early and late stay near zero — which is
  what settles the 2026-08-13 reading that is currently in doubt.
  **`b64f81f` is the control arm**: it carries the split counters and not the
  scan's quiescence bracket, so it is the last build that can still observe the
  event traffic the bracket removes. Run the experiment on that ref before
  flashing anything later.
- **Confirm the scan's quiescence bracket on a bus.** Every operator-driven
  walk in the shell — `scan`/`discover`, both commissioning pre-scans, the
  commissioning post-scan, `backup save` and the `restore` refresh — now
  broadcasts `START QUIESCENT MODE`, settles, walks, and releases on every exit
  path including the cancelled and errored ones. `find switches` does not scan
  and is untouched, and the integration's own periodic scan opts out: an
  unattended walk that silences occupancy for minutes is a trade nobody is
  present to accept. Host-covered and mutation-checked; no bus has seen it.
  What to watch for on hardware is the failure direction — a release that does
  not land leaves the installation's sensors quiet, and the shell's line saying
  so is the only thing that would tell an operator to run `quiescent off all`.
- **Add `address <aN> clear`.** Clearing a short address is a normal step in
  resolving a collision and is the one address operation with no typed verb. The
  fallback, `config-dtr0 <aN> set-short-address-dtr0 255`, is correct DALI but
  prints a bare `OK` with no read-back and no warning when the subject is
  already known to be contested. The verb should refuse or warn on a contested
  subject, send the sequence atomically as `address set` does, and verify the
  address went silent.

### P1 — Release and verification quality

- **Write the release notes.** The accumulated API migrations and
  operator-visible breaks — console verb renames with no aliases, the reply
  format change, error names replacing numbers, `special randomize` →
  `randomise`, and `backup export`'s single hex line becoming an import script —
  are collected in `project_log.md` under *Unreleased API and operator-visible
  changes*. Anything in Home Assistant that writes command strings to the
  `text:` entity needs updating.
- Enforce or document the actual ESP-IDF and ESPHome version requirements.
  `idf-build.yml` pins IDF 6.0.1, so the native requirement now fails when it
  stops holding. The ESPHome side is still advisory — CI installs whatever
  `pip install esphome` resolves to. Note that the two builds compile the same
  `components/dali` C with different toolchains.
- Keep local filenames hyphenated in all documentation. `dali_commands.md`
  tracks both verb surfaces and needs re-checking whenever either table changes.
- Complete release provenance: project SPDX identifiers and the full vendored
  Unity MIT license/third-party notice.
- Clarify the bus topology: direct-control couplers also transmit. Their
  coexistence is field-tested, but that is not collision-safe single-master
  arbitration.

### P2 — Further development

- Map DT8 to Home Assistant colour-temperature, XY, and RGB/RGBWAF traits after
  hardware validation.
- Add typed Part 303/type 3 occupancy and Part 304/type 4 illuminance profiles.
- Add a dedicated Home Assistant commissioning workflow/UI only after the shared
  path is hardware-proven. The TCP shell already exposes the guarded verb behind
  `allow_commissioning: true`.
- Improve scene/fade UX. A level-changing command arms a deferred re-read, but
  the refresh it triggers is a full pass over every light rather than a query of
  the one that moved, and nothing reads a final value after a transition longer
  than the arming window.
- Add capture replay, parser fuzzing, and hardware-in-loop tests for timing,
  collisions, queue pressure, bus faults, and power restoration.
- Add DT1 and other device types only when an installation requires them,
  following the DT6/DT8 module pattern.

## Operational Constraints

- Do not imply DALI Alliance certification or complete IEC 62386 coverage.
- The controller has no proven collision-detection/arbitration strategy.
  Existing direct-control couplers work on the installed buses, but simultaneous
  transmissions remain a risk.
- Treat input-device configuration writes as experimental until real-bus
  read/write/read-back validation is complete.
- Build every new multi-frame workflow on `DaliSequence`. The migration covers
  everything that exists today, so what remains is regression risk: issuing
  dependent frames as separate transactions reintroduces the class of bug this
  removed. The tell is a query whose answer depends on a register or enumeration
  pointer that the same query, or an immediately preceding frame, modifies.
- Do not use GPIO16 or GPIO17 on the WROVER-E target.

## Source Layout

| Path | Role |
|---|---|
| `components/dali/` | Reusable C protocol, PHY, scheduler, transport, discovery, commissioning, memory, device types, dispatch |
| `components/dali/dali_shell.c/.h` | The diagnostic CLI as a reusable session: every verb, the blocking transport, workflow caches |
| `components/dali/dali_cli.c/.h` | Portable CLI core: tokenizing, verb tables, validation, formatting. Shared by both front ends |
| `components/dali/dali_commissioning.*` | Part 102 control-gear commissioning walk |
| `components/dali/dali_device_commissioning.*` | Part 103 control-device commissioning walk |
| `components/dali/dali_snapshot.*` | Records which physical unit (Bank 0 id) holds which short address |
| `components/dali/dali_restore.*` | Turns a snapshot plus a live bus into an ordered move list |
| `components/dali/dali_group_map.*` | Group→member bookkeeping; picks a group light's poll representative |
| `components/dali/dali_light_write.h` | Header-only desired/in-flight/confirmed write arbitration |
| `components/dali/dali_refresh_cursor.h` | Header-only refresh-pump cursor |
| `components/dali/dali_dim_curve.*` | IEC 62386-102 arc power level ↔ light output conversion |
| `components/dali/dali_lunatone.*`, `dali_steinel.*` | Vendor-specific helpers |
| `main/main.c` | Native ESP-IDF diagnostic application entry point |
| `main/dali_diag.c/.h` | UART0 binding for the shell; moves bytes only |
| `esphome/components/dali/` | Active ESPHome external component |
| `esphome/components/dali/dali_shell_tcp.cpp` | TCP binding for the shell |
| `esphome/components/dali/proto_dali_*.c` | Shims pulling the vendored C in behind ESPHome's source glob |
| `esphome/dali_esphome.h` | Unused legacy placeholder |
| `dali-starter.yaml` | Tracked starter/commissioning firmware |
| `dali_test.yaml` | Tracked CI coverage config; the widest configuration this repo compiles against its own tree |
| `_local/` | Gitignored site snapshots, diagnostic compile-test config, and live secrets |
| `test/` | 29 host suites and the vendored Unity runner |
| `tools/dali-shell` | Operator-side script for the TCP shell |
| `AGENTS.md` | Architecture, layer rules, timing/ISR constraints, build commands |
| `project_log.md` | Verification history, investigations, unreleased-change list |
| `dali_commands.md` | Every verb and named command table, both surfaces |
| `dali_protocol.md` | Frame layouts, opcode tables by IEC part, event decoding |
| `commissioning_readme.md` | Commissioning workflow: flash, walk the bus, export a config |
| `dali_capability_matrix.md` | Per-capability API/verb/vector/hardware/ESPHome status |
| `steinel_bank2_reference.md` | Installation-specific Steinel memory observations |

## ESPHome Build And CI

`AGENTS.md` carries the native ESP-IDF and host-test commands. The ESPHome side:

```powershell
python -m esphome config  dali_test.yaml      # cheapest check; validates the in-repo component
python -m esphome compile dali_test.yaml      # the working tree, every platform
python -m esphome compile dali-starter.yaml   # whatever ref it pins, not the working tree
```

`esphome config` is what to reach for first: it validates the schema without
touching a build directory, so it cannot collide with a compile running in
another terminal. It does not run `to_code()`, so a codegen error still needs a
compile to surface.

To prove uncommitted component changes actually build, compile `dali_test.yaml`
— it resolves `type: local` against `esphome/components`, so it compiles the
working tree by construction, and it declares every platform and every optional
block on purpose. **An option added to the schema without being added to
`dali_test.yaml` is an option nothing compiles.** Extend that file in the same
change.

Four workflows in `.github/workflows/`:

| Workflow | Covers | Trigger |
|---|---|---|
| `host-tests.yml` | 29 host suites, cmake/ctest over `test/` | push main/dev, PR to main |
| `idf-build.yml` | Native firmware, `idf.py build` for esp32 on IDF 6.0.1 | push main/dev, PR to main |
| `esphome-build.yml` | Source/shim/`SRCS` agreement, config discovery, per-config schema validation, and compile of every `type: local` config | push main/dev, PR to main |
| `release-packaging.yml` | `esphome compile` from a git tag in an empty directory | tag `v*`, manual |

Notes an operator needs:

- `esphome-build.yml` names no configuration: `discover` runs
  `git ls-files 'dali*.yaml'` and sorts by whether `external_components` says
  `type: local`. Those build the tree under test and are compiled; a config
  pinning `type: git` at a ref is validated only, because ESPHome would fetch
  and compile that ref instead of the branch, so a 20-minute build would report
  on code the pull request never touched.
- A failure on a pinned config does not mean the branch is broken. It means the
  tracked config has drifted out of schema with the ref it names, and either the
  pin or the config needs updating before release.
- If no tracked config uses `type: local`, `compile` is skipped and the ESPHome
  C++ layer gets no coverage at all. `discover` emits a warning, but a run can
  still go green — **do not delete `dali_test.yaml`** without replacing what it
  covers.
- `secrets.yaml` is gitignored, so every ESPHome job writes its own dummy one at
  the values the tracked configs reference. `dali_test.yaml` needs none of them:
  its credentials are inline dummies, so it validates in a bare checkout,
  including a fork's first CI run.
- `release-packaging.yml` deliberately never runs `actions/checkout` and caches
  nothing. Run it inside a repo checkout and it passes for the wrong reason.
  `workflow_dispatch` takes a ref, so a branch can be packaging-tested before it
  is tagged.

## Documentation Policy

- Keep this file limited to current state, constraints, and open work. If an
  entry has a date on it, it belongs in `project_log.md`.
- Record completed work in Git history or `project_log.md`; remove it from the
  backlog rather than annotating it as done.
- Keep verb and argument detail in `dali_commands.md`, frame and opcode detail
  in `dali_protocol.md`, and per-capability status in
  `dali_capability_matrix.md`.
- Label every claim as host-tested, hardware-verified, or unverified.
- Do not add session-log TODO files; merge active work into the prioritized list
  above.
