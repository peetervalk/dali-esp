# DALI-ESP Current Status

**Last updated:** 2026-08-12

**Known-working deployment/component baseline:** `v1.0.1` (`0302d70`), for
the two recorded site configurations; this is not a conformance claim.

**Deployed now:** the 1k site runs the working tree, not the release. See
Installation State for what each configuration currently resolves to — the tag
above is the last release known good, which is no longer the same statement as
"what is on the hardware".

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
| CLI completeness | **Surface complete, verification incomplete.** Every shared capability now has a typed native verb, including memory, DT6, DT8, input-device query/configuration, vendor helpers, control-device memory, CONTINUOUS UP/DOWN, arc power MASK, and send-twice `raw2`. `dali_capability_matrix.md` tracks the gap that remains: DT6, DT8, memory writes, and input-device configuration have host vectors but no real-bus result. |
| ESP32 / ESPHome controller | **Working installation-grade baseline.** The two known sites have operational brightness control, observation, sensor polling, diagnostics, and discovery. It is not yet a general, fully state-correct DALI controller. |
| Protocol separation | **Directionally strong.** The reusable C stack is independent of ESPHome, and every frame and sequence the ESPHome layer sends is now built by a shared builder in `components/dali` — no hand-assembled opcodes or steps remain in `dali_component.cpp`. What is still ESPHome-bound is the wiring: console dispatch, the refresh pump, and the entity registries, none of which host tests can reach. |
| Standards confidence | **Selected workflows verified, not complete conformance.** Host tests and recorded hardware results provide useful evidence, but some tests repeat implementation constants rather than independent standard-derived vectors. The project is not DALI Alliance certified. |

The next development phase should prioritize protocol and state correctness before
adding broad new device support.

## Verification Baseline

### Verified locally on 2026-08-12 (console verb parity)

- The ESPHome console now implements every native CLI verb whose answer fits one
  Home Assistant text state and whose execution fits one enqueue and one
  completion. Added: the Part 102 fade/step instructions (`up`, `down`,
  `step-up`, `step-down`, `step-off`, `on-step`, `cont-up`, `cont-down`,
  `dapc-seq`, `last`), `scene`, `mask`, `status`, `dtr`, `special`, `dt6`,
  `memread`, `vendor`, and the DTR0-selected form of `iquery`. What stays
  native-only is listed with its reason in `dali_capability_matrix.md`.
- All 24 DT6 names are reachable from the console under the native CLI's
  spellings, through `dali_dt6_build_command_sequence()`, so a DTR0 load, ENABLE
  DEVICE TYPE 6, and the command cannot be separated by other locally scheduled
  traffic.
- `dt6 select-curve` drops the address's cached level profile and starts a
  refresh, matching what `config_changes_level_profile()` already did for
  SET MIN/MAX LEVEL and RESET. The curve is what every brightness the light
  layer sends is computed from, so leaving it cached would misreport and
  miscommand in both directions.
- Reply decoding is now shared. `dali_cli_format_response()` and
  `dali_cli_format_status()` produce the one-line form the console publishes,
  and `dali_cli_print_response()` is that line plus the newline, plus the
  per-field block for a status byte. A yes/no query therefore answers `yes` on
  both surfaces rather than `255` on one. This is an operator-visible change to
  the `command_result` string; see the release-notes item below.
- `special` refuses the nine commissioning primitives marked by
  `dali_cli_special_is_commissioning()` — INITIALISE, RANDOMISE, the three
  SEARCH ADDRESS registers, PROGRAM SHORT ADDRESS, WITHDRAW, and both WRITE
  MEMORY LOCATION forms — because this integration exposes discovery, not a
  guarded commissioning workflow. TERMINATE stays available as the remedy for a
  window another tool opened. Host vectors assert the set in both directions.
- A console verb that moves the level now arms the deferred level query the
  Part 103 dispatch path already used after a dim or scene, instead of leaving
  Home Assistant to catch up on the next periodic poll. It covers `level`, `off`,
  `up`, `down`, the four step verbs, `cont-up`/`cont-down`, `last`, `max`, `min`,
  and `scene`; `mask` and `dapc-seq` are excluded because neither changes the
  level. The 600 ms arming window is unchanged, so a burst of step commands still
  costs one refresh rather than one query each. Verified by compile only — the
  wiring is in `dali_component.cpp` and has no host vectors, like the rest of the
  console's dispatch.
- 65 host vectors now cover `dali_cli`; all 26 suites pass. `dali-diag` and
  `dali-1k` compile clean with no new warnings. None of this has been run on a
  bus.

### Verified on hardware 2026-08-12

- Arc power level and light output are no longer treated as the same quantity.
  `components/dali/dali_dim_curve.c` implements the IEC 62386-102 logarithmic
  curve, `output(X) = 10 ^ ((X - 1) / (253 / 3) - 3)`, and the ESPHome light
  entity converts through it in both directions. Level 85 is 1 % of maximum
  light, not the 33 % a linear reading of 85/254 reported, and a requested 1 %
  now sends level 85 rather than level 3 — which was 0.1 %, dark enough to look
  like a failed command.
- Confirmed on the 1k site: a lamp already sitting at 1 % light output read 1 %
  in Home Assistant after the flash, where it had read 33 % before. This is the
  first hardware verification of any work past `v1.0.1`.
- The conversion has to round-trip exactly or the light entity mistakes the echo
  of its own bus reading for an operator command and transmits it.
  `test_dim_curve` asserts level → output → level for all 254 levels, plus the
  standard's anchor points computed independently in double precision.
- The on/off threshold moved from `brightness >= 1/254` to `brightness > 0`.
  The dimmest legal level emits 0.1 %, below the old linear floor, so a level-1
  observation would otherwise have echoed back to the bus as OFF.
- Input sensors take a per-instance `poll_on_event` option, default `true`.
  An event is not evidence of a change: the Steinel reports instance 0 every
  3.0 s and instance 1 every 1.0 s with an unchanged value, so following every
  event replaced the configured 30 s lux interval with the device's report rate.
  Measured cost on the live bus: every lux poll delayed the next occupancy event
  by about 95 ms, and roughly one poll in ten returned no reading at all.
  Occupancy keeps `true` for latency; lux, temperature and humidity are `false`.
- 26 host test executables pass. The ESPHome protocol wrapper set matches the 22
  reusable C source files.
- The 2k site configuration compiles against the working tree as a local ESPHome
  2026.7.4 external component; the image is 938591 bytes.

Not verified, and worth stating plainly:

- The 2k site has not been reflashed, so `poll_on_event` has no hardware result.
  Everything above about the poll rates is measurement of the old behavior plus
  a compile of the new.
- ESPHome now acquires and caches a per-short-address MIN/MAX/curve profile for
  refreshes, uses physical-output interpolation for standard and linear curves,
  and accepts `min_level`, `max_level`, and `dimming_curve` overrides. The
  profile query and HA mapping are host-tested and compile in a local ESPHome
  fixture, but have no hardware result yet.
- A group or broadcast entity maps brightness through the union of its known
  members' windows, not one representative's. Gear clamps any arc power level
  into its own MIN/MAX whatever the sender believed, so a narrower window cannot
  make a mixed group uniform — it can only make levels the hardware could reach
  unreachable, and would cap the group at its dimmest member's ceiling. Each
  member therefore dims until it hits its own floor and holds. Members are only
  counted once a scan has read their limits; until then the union is whatever is
  known, which for a fresh device is the representative alone. A group whose
  members disagree on the dimming curve falls back to the standard curve, since
  no single mapping drives both correctly.
- An observed level outside an entity's window is reported as the nearest level
  in it rather than refused. That happens legitimately — a narrower configured
  window, or a group member answering for a level the whole group was given.
- Existing Home Assistant scenes and automations that store a brightness
  percentage now produce very different light output — 20 % was level 51
  (0.4 % light) and is now level 195 (20 % light). They need re-tuning. Home
  Assistant's 8-bit brightness also can no longer reach levels 1-50; brightness
  1 maps to level 51. The console `level` verb still reaches the full range.

### Verified locally on 2026-08-11

- The native CLI is now table-driven, and the half of it that decides what a
  typed line means is portable and host-tested. `main/dali_cli.c` owns
  tokenising, the verb table, argument validation, the named command tables, and
  response formatting; `main/dali_diag.c` keeps the FreeRTOS task, the blocking
  scheduler slots, and the long-running workflows. A verb is reachable only
  through the one table, and `dali_diag.c` switches over `DaliCliCommandId` with
  no default case, so a table entry without a handler is a `-Wswitch`
  diagnostic. 57 host vectors cover this layer.
- Trailing tokens are now rejected for every verb instead of ignored. The table
  carries each verb's argument-count bounds and `dali_cli_resolve()` enforces
  them before a handler runs, so `level a1 100 junk` is refused rather than
  acted on. The same class of bug remains open in the separate ESPHome console
  parser, which does not share this code.
- Help and `list <table>` are generated from the same tables the parser
  dispatches on, so they cannot describe a command the CLI does not accept. The
  host suite additionally asserts that every `DaliCliCommandId` has a table
  entry, that verb and table names are unique, and that every verb appears in
  help.
- The table entries are checked against the shared stack rather than trusted:
  every `query` name must map to an addressed 16-bit command that expects a
  reply, every `config` name to a send-twice configuration command whose
  `uses_dtr0` column agrees with `dali_control_config_uses_dtr0()`, every
  `special` name to a special frame, and any ranged opcode must declare a
  parameter. A name pointing at the wrong `DaliCommandId` would otherwise
  transmit a different command than the operator asked for, silently.
- New typed verbs: `memread`/`meminfo` for Part 102 control-gear memory,
  `devmem read`/`devmem write` for Part 103 control-device memory, `dt6` and
  `dt8` for the device-type command tables including the 16-bit colour value
  read, `iquery`/`iconfig` for Part 103 instance query and configuration,
  `vendor lunatone`/`vendor steinel`, and `list` for any named table. `iconfig`
  reports transmitted, not applied, and says so on every success line.
- The native CLI names the control-device memory verbs `devmem read`/`devmem
  write` while the ESPHome console calls the same operations `memread`/
  `memwrite`. Native `memread` is the Part 102 control-gear form. The two use
  different DTR and memory opcodes, so the divergence is deliberate and
  documented rather than silent; converging the two consoles is not scheduled.
- Every new multi-frame verb runs as one scheduler sequence.
  `dali_dt6_build_command_sequence()` and `dali_dt8_build_command_sequence()`
  carry [DTR0..DTRn] + ENABLE DEVICE TYPE + command, and
  `dali_input_build_config_sequence()` carries the Part 103 control-device DTR
  loads plus the command, with no ENABLE step because Part 103 does not use one.
  No step in any of them carries a retry budget: a lone retransmission of the
  command would run without its enable, and a repeated DTR write cannot be told
  apart from the caller's next value. These live in the reusable stack, so the
  ESPHome DT8-to-Home-Assistant mapping can use them later.
- `raw2` sends one arbitrary frame twice through the scheduler's send-twice
  path. Two manually typed `raw` commands cannot meet the 100 ms window, so a
  send-twice command entered that way was never the command the standard
  describes. Frame parsing bounds the value by the stated width, so a mistyped
  length is refused rather than transmitted as a differently framed command.
- CONTINUOUS UP and CONTINUOUS DOWN (IEC 62386-102:2022 opcodes 11 and 12) are
  in the shared command table with `dali_control_build_continuous_up/down()` and
  the `cont-up`/`cont-down` verbs. Standard-derived vectors cover the opcodes,
  the short/group/broadcast address bytes, the send-once metadata, and rejection
  of a non-zero parameter, which would otherwise walk into the GO TO SCENE range.
- Arc power MASK (255) has its own builder, `dali_build_dapc_mask()` and
  `dali_control_build_dapc_mask()`, reachable as `mask <target>` or
  `level <target> mask`. The ordinary DAPC builders still reject 255, so no
  level arithmetic can land on MASK and silently stop meaning "set this level".
- `dali_capability_matrix.md` is the new per-capability record: shared API,
  native verb, host vector, real-bus result, and ESPHome exposure. It is what
  makes the remaining gap legible — DT6, DT8, memory writes, and input-device
  configuration are implemented and host-covered but have never been run against
  physical gear.
- A verb whose first argument is a fixed keyword declares those keywords in the
  same table row, and the handler tests membership with
  `dali_cli_has_subcommand()` rather than comparing its own literal. This was
  added after the first version of the table advertised `bus on|off` while the
  handler accepted only `bus check`: help told the operator to type a command
  that could not work, and nothing caught it. A host vector now asserts that
  every declared keyword is recognised and appears in the usage line.
- All 24 host executables pass. The new CLI suite is 59 cases; protocol is now
  66, control 31, DT6 21, DT8 46, and input config 9.
- The native firmware builds with ESP-IDF 6.0.1 (`0x3C0A0`-byte application
  image). `_local/dali-diag-local.yaml` builds the working-tree component with
  ESPHome 2026.7.4 (919611-byte OTA image); the shared-stack header changes did
  not disturb the ESPHome wrapper set.
- These changes are host- and compile-verified only. No new verb has been
  flashed or exercised on COM6, and neither site deployment has been re-tested.

- Light command deduplication is now committed only by a scheduler completion.
  Previously the cached on/level was written as soon as `dali_control_*`
  returned `DALI_OK`, which means queued, not transmitted: a command that later
  failed on the bus left the cache asserting success, and the identical retry
  from Home Assistant was then suppressed, so the gear stayed at the old level
  with nothing able to correct it. A failed transmission now invalidates the
  cache instead — the real level is unknown, so nothing may be suppressed
  against it — and re-arms the same desired state for one bounded retry.
  `DALI_LIGHT_WRITE_TX_RETRIES` bounds it so a dead bus cannot generate traffic
  indefinitely, while the cache stays invalid so an operator's repeat still
  reaches the bus.
- The same change closes a silent drop: outside a scan, a queue-full enqueue
  previously returned without retaining the command at all. Enqueue rejection is
  transient back-pressure, so the desired state is now retained and retried on a
  later loop, matching what the scan path already did.
- The arbitration is a portable header, `components/dali/dali_light_write.h`,
  with the ESPHome entity holding one `DaliLightWrite`. Only one command per
  light is in flight at a time, so a completion can never be attributed to the
  wrong level, and a bus readback arriving while a command is in flight is
  refused rather than committed over the state that command is establishing.
  A newer desired state supersedes a failed one, so a stale level is never
  resurrected over more recent operator intent. 19 independent host vectors
  cover the enqueue/transmission distinction, the retry budget, observation
  precedence, and the argument boundaries.
- `dali_control_set_level_cb()` and `dali_control_off_cb()` are the new
  completion-carrying forms. Three control vectors assert that their `DALI_OK`
  precedes transmission and that a PHY failure surfaces only in the completion.
  The completion runs on the DALI task, so it reaches Core 0 through a new
  single-slot `DaliLightCommandMailbox` that reports how many completions it is
  acknowledging and whether any failed.
- The scheduler reports queue admission diagnostics through
  `dali_sched_queue_stats()`: depth, capacity, high-water, admitted, and
  rejections split into queue-full and reset-barrier. A rejected submission is
  dropped work, because the scheduler never retries one on the caller's behalf.
  Native `stats` includes them, native `queue [reset]` and the ESPHome console
  `queue [reset]` report them directly, and ESPHome logs a warning whenever
  either rejection counter advances. The console verb generates no bus traffic
  and is therefore the only one accepted during a scan.
- Remaining unchecked enqueue paths now handle their failures. The identify
  blink retains its phase and deadline on a rejected enqueue so the next loop
  retries that half-blink instead of stalling at one level; the diagnostic
  on/off/max/min/refresh buttons report a rejection on the diagnostic text
  sensor and in the log; headless dispatch distinguishes an unmappable frame,
  which stays at debug level, from a matched entry whose action was refused,
  which is now a warning. A refused dispatch action remains intentionally
  dropped rather than replayed against a stale physical context.
- All 23 host executables pass. The scheduler suite is 50 cases, control 29, and
  the new light-write suite 19.
- The native firmware builds with ESP-IDF 6.0.1 (`0x38AA0`-byte application
  image). `_local/dali-diag-local.yaml` builds the working-tree component with
  ESPHome 2026.7.4 (919515-byte image).
- These changes are host- and compile-verified only. They have not been flashed
  or exercised on COM6, and neither site deployment has been re-tested.

- The correctness audit closed the remaining local split-transaction paths.
  Every existing dependent discovery, commissioning, memory, DT8, and input-
  polling executor now uses `dali_transport_run_sequence_atomic()` and rejects a
  frame-only or incomplete transport before sending traffic. The ordinary
  stepwise runner remains available only for callers that explicitly accept
  local interleaving. This does not exclude a separate physical DALI master.
- Native `sensor poll` now uses the same atomic input-value sequence as ESPHome.
  Response retry policy is command-aware: QUERY NEXT DEVICE TYPE, READ/WRITE
  MEMORY LOCATION, and QUERY INPUT VALUE LATCH never retry after a lost reply,
  while idempotent reads retain their retry budget. The native typed `query` and
  `special` paths use that policy too.
- Scheduler reset is owner-task deferred and fenced. Active and queued
  transactions/sequences receive exactly one `DALI_ERR_CANCELLED` completion,
  partial sequence results are retained, queue admission stays closed through
  the reset callback, and the native CLI resets the PHY only inside that owner-
  task barrier. It no longer orphans synchronous diagnostic slots or races a
  blocking PHY transmit.
- A 16- or 24-bit forward frame arriving during a local reply window now aborts
  the pending query with `DALI_ERR_INTERVENED`; a later backward byte cannot be
  misattributed as its reply and the invalidated query is not retried. Physical
  collision/activity detection is still open below.
- ESPHome scans gate all component-owned producers, wait for already admitted
  work to drain, and retain due sensor/refresh requests. Identify timing pauses;
  console/diagnostic commands are rejected; headless events are drained but
  actions are intentionally suppressed rather than replayed. HA light targets
  are stored as packed desired on/off+level values and retried after scan/queue
  pressure, so bus readback cannot overwrite them and more pending lights than
  queue slots drain over successive loops.
- Incomplete group enrichment no longer replaces or persists an authoritative
  empty map. `DGP2` invalidates legacy `DGP1` snapshots that older firmware may
  have created from partial scans; known members must be positively observed
  before replacement; incomplete scans retain the old map, withhold YAML, and
  surface the condition in HA. A valid gear group reply also classifies gear
  when the initial status reply was missed. Pure input devices no longer consume
  control-gear commissioning addresses.
- Broadcast toggle state is tracked, so repeated broadcast TOGGLE alternates
  rather than always issuing RECALL MAX.
- All 23 host executables pass. Focused totals include transport 12, input poll
  8, discovery 47, commissioning 20, memory 40, DT8 42, scheduler 50, dispatch
  30, group map 29, control 29, light write 19, and protocol 61 tests.
- The native firmware builds with ESP-IDF 6.0.1 (`0x387D0`-byte application
  image). `_local/dali-diag-local.yaml` builds the working-tree component with
  ESPHome 2026.7.4 (ESP-IDF 5.5.5; 918608-byte OTA image).
- These audit fixes are host- and compile-verified only. They have not been
  flashed or exercised on COM6, and neither site deployment has been re-tested.

- Commissioning's opening is one three-step sequence built by
  `dali_commissioning_build_start_sequence()`: TERMINATE, INITIALISE
  (unaddressed), RANDOMIZE. INITIALISE and RANDOMIZE are send-twice, so three
  logical steps become five forward frames. No step retries, because a repeated
  RANDOMIZE would hand out a fresh set of random addresses.
- That grouping also closed a leftover-state bug. If INITIALISE went out and
  RANDOMIZE then failed, the old code returned the error without a TERMINATE,
  leaving the gear in initialisation state for the full fifteen minutes with
  nothing on the bus aware of it. A failed start now issues TERMINATE whenever
  the INITIALISE step was attempted, and does not when only the opening
  TERMINATE itself failed. Both paths have vectors, driven by a new
  fault-injection hook in the commissioning mock.
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
  enumeration. With an atomic transport no other locally scheduled transaction
  can be interleaved, so the DT6 and DT8 enrichment bytes recorded during a busy
  scan can no longer be read under a device type that local traffic already
  cleared. A separate physical master can still interpose.
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
  truncation at capacity. The current discovery suite has 47 cases.
- Commissioning vectors cover the three sequence layouts against standard-derived
  frames, the retry budgets, argument boundaries, YES/NO/timeout readings, and
  the distinction between a negative answer and a failed earlier step. All 16
  commissioning cases pass, including the nine pre-existing ones unchanged: the
  migration emits the same frames in the same order with the same retry budgets,
  so the existing bus-level expectations still hold. The current commissioning
  suite has 20 cases.
- DT8 vectors cover the four-step layout against standard-derived frames, the
  per-step retry contract, selector and address placement, argument boundaries,
  MSB/LSB assembly, and the partial and failed cases. All 42 DT8 cases pass,
  including the four pre-existing colour-read cases unchanged.
- All 23 host suites pass.
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
  assembly, and the partial and failed cases. The native CLI uses this executor
  too; both production transports provide scheduler-backed atomic grouping.
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
  `transact_sequence` that runs a whole `DaliSequence` without other locally
  scheduled work interleaved.
  `dali_transport_run_sequence()` uses it when present and otherwise issues the
  steps individually — same frames, no atomicity — and
  `dali_transport_supports_atomic_sequence()` lets a caller tell the two apart.
  Dependent high-level executors use the strict atomic runner and never silently
  take that fallback. The ESPHome scan task and native CLI provide the atomic form.
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
- All 23 host test executables pass, with 50 cases in the scheduler suite, 8 in
  the input-poll suite, 40 in the memory suite, and 12 in the transport suite
  covering capability reporting, the atomic and fallback paths, failure
  truncation, reply retention, argument handling, and the wait budget.
- The ESPHome protocol wrapper set matches the 20 reusable C source files.
- The ignored compile-test configuration builds the working-tree component with
  ESPHome 2026.7.4 (ESP-IDF 5.5.5); the current OTA image is 918608 bytes. This
  is a newer ESPHome than the 2026.6.2 recorded
  below; the three pinned YAMLs were not re-checked against it.
  `_local/dali-diag-local.yaml` had been switched to the `v1.0.1` git source and
  was restored to `type: local` with `path: ../esphome/components`, without
  which the compile test verifies the release rather than the working tree.
- The native ESP-IDF firmware builds with ESP-IDF 6.0.1; the application binary
  is `0x387D0` bytes.
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
- `dali_phy_init()` checks every acquisition it makes — both `gpio_config()`
  calls, `gpio_set_level()`, `gpio_isr_handler_add()`, and the GPTIMER
  alarm/callback/enable calls — and releases what earlier steps acquired before
  returning an error. Core affinity is not hardcoded: `dali_worker_core()` in
  `dali_core_affinity.h` asks for no affinity on a single-core target, where
  pinning to core 1 would fail outright.
- The scheduler reports queue admission diagnostics, and `dali_control` offers
  completion-carrying level/off entry points for callers that must distinguish
  a queued command from a transmitted one. `dali_light_write.h` is the portable,
  integration-agnostic arbitration between desired, in-flight, and confirmed
  light state built on that distinction.
- Scheduler sequences run contiguously and report a `DaliSequenceResult` holding
  the overall error, the failing step, the steps attempted, and one backward
  frame per reply-bearing step. This is the primitive the atomic transaction
  facility needs. Every multi-frame workflow in the reusable stack is built on
  it: multi-byte input polling, memory reads and the control-device memory
  write, discovery's ENABLE DEVICE TYPE pairs, group query, and multi-type
  enumeration, commissioning's opening, search-plus-COMPARE probe, and
  PROGRAM/VERIFY pair, the DT8 16-bit colour value read, the generic DT6/DT8
  [DTR loads + ENABLE DEVICE TYPE + command] grouping, and the Part 103
  [control-device DTR loads + command] grouping. Only genuine single-frame
  queries still go through `transact` directly.
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
- DT6 and DT8 builder APIs are host-tested and fully surfaced through the native
  CLI, each command running as one sequence carrying its DTR loads, ENABLE
  DEVICE TYPE, and the command itself. They are not hardware-verified.
- DT1 and other specialized/legacy device types remain intentionally unimplemented.

### Native diagnostic CLI

- `main/main.c`, `components/dali/dali_cli.c`, and `main/dali_diag.c` provide the
  ESP-IDF serial diagnostic application. The split is deliberate: `dali_cli.c` is
  plain C with no ESP-IDF, FreeRTOS, or bus dependency and is built by the host
  tests; `dali_diag.c` owns the task, the blocking scheduler slots, the caches,
  and the long-running workflows. `dali_cli.c` sits in `components/dali` rather
  than `main/` because the ESPHome console dispatches through the same table.
- Dispatch is one table. `dali_cli_resolve()` tokenises, looks the verb up, and
  checks the argument-count bounds before any handler runs, so trailing tokens
  are rejected rather than ignored. Help and `list <table>` are generated from
  the same tables, so neither can describe a command the parser will not accept.
- Common gear commands and queries, DTR/config operations, scan/discovery,
  control-gear commissioning, inventory/export, input polling, capture, and
  coupler-finding workflows are present, plus typed verbs for control-gear and
  control-device memory, DT6, DT8, Part 103 instance query and configuration,
  and vendor helpers.
- `stats` and `queue [reset]` report PHY/RX counters and scheduler queue
  admission state respectively.
- Native `raw` sends one arbitrary 16- or 24-bit frame and `raw2` sends one
  twice through the scheduler's send-twice path. Both remain diagnostic escape
  hatches rather than substitutes for the typed atomic verbs.
- Native event capture/export reports canonical Part 103 source fields and full
  event information. Device/Instance push-button events are typed from the
  discovery cache rather than guessed from their event value.
- `identify`, `smoke`, `capture`, and the inventory JSON export still have no
  host vectors of their own; they are composed from covered primitives, but
  their output formats are unasserted.

### ESPHome component

- Active component: `esphome/components/dali`.
- `dali_diag.yaml` provides discovery and diagnostics, including scan, identify,
  find-couplers, target controls, bus monitoring, group-map output, and generated
  YAML log lines.
- Light entities currently expose brightness only. Shared DT8 support is not yet
  mapped to Home Assistant colour-temperature, XY, or RGB controls.
- Brightness is mapped through the gear's own window and curve, not `[1, 254]`.
  Each entity's MIN LEVEL, MAX LEVEL, and — for DT6 gear — dimming curve are
  queried per short address at boot, on every refresh pass, after a scan, and
  after any command that moves them (SET MIN/MAX LEVEL, RESET, DT6
  `select-curve`, each of which drops the cached profile first). A group or
  broadcast entity uses the union of its known members' windows, and YAML
  `min_level`, `max_level`, and `dimming_curve` options override what was read.
  A reduced ceiling therefore reports 100 % at MAX LEVEL, and an observed level
  outside the window is reported as the nearest level inside it rather than
  refused.
- The command console covers control-gear output and configuration, control-gear
  and control-device memory, DTR loads, non-commissioning special commands, DT6,
  Part 103 instance query and configuration, vendor helpers, `raw`/`raw2`, and
  the local `queue`/`group` verbs. It is the native CLI's verb set minus what
  needs a terminal or a blocking transport; `dali_capability_matrix.md` lists
  the exclusions with reasons. None of this implies hardware validation.
- Console enqueue results are mapped consistently. Async commands publish
  `pending`, replace it on completion, and ignore callbacks belonging to an older
  submitted command. `OK` still means queued rather than execution- or
  device-level confirmation. A reply is decoded by the same shared function the
  native CLI prints through, under the name of the command that asked for it;
  the pending name/kind/step record is written before the enqueue and read back
  under the same generation check that discards a stale completion.
- Control-device `devmem write` is one scheduler-contiguous sequence and cannot
  be partially enqueued. Its `OK` result means queued, not device-acknowledged or
  read-back verified, and it does not exclude another physical bus master.
- Light state transferred from the DALI task to ESPHome is one coherent packed
  update; multiple pending observations intentionally coalesce to the latest.
  `DaliInputSensor` uses the same packed single-word mailbox
  (`DaliInputValueMailbox`), so neither surface has a lost-update window between
  clearing the dirty flag and reading the value.
- Writes the operator did not issue are identified by what they are, not by when
  they arrive. ESPHome's restore/default write is the first `write_state()`
  after `setup()`, which is exactly the one `LightState::setup()` performs; a bus
  reading pushed back through ESPHome is matched on the exact `(is_on, level)`
  pair `apply_bus_state()` sent. There is no startup time window, so a command
  issued in the first seconds after boot reaches the bus.
- A level-changing console verb arms the same deferred re-read the Part 103
  dispatch path uses after a dim or scene, so Home Assistant follows a console
  `level`, `off`, fade, step, or `scene` without waiting for the periodic poll.
  The delay both lets a fade land before it is read and collapses a burst of
  step commands into one refresh; a fade longer than that window is read
  mid-fade and corrected by the next poll. `mask` and `dapc-seq` arm nothing —
  neither moves the level.
- A light entity suppresses a redundant command only against state a scheduler
  completion confirmed, never against a successful enqueue. A rejected enqueue
  retains the desired state and retries; a failed transmission invalidates the
  cache and re-arms one bounded retry, so a lost command cannot be masked by a
  cache the command never established. One command per light is in flight at a
  time, and a bus readback arriving during that window is refused rather than
  committed over the state the command is establishing. Confirmation still means
  transmitted, not device-acknowledged.
- Boot, periodic, deferred, and post-scan light refreshes share a one-query-at-a-
  time pump. Queue-full leaves the current light pending for retry, and refresh
  requests arriving during a pass coalesce into one additional pass.
- Every component-owned producer observes the scan gate. Refresh and due sensor
  polls remain pending; identify time is paused; console and diagnostic actions
  are rejected; HA light writes retain their latest desired target and retry after
  admission reopens. The one exception is the local-only console `queue` verb,
  which reads scheduler state without touching the bus. Headless events are
  drained so their fixed queue cannot overflow, but actions observed during a
  scan are intentionally dropped rather than replayed after their physical
  context is stale.
- Queue admission is observable. `dali_sched_queue_stats()` reports depth,
  capacity, high-water, admitted, and rejections split into queue-full and
  reset-barrier; ESPHome logs a warning whenever either rejection counter
  advances, and both CLIs expose a `queue` verb. A rejection is dropped work,
  since the scheduler never retries a refused submission for the caller.
- Identify, the diagnostic buttons, and headless dispatch all handle a refused
  enqueue: identify retries the same half-blink rather than advancing its phase,
  the buttons surface the rejection on the diagnostic text sensor, and a dropped
  dispatch action is logged as a warning distinct from an unmappable frame.
- A scan waits for the local scheduler to become quiescent before its first
  transaction. This is an integration-level gate, not a scheduler reservation:
  a future client that bypasses `DaliComponent` or another physical DALI master
  can still interpose.
- Only complete group discovery replaces and persists membership. Optional query
  failure or a missed previously known member retains the prior `DGP2` map and
  withholds generated YAML; HA reports group data as incomplete.
  `group forget <addr> [group]` retires a departed member from the cache without
  touching the bus; the scan's own retention stays conservative, which is right
  for gear that is merely offline.
- Matching Device/Instance events request an immediate authoritative sensor
  poll; event information is never published as a generic sensor value.
- A sensor reading is one scheduler sequence, so a two-byte instance cannot have
  its latching query and its latch read separated by other traffic. Admission is
  all-or-nothing, and a rejected poll simply retries on the next interval.
- Bus monitoring and Find Couplers retain and format the canonical Part 103
  source scheme, selectors, and full event information. The Find Couplers
  summary fits the 255-character Home Assistant limit and appends `+N more`
  rather than truncating silently; every captured frame is still logged in full.
- `bus_fault` separates current availability from cumulative history. The
  `tx_frames_ok` PHY counter is the recovery signal, since the fault counters
  only grow: the sensor publishes `Bus stuck (N total)` and returns to
  `OK (N past faults)` once a frame is clocked out in full.
- The GPIO schema is board-aware (`pins.internal_gpio_output_pin_number` /
  `..._input_pin_number`), rejects TX equal to RX, and rejects GPIO16/17 for the
  WROVER PSRAM restriction. DALI-task creation is checked and fails the
  component rather than running without a bus task.
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

`DaliCommandId` gains `DALI_CMD_CONTINUOUS_UP` and `DALI_CMD_CONTINUOUS_DOWN`,
appended before `DALI_CMD_COUNT` so every existing numeric value is unchanged.
`dali_command_lookup_opcode(DALI_CMD_FRAME_16BIT, ...)` now resolves opcodes
`0x0B` and `0x0C`, which previously returned NULL. `dali_frame.h` adds
`DALI_DAPC_MASK_LEVEL`; `dali_protocol.h` adds `dali_build_dapc_mask()` and
`dali_control.h` adds `dali_control_build_dapc_mask()` plus
`dali_control_build_continuous_up/down()`. The ordinary DAPC builders still
reject 255, so this is additive rather than a behaviour change.

`dali_gear_dt6.h` now includes `dali_scheduler.h` and `dali_input_config.h` now
includes `dali_scheduler.h`, for the new
`dali_dt6_build_command_sequence()`, `dali_dt8_build_command_sequence()`, and
`dali_input_build_config_sequence()`. A translation unit that included either
header only for its pure frame builders now also sees the scheduler types.

`dali_scheduler.h` adds `DaliSchedQueueStats`, `dali_sched_queue_stats()`, and
`dali_sched_reset_queue_stats()`. These are additive; existing callers are
unaffected. `dali_control.h` likewise adds `dali_control_set_level_cb()` and
`dali_control_off_cb()`, with the existing `dali_control_set_level()` and
`dali_control_off()` unchanged as their NULL-callback forms.

`DaliBusLight::flush_pending_write()` changes meaning: it now also collects the
scheduler completion for an in-flight command, so `DaliComponent::loop()` calls
it every loop rather than only outside a scan. An implementation must apply the
scan gate itself before admitting new traffic.

`DaliError` adds `DALI_ERR_TIMING = 8`, `DALI_ERR_CANCELLED = 9`, and
`DALI_ERR_INTERVENED = 10`. Existing earlier numeric values remain unchanged;
callers with exhaustive error handling should add the new scheduler results.

`dali_sched_reset()` is now a deferred compatibility wrapper around
`dali_sched_request_reset()`. A successful request means accepted, not complete;
the owner-task completion callback (or `dali_sched_reset_pending() == false`) is
the fence. Work discarded by reset completes with `DALI_ERR_CANCELLED`.

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

Dependent public executors require the new strict
`dali_transport_run_sequence_atomic()` contract. A transport must provide both
its normal `transact` callback and scheduler-backed `transact_sequence`; otherwise
the call returns `DALI_ERR_INVALID` before traffic. The ordinary runner retains
its stepwise fallback only for explicitly non-dependent callers.

`dali_gear_dt8.h` now includes `dali_transport.h`, which transitively pulls in
`dali_scheduler.h`. A translation unit that included only `dali_gear_dt8.h` for
its pure frame builders now also sees the scheduler and transport types.

## Installation State

Each site now exists twice: a tracked reference copy at the repo root and the
deploy copy under ignored `_local/`. Check the source column before reasoning
about what is running on a device — the pin, not the working tree, decides what
gets built.

| Configuration | Component source | Role |
|---|---|---|
| `dali_diag.yaml` | `ref: v1.0.1` | Tracked diagnostic/discovery firmware |
| `dali_1k.yaml` | `type: local` | Tracked reference copy of the first-floor site |
| `dali_2k.yaml` | `type: local` | Tracked reference copy of the second-floor site |
| `_local/dali-1k.yaml` | `ref: dev` | Deploy copy; 16 control gear and group entities for groups 0/2/3/4/5/6/7 |
| `_local/dali-2k.yaml` | `ref: dev` | Deploy copy; group 0 lighting, HA console, and Steinel HF 360 II polling |

The tracked copies use `type: local` with `path: esphome/components` so that a
configuration and the component it configures always agree within a commit. The
`_local` copies keep the git pin because they are what gets flashed and the
operator chooses when to move.

Two consequences of that split are live right now:

- The 1k site was flashed on 2026-08-12 from a temporary `type: local` edit that
  has since been reverted, so `_local/dali-1k.yaml` names `dev` while the
  hardware runs the working tree. The file does not describe what is deployed.
- `_local/dali-2k.yaml` sets `poll_on_event`, which `dev` does not yet contain,
  so it fails validation until that branch is pushed or it is pointed at
  `type: local`. It was deliberately not reflashed: the second floor is occupied
  at night and the brightness change is visible.

Both resolve when `dev` is pushed.

The entire `_local` directory is deliberately ignored by Git. This checkout also
contains `_local/dali-diag-local.yaml`, a compile-test copy of the tracked
diagnostic configuration. Inspect and back up any real site files separately;
normal `git status` does not show changes under `_local`.

`_local/secrets.yaml` was previously described here as dummy compile-only values
that must not be deployed. That is wrong and the description has been removed:
its six shared keys are byte-identical to the root `secrets.yaml`, and the 2k
device joins WiFi and accepts OTA using them, so both files hold live
credentials. Both are gitignored (`.gitignore:36` and `.gitignore:40`) and
neither is tracked. Treat both as real secrets.

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

- If new scheduler clients are added outside `DaliComponent`, add a true
  scheduler admission reservation for scans. The current quiescence check plus
  component-wide gate covers present local producers but is not an atomic
  check-and-reserve primitive.
- Give the remaining fire-and-forget scheduler clients the same
  confirmed-transmission treatment the light entities now have. Console `OK`,
  the refresh pump, and headless dispatch still report or act on admission
  rather than transmission; only light writes distinguish the two.
- Correct observed-state semantics for RECALL MAX, STEP DOWN AND OFF, and
  broadcast propagation to ordinary light entities. `dali_dispatch` asserts a
  state for two commands whose result it cannot know: RECALL MAX (and TOGGLE's
  on branch) claims level 254, which is wrong for any gear whose MAX LEVEL is
  reduced, and the legacy-observe path for STEP DOWN AND OFF claims off, which
  only holds if the gear was already at its minimum. Both should report unknown
  and let the deferred actual-level query answer, as RECALL MIN, the dim/step
  actions, and the console's level-changing verbs already do. Separately,
  `notify_lights()` matches a broadcast result only against broadcast entities,
  so ordinary short-address lights never see it.

### P1 — CLI verification on real hardware

The typed verb surface is in place; what is missing is evidence. Keep
`dali_capability_matrix.md` current as each row is exercised.

- Run the DT6 and DT8 verbs against real DT6/DT8 gear. Every DT8 row in the
  matrix is host-covered and hardware-unverified, and both the P2 mapping of DT8
  to Home Assistant colour traits and its absence from the ESPHome console are
  deliberately held behind this. The DT6 console verbs added on 2026-08-12 send
  the same frames as the native ones, so one hardware session clears both
  columns at once — start with `dt6 <a> dimming-curve` and
  `dt6 <a> failure-status` on the 1k site's LED drivers, where a wrong answer is
  visible rather than destructive.
- Complete the memory read/write/read-back cycle on hardware for both
  `memread`/`meminfo` (Part 102) and `devmem` (Part 103). No write path reads
  its value back today, so `devmem write` reports transmitted, not applied.
- Validate input-device configuration writes with read/write/read-back per
  parameter. `iconfig` is explicit that its success means transmitted; until
  this is done the whole surface stays experimental.
- Add host vectors for `identify`, `smoke`, `capture`, and the inventory JSON
  export, whose output formats are currently unasserted.
- The ESPHome console shares `dali_cli`'s tokeniser, argument parsers, named
  command tables, and reply decoding. `dali_cli.{c,h}` moved to
  `components/dali/` so both front ends can reach it, and
  `dali_cli_resolve_in()` resolves against a caller-supplied verb table — the
  console brings the subset that suits a Home Assistant text entity, and the
  native CLI keeps its own. Verified on hardware: nothing in this migration, or
  in the 2026-08-12 verb-parity work built on it, has been exercised on a real
  bus.

### P1 — ESPHome correctness and architecture

The correctness work previously tracked here is implemented and recorded under
"Current Software State" and the verification baseline; per the documentation
policy it is out of the backlog. The 1k site flash on 2026-08-12 is the first
real-bus exercise of any of it; the 2k paths — sensor polling, `poll_on_event`,
the Steinel workflows — remain unexercised. See "ESPHome console verb renames"
below for what changed for an operator.

- Extend the compact Part 103 dispatch key if a site needs to distinguish
  Device-Group from Instance-Group sources or match instance type; the canonical
  event/capture path retains these fields, but the five-field rule key does not.
  No site needs this today, and it changes a C struct layout that needs a
  documented migration.
- A paged or exportable Find Couplers result, rather than one truncated-with-a-
  count summary. The log already holds every frame.
- Define recovery after bus-only power cycles, beyond the current/cumulative
  fault split described under "Current Software State".
- The console's own dispatch has no host vectors. Its parsing, argument
  validation, and reply decoding are shared code that does, but the wiring
  between them in `dali_component.cpp` is ESPHome/FreeRTOS-bound and testable
  only on the device.

### P1 — Release and verification quality

- Add CI that compiles a clean external-component checkout from the release tag and
  validates the Python schema, ESPHome C++ layer, YAML, and wrapper packaging.
- Add the native ESP-IDF build to CI alongside the existing 26-suite host workflow.
- Keep one intentional ESPHome source-inclusion path. The unused component
  `CMakeLists.txt` is gone and the Python-copy/wrapper route is authoritative;
  what is left is the second candidate in `_protocol_source_dir()`,
  `esphome/components/dali/protocol/`, which no build in this repo exercises.
  Either make it a real packaging layout or drop it.
- Enforce or document the actual ESP-IDF and ESPHome version requirements; the
  current `manifest.json` is not enforcement for normal external components.
- Keep local filenames hyphenated in all documentation. `dali_command_reference.md`
  was synchronized with both verb surfaces on 2026-08-12; it needs re-checking
  whenever either table changes.
- Complete release provenance: project SPDX identifiers and the full vendored
  Unity MIT license/third-party notice.
- Document the next-release C migration before tagging it. Accumulated so far:
  the intentionally changed `DaliInputEvent` and `DaliDispatchKey` field
  names/layout; `dali_cli.{c,h}` moved from `main/` to `components/dali/`;
  `DaliCommandId` gained three enumerators before `DALI_CMD_COUNT`
  (`DALI_CMD_QUERY_DEVICE_CONTENT_DTR0/1/2`), so any persisted or wire-shared
  numeric command id is unaffected but `DALI_CMD_COUNT` itself moved;
  `dali_stats_t` gained `tx_frames_ok` at the end; `DaliCliCommandSpec` and
  `DaliCliInstanceConfig` gained fields, so brace-initialised tables outside
  this repo need updating. Additive since: `dali_control_continuous_up/down()`,
  `dali_cli_format_response()`, `dali_cli_format_status()`, and
  `dali_cli_special_is_commissioning()`. One output change comes with them —
  `dali_cli_print_response()` now prints a status byte's head line as
  `status: 0x04 arc-on` rather than `status: 0x04`, before the same per-field
  block, so anything scraping native CLI output for that exact line needs
  updating.
- Announce the ESPHome console verb renames in the release notes. They are the
  operator-visible half of the `dali_cli` adoption and have no aliases — the
  table is in `dali_command_reference.md` under "Verbs renamed when the console
  adopted the shared tables". Anything in Home Assistant that writes a command
  string to the `text:` entity (scripts, automations, dashboard buttons) needs
  updating. The target spelling is the one that breaks every stored command at
  once and is easiest to miss: `a<N>` is rejected as `bad target`, and a short
  address is now `s<N>` or a bare number. Beyond that, `memread`/`memwrite`
  became `devmem read`/`devmem write`, `iquery a0:1 x` became `iquery 0 1 x`,
  `query a0 actual-level` became `query s0 actual`, `config <t> <name> <dtr0>`
  became `config-dtr0`, and the Part 303/304 instance names took type prefixes
  (`hold-timer` → `occ-hold-timer`).
- Announce the console reply-format change alongside those renames. Every query
  reply is now named and decoded (`actual: 42 (0x2A)`, `present: yes (0xFF)`,
  `status: 0x06 lamp-fail,arc-on`) instead of being published as a bare
  `42 (0x2A)`, so an automation that parses `command_result` numerically breaks.
  This is the second half of the same migration and has the same audience.
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
- Improve scene/fade UX. A level-changing command now arms a deferred re-read,
  but the refresh it triggers is a full pass over every light entity rather than
  a query of the one that moved, and nothing reads a final value after a
  transition longer than the arming window.
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
- Build every new multi-frame workflow on `DaliSequence`. The atomic-transaction
  migration covers everything that exists today, and the strict transport runner
  and command retry-safety metadata enforce it, so what remains is a regression
  risk: issuing dependent frames as separate transactions reintroduces the class
  of bug this removed. The tell is a query whose answer depends on a register or
  enumeration pointer that the same query, or an immediately preceding frame,
  modifies.
- Do not use GPIO16 or GPIO17 on the WROVER-E target.

## Source Layout

| Path | Role |
|---|---|
| `components/dali` | Reusable C protocol, scheduler, PHY, discovery, dispatch, memory, dimming curve, and device-type stack |
| `components/dali/dali_cli.c/.h` | Portable CLI core: tokenising, verb table, validation, formatting. Shared by the native app and the ESPHome console |
| `components/dali/dali_dim_curve.c/.h` | IEC 62386-102 arc power level ↔ light output conversion |
| `main/main.c` | Native ESP-IDF diagnostic application entry point |
| `main/dali_diag.c/.h` | Device half of the serial CLI: task, transports, workflows |
| `esphome/components/dali` | Active ESPHome external component |
| `dali_diag.yaml` | Tracked diagnostic/discovery firmware |
| `dali_1k.yaml` / `dali_2k.yaml` | Tracked reference copies of the two site configurations |
| `_local/dali-diag-local.yaml` | Ignored compile-test copy of the diagnostic firmware |
| `_local/secrets.yaml` | Ignored, untracked, and holds live credentials — see Installation State |
| `_local/dali-1k.yaml` | Ignored first-floor site firmware |
| `_local/dali-2k.yaml` | Ignored second-floor site firmware |
| `dali_command_reference.md` | Protocol and command catalog |
| `dali_capability_matrix.md` | Per-capability API/verb/vector/hardware/ESPHome status |
| `esphome_verb_readme.md` | ESPHome console examples and notes |
| `steinel_bank2_reference.md` | Installation-specific Steinel memory observations |

## Build and Test Commands

ESPHome configuration/build:

```powershell
esphome config  dali_1k.yaml                 # tracked copies build the in-repo component
esphome config  dali_2k.yaml
esphome compile dali_diag.yaml
esphome compile _local/dali-1k.yaml          # deploy copies; whatever they pin
esphome compile _local/dali-2k.yaml
```

`esphome config` is the cheap check and is what to reach for first: it validates
the schema without touching a build directory, so it cannot collide with a
compile already running in another terminal.

To prove uncommitted component changes actually build, compile a tracked copy —
`dali_1k.yaml` and `dali_2k.yaml` resolve `type: local` against
`esphome/components`, so they compile the working tree by construction. Give a
throwaway copy a different `esphome: name:` if the real site's build directory
must not be disturbed. `_local/dali-diag-local.yaml` remains the diagnostic
equivalent, with `path: ../esphome/components` for its location.

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
- Keep protocol/command detail in `dali_command_reference.md`, and per-capability
  API/verb/vector/hardware status in `dali_capability_matrix.md`.
- Label claims as host-tested, hardware-verified, or still unverified.
- Do not add session-log TODO files; merge active work into the prioritized list
  above.
