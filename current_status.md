# DALI-ESP Current Status

**Last updated:** 2026-08-11

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

### Verified locally on 2026-08-11

- The DT8 16-bit colour value read is now one four-step sequence built by
  `dali_dt8_build_colour_value_sequence()`: DTR0 = selector, ENABLE DEVICE
  TYPE 8, QUERY COLOUR VALUE for the MSB, then QUERY CONTENT DTR0 for the LSB
  the gear left behind. This was the last workflow where an interleaved frame
  produced a wrong answer rather than an error: any DTR0 write landing between
  the two reads replaced the low byte, and the caller received a plausible but
  incorrect 16-bit value.
- The same change removes a third retry hazard of the READ MEMORY LOCATION
  kind, and the most damaging one found so far. QUERY COLOUR VALUE previously
  retried once, but it is answered under the preceding ENABLE DEVICE TYPE and
  it overwrites the selector in DTR0 with the result's low byte. A retransmitted
  step would therefore have been read as a request for whatever selector that
  byte happened to name, returning a different colour attribute under the
  caller's original label. It now carries no retry budget. QUERY CONTENT DTR0
  changes nothing and keeps its budget.
- `DaliDt8Transport` and `DaliDt8TransactionFn` are now aliases of the shared
  `DaliTransport` and `DaliTransactionFn`, so every module in the stack takes
  the same transport value.
- Commissioning's order-dependent groups now run as sequences too.
  `dali_commissioning_build_search_sequence()` carries the SEARCH ADDRH/M/L
  triple, `dali_commissioning_build_search_compare_sequence()` extends it with
  the COMPARE the triple exists to answer, and
  `dali_commissioning_build_program_verify_sequence()` pairs PROGRAM SHORT
  ADDRESS with its VERIFY read-back. The binary search and the assignment loop
  use the combined forms, so no frame can land between a search-address write
  and the COMPARE that interprets it, or between an address write and its
  confirmation.
- The sequence readers keep the standard "silence means no" rule but scope it to
  the query step. A reply-window timeout on COMPARE or VERIFY is still reported
  as a negative answer with `DALI_OK`; a failure on any earlier step is returned
  as the error it was. Previously a failed SEARCH ADDR write could not be told
  apart from a genuine NO, which would walk the binary search past a real
  device. Independent vectors cover both readings.
- This does not address the COMPARE collision inversion recorded below. Several
  devices answering at once still produce an undecodable reply that the PHY
  drops, which reads as NO. Atomicity keeps other traffic out of the group; it
  does not make a collided reply readable, and commissioning remains dependable
  only with a single unaddressed device on the bus.
- Discovery's order-dependent query groups now run as sequences instead of
  independent transactions. `dali_discovery_build_device_type_query_sequence()`
  pairs ENABLE DEVICE TYPE with the query it enables,
  `dali_discovery_build_groups_sequence()` carries both group queries, and
  `dali_discovery_build_device_types_sequence()` holds the whole multi-type
  enumeration. With an atomic transport nothing can be interleaved, so the DT6
  and DT8 enrichment bytes recorded during a busy scan can no longer be read
  under a device type that an intervening frame already cleared.
- The same change removes two retry hazards of the READ MEMORY LOCATION kind.
  The DT query step carries no retry budget, because ENABLE DEVICE TYPE is
  consumed by the command that follows it and a lone retransmission would be
  answered under the device's default type. No step of the enumeration retries
  either, because QUERY NEXT DEVICE TYPE advances the device's own list and a
  repeated step would skip a type. Both previously retried once.
- The enumeration is a fixed six-step block: QUERY DEVICE TYPE followed by five
  QUERY NEXT DEVICE TYPE steps, one more than `DALI_DISCOVERY_MAX_DEVICE_TYPES`
  so an over-long list is still reported as truncated. It re-issues QUERY DEVICE
  TYPE as its first step so the answer sequence restarts inside the atomic
  block. The cost is paid only by gear that reports multiple types: seven frames
  instead of four for a two-type device, since every fixed step transmits
  regardless of where the list ended. Single-type gear is unaffected and keeps
  its retry budget on the standalone query.
- Replies gathered before a failing enumeration step are still stored, so a
  sequence that aborts part-way contributes the types it had.
- Host vectors cover the three sequence layouts against standard-derived frames
  and argument boundaries, the reply readers, low/high group assembly, partial
  and failed results, the ascending-list and sentinel termination rules, and
  truncation at capacity. All 45 discovery cases pass.
- Commissioning vectors cover the three sequence layouts against standard-derived
  frames, the retry budgets, argument boundaries, YES/NO/timeout readings, and
  the distinction between a negative answer and a failed earlier step. All 16
  commissioning cases pass, including the nine pre-existing ones unchanged: the
  migration emits the same frames in the same order with the same retry budgets,
  so the existing bus-level expectations still hold.
- DT8 vectors cover the four-step layout against standard-derived frames, the
  per-step retry contract, selector and address placement, argument boundaries,
  MSB/LSB assembly, and the partial and failed cases. All 41 DT8 cases pass,
  including the four pre-existing colour-read cases unchanged.
- All 22 host suites pass.
- These changes are host- and compile-verified only. The atomic paths themselves
  have not been exercised on a real bus, no site has been re-scanned to confirm
  the multi-type enumeration against actual DT6/DT8 gear, and commissioning has
  not been re-run against hardware.
- Scheduler sequences now record one backward frame per reply-bearing step, so a
  workflow needing replies from several steps fits in one atomic queue entry.
  Replies live in a single 64-byte `DaliSequenceResult` held for the active
  sequence only, so the 16-entry queue does not grow; the native CLI's four
  synchronous slots grow by about 64 bytes each.
- Host vectors cover per-step capture, retention of replies gathered before a
  later step aborts, and the accessor boundaries.
- Multi-byte input polling is the first workflow moved onto that primitive.
  `dali_input_poll_build_value_sequence()` emits QUERY INPUT VALUE followed by
  one QUERY INPUT VALUE LATCH per remaining byte, and
  `dali_input_poll_value_from_sequence()` assembles the reading only when every
  step replied. The ESPHome sensor path no longer chains two independent
  transactions, so the bytes of one latched reading cannot be separated by other
  bus traffic, and a full queue can no longer strand a half-finished read.
  Independent vectors cover the frame layout, argument boundaries, MSB-first
  assembly, and the partial and failed cases.
- Memory reads have typed sequence builders for both forms:
  `dali_memory_build_read_sequence()` for Part 102 control gear and
  `dali_memory_build_control_device_read_sequence()` for Part 103 control
  devices, each emitting DTR1, DTR0, then one READ MEMORY LOCATION per byte,
  with `dali_memory_read_from_sequence()` collecting the bytes only when every
  read replied. Read steps deliberately carry no retry budget because READ
  MEMORY LOCATION advances DTR0. A block is capped at
  `DALI_MEMORY_MAX_SEQUENCE_READ_BYTES` (5), the space left after the two setup
  steps. The ESPHome console `memread` now uses the typed control-device builder
  instead of a hand-assembled `0x3C` frame. Independent frame vectors cover both
  forms, the block layout, argument boundaries, and the partial and failed cases.
- `components/dali/dali_transport.c` is the new home of the bus abstraction that
  discovery, commissioning, memory, and input polling share. `DaliTransport`
  keeps the existing per-frame `transact` and adds an optional
  `transact_sequence` that runs a whole `DaliSequence` with nothing interleaved.
  `dali_transport_run_sequence()` uses it when present and otherwise issues the
  steps individually — same frames, no atomicity — and
  `dali_transport_supports_atomic_sequence()` lets a caller tell the two apart.
  The ESPHome scan task and the native CLI both provide the atomic form.
- `DaliDiscoveryTransport` and `DaliMemoryTransport` are now the same type, so
  the hand-rolled struct conversion in discovery is gone and one transport value
  serves every module.
- Blocking callers size their wait with `dali_transport_sequence_timeout_ms()`
  instead of a single-frame constant. The native CLI previously waited 200 ms for
  a whole sequence, which a seven-step sequence can exceed several times over.
- Memory reads run through the transport in chunks of at most five bytes, each
  chunk re-issuing its own DTR1/DTR0. This removes the READ MEMORY LOCATION retry
  hazard recorded below: read steps carry no retry budget, so a lost reply now
  fails the read instead of silently returning the following location. The Bank 0
  identity read that discovery performs costs six extra setup frames (26 instead
  of 20) in exchange for a re-established offset at every chunk boundary.
- All 22 host test executables pass, with 43 cases in the scheduler suite, 7 in
  the input-poll suite, 39 in the memory suite, and 9 in the new transport suite
  covering capability reporting, the atomic and fallback paths, failure
  truncation, reply retention, argument handling, and the wait budget.
- The ESPHome protocol wrapper set matches the 20 reusable C source files.
- The ignored compile-test configuration builds the working-tree component with
  ESPHome 2026.7.4 (ESP-IDF 5.5.5), warning-free, at 34.9% RAM (63144 bytes) and
  49.9% flash (916335 bytes). This is a newer ESPHome than the 2026.6.2 recorded
  below; the three pinned YAMLs were not re-checked against it.
  `_local/dali-diag-local.yaml` had been switched to the `v1.0.1` git source and
  was restored to `type: local` with `path: ../esphome/components`, without
  which the compile test verifies the release rather than the working tree.
- The native ESP-IDF firmware builds warning-free with ESP-IDF 6.0.1; the
  application binary uses 22% of its partition (0x38100 bytes).
- A C++ translation unit mirroring the ESPHome memory-read callback compiles
  against the new API, confirming the signature and accessors work from C++.
- This change is host- and compile-verified only; it has not been run on hardware.

### Verified locally on 2026-08-10

- The audit began from commit `0302d70` (tag `v1.0.1`); all three working-tree
  deployment YAMLs now reference that tag.
- All 21 host test executables pass.
- The native ESP-IDF firmware builds successfully with ESP-IDF 6.0.1.
- All three pinned YAMLs pass `esphome config` with ESPHome 2026.6.2 and
  resolve the published `v1.0.1` external component.
- The current `dev` worktree compiles and links as a local ESPHome 2026.6.2
  external component through `_local/dali-diag-local.yaml`; dispatch schema boundary
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
- Cross-core light-state updates now use one packed atomic latest-value mailbox,
  so on/off and level cannot be mixed across updates and a newer publish cannot
  be erased by the consumer. A portable C++ host suite covers empty, coherent,
  coalesced, and successive publish/take behavior; hardware is not re-verified.
- Every ESPHome command-console path now reports scheduler admission failures
  consistently: queue pressure is `queue full`, other rejections are `err`, and
  direct commands publish `OK` only after successful enqueue. Async commands
  publish `pending`, and generation-gated callbacks prevent an older completion
  from overwriting the newest command result. This is compile-verified; the
  console parser/result layer still has no direct host test.
- ESPHome full-light refresh now admits only one query at a time, retains its
  cursor on scheduler queue pressure, pauses admission during scans, and
  coalesces overlapping requests into one follow-up pass. Short-address lights
  query their own target by default, and a successful scan requests fresh state
  using the rebuilt group map. Portable cursor tests cover retries, skips, and
  coalescing; the ESPHome integration compiles, but hardware is not re-verified.
- Scheduler RX handoff and transmit spacing are now independent. Locally generated
  forward frames wait a rounded 22 Te guard after a local transmit attempt.
  Send-twice operations conservatively bracket both blocking PHY calls and, when
  both PHY calls succeed, fail with `DALI_ERR_TIMING` if the second call returns
  beyond 100 ms. A PHY error takes precedence. Pre- and post-PHY checks detect
  both delayed scheduler service and a blocking transmit that crosses the
  deadline. Host tests cover exact scheduler boundaries, clock wrap, retry/reset
  state, and sequence failure; hardware is not re-verified.
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
  transport, protocol/control APIs, discovery, control-gear commissioning, memory
  helpers, DT6/DT8 builders, input-device support, events, mapping, and dispatch.
- `dali_transport` is the single bus abstraction the higher modules share. It
  offers per-frame and whole-sequence entry points; only the sequence form is
  atomic, and it is optional, so callers that require atomicity must ask via
  `dali_transport_supports_atomic_sequence()` rather than assume it.
- Static allocation and the buffer-first ISR model are preserved in the timing
  and protocol layers.
- Scheduler sequences run contiguously and report a `DaliSequenceResult` holding
  the overall error, the failing step, the steps attempted, and one backward
  frame per reply-bearing step. This is the primitive the atomic transaction
  facility needs. Multi-byte input polling, memory reads, and discovery's
  ENABLE DEVICE TYPE pairs, group query, and multi-type enumeration are built on
  it. Commissioning still issues independent transactions, as do discovery's
  remaining single, order-independent queries.
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
- Console enqueue results are mapped consistently. Async commands publish
  `pending`, replace it on completion, and ignore callbacks belonging to an older
  submitted command. `OK` still means queued rather than execution- or
  device-level confirmation.
- Control-device `memwrite` is one scheduler-contiguous sequence and cannot be
  partially enqueued. Its `OK` result means queued, not device-acknowledged or
  read-back verified, and it does not exclude another physical bus master.
- Light state transferred from the DALI task to ESPHome is one coherent packed
  update; multiple pending observations intentionally coalesce to the latest.
- Boot, periodic, deferred, and post-scan light refreshes share a one-query-at-a-
  time pump. Queue-full leaves the current light pending for retry, and refresh
  requests arriving during a pass coalesce into one additional pass.
- Light refresh is the only traffic source that pauses for a bus scan. Sensor
  polls, identify, headless dispatch, and Home Assistant light writes keep
  enqueuing, so a scan is not an exclusive bus session.
- Matching Device/Instance events request an immediate authoritative sensor
  poll; event information is never published as a generic sensor value.
- A sensor reading is one scheduler sequence, so a two-byte instance cannot have
  its latching query and its latch read separated by other traffic. Admission is
  all-or-nothing, and a rejected poll simply retries on the next interval.
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

`DaliError` adds `DALI_ERR_TIMING = 8`. Existing numeric values remain unchanged;
callers with exhaustive error handling should add the new scheduler result.

`DaliSequenceCompletionCb` changes from
`(DaliError, uint8_t failed_step, const DaliFrame *last_reply, void *cb_ctx)` to
`(const DaliSequenceResult *result, void *cb_ctx)`. The new result carries the
overall error, the failed step, the number of steps attempted, and one backward
frame per reply-bearing step, read through `dali_sequence_result_reply()` and
`dali_sequence_result_last_reply()`. Replies collected before a failing step are
retained rather than discarded. The pointer refers to scheduler-owned storage
that the next sequence overwrites, so a callback must copy anything it keeps.
Transaction callbacks (`DaliSchedCompletionCb`) are unchanged; only sequence
callers need migrating.

`DaliDiscoveryTransport`, `DaliMemoryTransport`, and `DaliDt8Transport` are now
aliases of the shared `DaliTransport` in the new `dali_transport.h`, and
`DaliDiscoveryTransactionFn`, `DaliMemoryTransactionFn`, and
`DaliDt8TransactionFn` alias `DaliTransactionFn`. Existing code keeps compiling,
and a transport built for one module can now be passed to another without
conversion. The struct gains an optional `transact_sequence` member between
`transact` and `ctx`: designated initialisers are unaffected, but any positional
initialiser must be updated. `DALI_MEMORY_QUERY_RETRIES` is removed, because
memory reads no longer retry individual READ MEMORY LOCATION frames.

`dali_gear_dt8.h` now includes `dali_transport.h`, which transitively pulls in
`dali_scheduler.h`. A translation unit that included only `dali_gear_dt8.h` for
its pure frame builders now also sees the scheduler and transport types.

## Installation State

The three active firmware configurations pin their external component to
`v1.0.1`, the current known-working release:

| Configuration | Role |
|---|---|
| `dali_diag.yaml` | Tracked diagnostic/discovery firmware |
| `_local/dali-1k.yaml` | First-floor site firmware; 16 control gear and group entities for groups 0/2/3/4/5/6/7 |
| `_local/dali-2k.yaml` | Second-floor site firmware; group 0 lighting, HA console, and Steinel HF 360 II polling |

The entire `_local` directory is deliberately ignored by Git. This checkout also
contains `_local/dali-diag-local.yaml`, a compile-test copy of the tracked diagnostic
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

- Complete bus timing beyond the local own-forward-frame guard: export reliable
  backward/external frame-end timestamps, implement DALI-2 priority/backoff and
  collision/intervening-frame handling, and add a deadline-aware PHY call if a
  repeat that crosses the 100 ms limit must be suppressed rather than transmitted
  and reported late.
- Fix the COMPARE collision inversion. When two or more unaddressed devices
  answer COMPARE at once, the overlapping backward frames usually fail Manchester
  decode; the PHY drops them before the scheduler sees anything, so the reply
  window times out and `dali_commissioning_compare()` reports NO. The binary
  search then walks past real devices, which makes commissioning dependable only
  with a single unaddressed device on the bus. IEC 62386-102 requires undecodable
  reply-window activity to count as YES, so this needs a new PHY signal for
  "activity detected but not decodable" that the scheduler can surface as a
  distinct result.
- Strengthen commissioning RANDOMIZE timing, collision handling,
  equal-random-address recovery, and the separation of gear and control-device
  address spaces.

### P0 — Transaction and runtime reliability

- Add a scheduler-level atomic transaction/session facility for DTR operations,
  ENABLE DEVICE TYPE sequences, memory access, DT8 multi-byte queries,
  send-twice commands, discovery, and commissioning. Sequences already execute
  contiguously and report a reply per step; multi-byte input polling and memory
  reads are built on them, and `DaliTransport` now carries an atomic-sequence
  entry point that both real transports implement. Discovery and commissioning
  have both been moved across: discovery's ENABLE DEVICE TYPE pairs, group
  query, and multi-type enumeration, and commissioning's SEARCH ADDRH/M/L
  triple, its search-plus-COMPARE probe, and its PROGRAM/VERIFY pair are all
  sequences, as is the DT8 16-bit colour value read. Every workflow where an
  interleaved frame could produce a wrong answer rather than an error has now
  been moved across. What remains is benign by comparison: commissioning's
  TERMINATE / INITIALISE / RANDOMIZE opening is still three independent
  transactions, and the ESPHome console's `dtrcheck` and `iconfig` paths still
  assemble their own DTR setup steps. An interleaved frame in either case costs
  a retry, not a corrupted reading.
- Give `dali_sched_reset()` a defined contract: complete every queued and active
  transaction with an error before clearing state instead of dropping their
  callbacks. The native CLI `reset` verb currently orphans its diagnostic sync
  slots, which stay `in_use` permanently and make every synchronous CLI verb
  return BUSY until reboot. The planned ESPHome bus-fault recovery would hit the
  same trap through the console `pending` results and the refresh in-flight flag.
- Decide whether scans still need to be exclusive. Only the light-refresh pump
  observes `scan_running_` today; sensor polls, identify blinking, headless
  dispatch actions, and Home Assistant light writes all keep enqueuing during a
  scan. The concrete corruption this used to cause is gone — discovery's
  order-dependent queries are now atomic sequences, so an interleaved frame can
  no longer clear an enabled device type mid-workflow. What remains is scan
  duration under contention rather than wrong data, so exclusivity is now a
  performance question, not a correctness one.
- Make the reply retry budget a per-call-site decision. `dali_control_query()`
  gives every reply-bearing transaction `DALI_MAX_RETRIES`, so a semantically
  negative or absent-device answer costs four reply windows (about 140 ms).
  Refresh passes and scans pay that for every silent address.
- Audit the remaining retry budgets for commands that mutate device state the
  way READ MEMORY LOCATION advances DTR0. Four cases are fixed: memory reads,
  the ENABLE DEVICE TYPE pairs, QUERY NEXT DEVICE TYPE, and QUERY COLOUR VALUE
  all now run as sequences whose state-advancing steps have no retries.
  `dali_discovery_query_u8()` still retries every query it issues, which is safe
  only while the queries left on that path stay idempotent — they are today, but
  nothing enforces it. The pattern to check for is a query whose reply depends
  on a register the same query modifies.
- Handle remaining non-console enqueue failures in identify, diagnostic, and
  headless-dispatch paths, and expose queue depth/high-water/drop diagnostics.
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

- Replace the blanket ten-second startup write suppression with logic that
  distinguishes restore/default writes from intentional user commands.
- Give input-sensor values the same packed single-word mailbox the lights use.
  `DaliInputSensor` still keeps separate `pending_raw_` and `dirty_` atomics and
  clears `dirty_` before reading the value, so a Core 1 publish landing between
  the two is discarded with its flag already consumed. The next poll recovers the
  value, but this is the lost-update pattern the light handoff removed.
- Reject trailing tokens in the ESPHome console parser. Verb handlers test
  `ntok >= N` and ignore the remainder, so `level a1 100 junk` is accepted.
- Extend the compact Part 103 dispatch key if a site needs to distinguish
  Device-Group from Instance-Group sources or match instance type; the canonical
  event/capture path retains these fields, but the five-field rule key does not.
- Replace the 128-byte Find Couplers summary with a paged/exportable result; all
  canonical captures are logged, but the Home Assistant aggregate can truncate.
- Move the remaining raw opcodes out of `dali_component.cpp` into reusable typed
  C APIs. The console `memread` path is now the typed control-device read
  sequence, but `dtrcheck` still hand-builds the QUERY CONTENT DTR0/DTR1 device
  opcodes `0x36`/`0x37`, and `iconfig` still assembles its own DTR0 setup step.
- Use board-aware ESPHome GPIO schemas, reject TX=RX and invalid output pins, and
  honor the documented WROVER-E restrictions. The schema currently accepts
  input-only GPIO34-39 as TX and accepts TX equal to RX. Propagate hardware
  failures out of `dali_phy_init()` as well: both `gpio_config()` calls,
  `gpio_isr_handler_add()`, and the GPTIMER alarm, callback, and enable calls are
  issued without checking their return, so a PHY that cannot drive its transmit
  pin still reports success and the component starts up looking healthy.
- Check the remaining DALI-task creation result and remove or validate hard-coded
  Core 1 assumptions, particularly for single-core ESP32 targets. Scan-task
  allocation/creation failure is now reported and releases scan/refresh state.
- Define recovery after DALI bus faults and bus-only power cycles. Distinguish
  current fault/availability from cumulative fault history.

### P1 — Release and verification quality

- Add CI that compiles a clean external-component checkout from the release tag and
  validates the Python schema, ESPHome C++ layer, YAML, and wrapper packaging.
- Add the native ESP-IDF build to CI alongside the existing 21-suite host workflow.
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
- Add capture replay, parser fuzzing, and hardware-in-loop tests for timing,
  collisions, queue pressure, bus faults, and power restoration.
- Add DT1 and other device types only when an installation requires them, following
  the shared DT6/DT8 module pattern.

## Operational Constraints

- Do not imply DALI Alliance certification or complete IEC 62386 coverage.
- The controller has no proven collision-detection/arbitration strategy. Existing
  direct-control couplers work on the installed buses, but simultaneous
  transmissions remain a risk.
- Treat input-device configuration writes as experimental until real-bus
  read/write/read-back validation is complete.
- Do not use GPIO16 or GPIO17 on the WROVER-E target.

## Source Layout

| Path | Role |
|---|---|
| `components/dali` | Reusable C protocol, scheduler, PHY, discovery, dispatch, memory, and device-type stack |
| `main/main.c` | Native ESP-IDF diagnostic application entry point |
| `main/dali_diag.c/.h` | App-specific serial CLI |
| `esphome/components/dali` | Active ESPHome external component |
| `dali_diag.yaml` | Tracked diagnostic/discovery firmware |
| `_local/dali-diag-local.yaml` | Ignored compile-test copy of the diagnostic firmware |
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
esphome compile _local/dali-diag-local.yaml  # ignored compile-test copy; dummy secrets
esphome compile _local/dali-1k.yaml
esphome compile _local/dali-2k.yaml
```

For an uncommitted/dev component compile, use the ignored
`_local/dali-diag-local.yaml` source with `type: local` and
`path: ../esphome/components`. Keep the
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
