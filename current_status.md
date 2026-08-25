# DALI-ESP Current Status

**Last updated:** 2026-08-25

**Known-working deployment/component baseline:** `v1.1.1` (`76cede1`), hardware
tested; this is not a conformance claim.

**Deployed now:** both sites run `v1.1.1`, so the last release known good and
what is on the hardware are the same statement again. The `_local` deploy copies
do not say so themselves; see Installation State.

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

### Verified on hardware 2026-08-14 (`v1.1.1`)

`v1.1.1` was flashed to both sites and exercised on the installed buses. This is
the first hardware result for the diagnostic shell and for the level-window work,
and it clears four items recorded as unverified under 2026-08-12:

- The diagnostic shell over TCP: discover, identify, live trace, rolling capture,
  and JSON export, driven from a terminal over the network front end rather than
  a serial cable.
- The per-short-address MIN/MAX and dimming-curve profile: the query, the
  physical-output interpolation for standard and linear curves, and the
  `min_level`, `max_level`, and `dimming_curve` overrides.
- The Home Assistant on-code floor at code 3: 1 % lands on MIN exactly, and
  codes 1-2 clamp onto it.
- `poll_on_event`: the 2k site has been reflashed, so the per-instance default
  has a hardware result rather than a measurement of the old behavior plus a
  compile of the new.

Unchanged by this pass: the DT6 and DT8 command sets, memory writes, and
input-device configuration writes still have host vectors and no real-bus
result. `dali_capability_matrix.md` states which is which per capability.

### Verified locally on 2026-08-25 (commissioning P0 slice)

- The PHY now exports task-context RX observations for decoded frames, malformed
  waveforms, and ring overflow, including wrapping first/last-edge timestamps and
  an edge count. The scheduler anchors the reply window to the ISR-captured TX bus
  release and attributes observations through 27 ms after that point, including
  replies decoded while the owner task still says `WAIT_SETTLE`. The open edge
  was 5.5 ms for every observation in this slice; it was later split per
  observation kind — see "1k bus: gear that replies just before the attribution
  window opens" for the two edges now in force.
- Commissioning COMPARE now has three outcomes instead of treating every missing
  byte as NO. Silence is NO; a decoded `0xFF` or qualified undecodable backward
  activity is YES; short/ambiguous malformed activity and overflow abort with an
  error. A decoded backward byte other than `0xFF` is malformed. Only COMPARE maps
  `DALI_ERR_RX_ACTIVITY` to YES; VERIFY and single queries propagate it.
- The short-address scan no longer aborts on `DALI_ERR_RX_ACTIVITY`. Propagating
  it out of a single query is right; propagating it out of the 0-63 walk meant one
  contested address failed the whole scan, which took the ESPHome boot scan (0
  devices, no group-membership rebuild) and `commission`'s own mandatory pre-scan
  down with it. Such an address is now recorded as `has_undecodable_activity`,
  counted in the new `undecodable_count`, and left `present = false`: not listed
  as a device, not offered to commissioning as free, and not fatal to the walk.
  Marking it absent instead would have been worse than aborting — the free-address
  mask would have handed a contested address to the next assignment. Real bus
  errors still abort. Host-tested only; the underlying classification still needs
  a physical collision capture.
- Every admitted commissioning opening now unwinds through one cleanup path. It
  attempts TERMINATE even if cancellation or a lost TCP peer prevents the normal
  waiter from knowing how far the opening sequence progressed. The device shell's
  dedicated cleanup transport bypasses front-end cancellation but preserves
  scheduler/bus failure; transports without that optional channel still record a
  failed attempt. The result keeps the primary operation error when both fail and
  records whether TERMINATE was transmitted or initialisation remains unknown.
- The reply window now opens at 5.5 ms rather than 7 ms. 7 ms is the nominal
  forward-to-backward settling time; attributing from the nominal timed out any
  compliant gear answering at the fast end of the range, which is a fault the
  pre-timestamp code did not have because it accepted any decoded backward frame
  in `WAIT_REPLY` regardless of arrival. A regression test pins a decoded reply at
  6 ms — inside the range, below the nominal — as accepted. The citation is still
  owed; see the P0 item.
- `dali_commissioning.c` no longer depends on FreeRTOS. The post-RANDOMISE settle
  comes from a new optional `DaliTransport::delay_ms`, and commissioning refuses a
  transport that supplies none before it transmits anything — a skipped settle
  otherwise presents as an empty bus rather than as an error. The module's header
  claim of no task dependency is true again, and the 100 ms settle now has a host
  vector asserting it happens exactly once with RANDOMISE as the last frame sent.
  The ESPHome scan transport deliberately supplies no wait: it never commissions,
  and the explicit rejection is preferable to a path that silently works.
- `dali_dispatch` no longer asserts levels it cannot know. RECALL MAX (both the
  action and the legacy-observe path) and TOGGLE's on branch report on-with-level-
  unknown instead of claiming 254, which was wrong for any gear with a reduced MAX
  LEVEL; STEP DOWN AND OFF reports unknown and leaves the toggle alone instead of
  claiming off, which only held if the gear was already at its minimum. OFF still
  reports level 0, which is exact on any gear. Operator-visible consequence: a
  wall switch driving RECALL MAX or STEP DOWN AND OFF through a coupler now
  updates its Home Assistant entity when the deferred query lands, roughly 600 ms
  later, instead of instantly at a value that could be wrong. RECALL MIN and the
  dim/step actions have always behaved this way, so this makes the set
  consistent rather than introducing a new lag. If the delay proves annoying in
  use, the fix is a "known on, level unknown" state in `DaliDispatchResult`
  rather than a return to asserting 254.
- `notify_lights()` propagates a broadcast result to every light entity. It
  previously required the entity's target type to equal the result's, so a
  broadcast matched only broadcast entities and ordinary short-address lights
  never saw it.
- `DaliError` reaches an operator by name on both surfaces. `dali_error.c` holds
  the single name table — `dali_error_name()` returns NULL for a code this build
  does not know, `dali_error_text()` renders that case as `error <n>` into a
  caller buffer — so no defined code prints as a bare number and an undefined one
  still carries its value. `DALI_ERR_RX_ACTIVITY`, which a commissioning run now
  produces routinely, reads as `rx activity` instead of `ERR 12` in the native
  shell and `err` in a Home Assistant text state. A host vector fails if a new
  enumerator is added without a name. Deliberately unchanged: `no reply` stays the
  ESPHome wording for a timeout, the shell keeps its `ERR` prefix ahead of the
  name so line shapes and greps survive, and the inventory JSON `query_error`
  field stays numeric. ESP_LOG lines still print numbers.
- The reply-error path now spends the retry budget on the codes that carry no
  meaning. `MALFORMED` (one unreadable waveform) and `OVERFLOW` (an event dropped
  because the RX ring filled, a local resource fault rather than a fact about the
  bus) decrement `retries_left` and re-arm the TX gap through the same
  `sched_retry_active_step()` the timeout branch uses; only commands whose
  response is retry-safe ever hold a budget, so this is exactly as safe as the
  timeout retry already was. `DALI_ERR_RX_ACTIVITY` deliberately does not retry:
  COMPARE reads it as YES, and a second attempt meeting silence would invert a
  correct YES into a NO. Previously a single blip anywhere in the reply window
  ended the step, and with it the sequence that owned it — for commissioning, the
  whole run. Host-tested for both retry codes, budget exhaustion, and the
  RX_ACTIVITY exclusion.
- Precedence between a decoded reply and later in-window noise is settled as it
  stood, and now says so in the source. Three mechanisms agree that noise wins
  regardless of arrival order — the guarded decoded-frame latch, the unconditional
  `s_reply_received = false` in `sched_latch_reply_error()`, and `SCHED_WAIT_REPLY`
  testing the error first — and the priority ladder applies the same fail-closed
  rule to error-versus-error. The reasoning is that an address answering cleanly
  while something else adds frame-like activity is genuinely ambiguous, most
  obviously two devices sharing a short address, and a confident single value
  would hide that. The known cost is recorded with it: for an ordinary query a
  spike at 19 ms discards a byte decoded at 8 ms. The retry split above softens
  it — that case now re-asks rather than failing outright.
- Part 103 quiescent mode is implemented end to end. `START`/`STOP QUIESCENT MODE`
  (`0x1D`/`0x1E`, instance byte `0xFE`, send-twice, no reply) are in the command
  table; `dali_input_build_quiescent_mode[_broadcast]()` build the frames; and
  `quiescent on|off <addr|all>` is a verb on both the native shell and the ESPHome
  console. The send-twice pair goes to the scheduler as one transaction rather
  than two enqueues, so nothing can land between the halves.
  `dali_build_device_broadcast_command()` closes the gap that made `all`
  impossible: address byte `0xFF` is not a short address, so it needed its own
  builder rather than a sentinel through `dali_build_device_command()`, which
  still rejects anything at or above 64. Both surfaces warn on `on`, because a
  quiesced device reports no events and nothing here tracks or releases the
  state — a forgotten `quiescent on all` presents as dead sensors. Host-tested
  for frame layout, the send-twice table flag, broadcast-versus-addressed
  distinctness across all 64 addresses, and argument rejection; no bus has run it.
  Whether the standard also ends the state on its own timer is not established
  here, so `off` is documented as the only reliable release.
- Gear commissioning brackets itself with broadcast quiescence. A send-twice
  START QUIESCENT MODE goes out before INITIALISE and a STOP follows TERMINATE on
  every exit path, so a conforming control device cannot put an event frame into
  a COMPARE reply window, where frame-like activity reads as YES and invents gear
  that is not there. Both live outside the opening `DaliSequence`, as its own
  transactions: `DaliSequenceStep` carries a frame and no duration, and the start
  sequence's atomicity exists to hold dependent pairs together rather than to
  carry unrelated steps.
  Decisions worth keeping: a failed START does not abort the run, because
  quiescence is hardening rather than a precondition and refusing to commission
  over it would trade a working operation for a risk the operator may not have;
  the release goes through the cleanup transport for the same reason TERMINATE
  does, so a cancelled run or a dropped TCP peer cannot be what leaves an
  installation's sensors silent; and `quiescence_started` is set from the
  transmit alone, separately from the settle that follows, because a settle
  failure after a successful transmit still leaves the bus quiesced and still has
  to be unwound.
  The settle before INITIALISE is `DALI_COMMISSIONING_QUIESCENT_SETTLE_MS`, 39 ms,
  and is deliberately not presented as a standards figure: it is two 24-bit frame
  times, long enough for an event frame already on the wire when START arrived to
  finish. If the standard specifies an entry time it is not read here, so the
  constant is bounded below by an argument that is checkable without it.
  `quiesce_control_devices` is off in a zero-initialized `DaliCommissioningOptions`,
  so an out-of-tree caller keeps its previous behaviour; the shell sets it and
  reports the two states that matter — a START that never went out, and a release
  that failed, which leaves control devices silent and names `quiescent off all`
  as the fix. Host-tested for ordering against INITIALISE and TERMINATE, the
  settle accounting, release on failure and on cancellation, both failure modes,
  and rejection before any traffic when the transport cannot wait. No bus has run
  it, and it cannot reach a device that never receives the broadcast.
- Spelling is now consistent, and the rule is which *thing* is being named
  rather than which dialect. The trigger was a real inconsistency: `special
  initialise` and `special randomize` sat on adjacent lines of the same CLI
  table.
  **DALI command names use the standard's `-ise` spelling.** IEC 62386 spells
  them INITIALISE and RANDOMISE, so matching the standard beats matching the
  surrounding prose — a name that differs from the spec is a name you cannot
  grep the spec for. This covers the command-table strings, both CLI verbs, the
  identifiers that denote a command or the protocol state one opens
  (`DALI_CMD_INITIALISE`, `DALI_CMD_RANDOMISE`, `dali_cmd_initialise()`,
  `dali_cmd_randomise()`, `DALI_INITIALISE_UNADDRESSED_PARAM`,
  `DALI_COMMISSIONING_RANDOMISE_SETTLE_MS`,
  `DALI_COMMISSIONING_START_STEP_INITIALISE`/`_RANDOMISE`,
  `DALI_COMMISSIONING_EVENT_INITIALISED`/`_RANDOMISED`,
  `DaliCommissioningResult::initialisation_state_unknown`), and every prose
  reference to the commands or to the fifteen-minute initialisation state.
  **Everything else is American.** `tokenize`, `recognize`, `quantize`,
  `normalize`, `serialize`, and ordinary software initialization —
  `Initialize the ring buffer`, `zero-initialized struct`, `s_initialized`,
  `main.c`'s "Initialization complete" — all take the `z`.
  Net effect on the operator surface: `special initialise` is unchanged from
  where it started, and `special randomize` became `special randomise`. The word
  migration itself was driven by an explicit list rather than a suffix regex, so
  `otherwise`, `raise`, `noise`, `precise`, `size`, and `advertise` were never
  candidates.
- Documentation cross-references audited. `dali_command_reference.md` was cited
  five times and `esphome_verb_readme.md` once; neither has existed since the
  2026-08-11 documentation split. Both are gone from the Source Layout table,
  which now also lists `test/`, `tools/`, `dali_commands.md`, `dali_protocol.md`,
  and `commissioning_readme.md`. The Documentation Policy and the
  hyphenated-filenames item point at the files that exist. One stale path fixed
  in prose: `dali_cli.c` has lived in `components/dali/` since the console
  adopted it, not `main/`. Every remaining `.md` file reference in the tree now
  resolves; `README.md` and `AGENTS.md` were already correct.
- The four GitHub workflows were reviewed and need no change. Verified rather
  than assumed: `actions/checkout@v7`, `actions/setup-python@v7`, and
  `actions/cache@v6` are all current majors; ESP-IDF `v6.0.1` is a real tag, and
  the pin is deliberate because it matches the tracked sdkconfig header, though
  `v6.0.2` now exists in that line; ESPHome 2026.8.1 requires Python
  `>=3.12,<3.15`, so the workflows' 3.12 is valid; the six dummy secrets CI
  writes cover the five `dali-starter.yaml` actually uses; and `test/build/` is
  gitignored, so no workstation CMake cache can leak into a run. No workflow
  hardcodes a suite or module count that `dali_error.c` would have invalidated.
- All 26 host suites pass. Focused totals are PHY 28, scheduler 78, transport 14,
  discovery 56, commissioning 32, and dispatch 31. Native ESP-IDF 6.0.1 and ESPHome 2026.7.4
  builds pass. These are host and compile results only: no COM6, flash, captured
  collision waveform, or commissioning run was used in this pass.
- Multi-device commissioning therefore remains restricted to the documented
  single-unaddressed-device envelope until HIL proves overlapping replies. Part
  103 quiescence, equal-random-address recovery, external-master arbitration, and
  the 100 ms post-RANDOMISE value still need standards/hardware validation.

### Verified locally on 2026-08-25 (reconfiguration surface)

Prompted by a documentation gap: group membership had no obvious verb, and
`commission` only ever addresses gear that has none, so nothing described how to
change a bus that already works. Writing that section surfaced three defects,
each an inconsistency between the two command surfaces rather than a protocol
fault.

- `set-short-address-dtr0` is now gated as a commissioning command.
  `allow_commissioning` covered `commission` and the nine `special` primitives
  and nothing else, so `special program-short` was refused from the Home
  Assistant text entity while `config-dtr0 b set-short-address-dtr0 255` — which
  de-addresses an entire installation — was accepted there with
  `allow_commissioning: false`. The gate refused the harder spelling of
  re-addressing and permitted the easier one. `dali_cli_config_is_commissioning()`
  now names it; the shell requires `DALI_SHELL_ALLOW_COMMISSION` and the console
  refuses it outright, matching the specials. Both spellings are covered: with
  DTR0 already holding `0xFF`, plain `config <t> set-short-address-dtr0` does the
  same thing as the DTR0 form.
- Broadcast group edits are refused in one place. The console rejected
  `config b add-group <g>`; the shell accepted it after a generic multi-target
  warning — a front end gating a verb, which is what the architecture rule in
  `AGENTS.md` exists to prevent. `dali_cli_config_rejects_broadcast()` and
  `DALI_CLI_MSG_NO_BROADCAST_GROUP` now hold the rule and its wording, and both
  surfaces report it identically. `raw2` remains the deliberate way to send the
  frame.
- A shell session now tells the component what it changed.
  `dali_shell_tcp.cpp` bound `inventory_changed` to `nullptr`, so a shell
  `discover` or `commission` never rebuilt the group-membership table, and a
  `config` verb reached no hook at all — a group edit typed into the shell was
  correct on the bus while a group light went on polling the member it had
  before. `DaliShellHooks` gained `config_applied`, the ESPHome binding
  implements both hooks, and `DaliComponent::on_config_applied()` is now the one
  place deciding what a config command invalidates, called by the console path
  too. `DaliComponent::apply_inventory_snapshot()` holds the group rebuild that
  was private to `dali_scan.cpp`, so the button scan and a shell walk publish
  through the same code. `commission` publishes its post-scan inventory rather
  than only the pre-scan one.
- Cache writes Core 0 owns are deferred rather than made from the session task:
  `external_profile_forget_mask_` is set by whichever task ran the edit and
  drained by `loop()` ahead of `pump_refresh()`, the same shape
  `external_refresh_request_` already had.
- A short address that moved is reported, not guessed. The new address is not
  knowable from the command — the DTR0 form carries it out of band and the plain
  form consumes whatever DTR0 held — so caches keyed by the old address are
  dropped and a warning says a scan is needed. Inventing a poll target would be
  worse than admitting the gap.
- Two host vectors added in `test/test_cli.c` assert both predicates by name and
  by set size, so a later config name cannot join or leave either set silently.
  All 26 host suites pass. `dali_shell.c`, `dali_cli.c`, and `main/dali_diag.c`
  compile clean against the ESP-IDF 6.0.1 flag set; `dali_component.cpp`,
  `dali_scan.cpp`, and `dali_shell_tcp.cpp` compile clean against the ESPHome
  flag set.
- Host- and compile-verified only. No bus has run any of it: the group-edit
  round trip, the console refusals, and the post-commission inventory publish all
  need a hardware pass. `Configuration commands (19 names)` still reads `partial`
  on the real-bus column and `DTR0-consuming configuration` still reads `no`.

### Verified on hardware 2026-08-25 (group representative discovery)

A group light needs one member's short address to poll, because QUERY ACTUAL
LEVEL addressed to a group collides the moment the group has two members. That
representative had to be written into the YAML as `query_address` — a fact about
the bus kept in a file the ESP32 never sees, which goes stale silently the day
the fixture is replaced. The bus can answer the question itself.

- `DaliComponent` gained a cold-start group seed sweep. When no membership
  snapshot is restored from flash, it walks short addresses running
  `dali_discovery_build_groups_sequence()` — QUERY GROUPS 0-7 / 8-15 — and seeds
  `s_group_map` with what answers, stopping as soon as every group-type light
  entity has a representative. Armed in `setup()` under exactly the condition
  that used to select the YAML seed, so a commissioned installation never pays
  for it.
- It seeds rather than publishing a snapshot. The result is explicitly
  unverified, so a scan supersedes it exactly as it supersedes a hand-written
  seed and `dali_group_map_scan_covers_known_members()` keeps its meaning.
  Seeding only addresses that actually answered is also safer than the YAML
  form, which could name an address that does not exist and would then block
  every future scan snapshot.
- Nothing about it blocks. `pump_group_seed_sweep()` is a state machine advanced
  by whichever loop pass observes the previous answer, shaped like
  `pump_refresh()` and sharing `s_refresh_query_in_flight_` with it so there is
  one component-issued query on the bus at a time. It yields to `scan_running_`,
  which is the same gate a shell workflow claims, and re-derives what it still
  wants after each completion so a scan or a console `add-group` landing
  mid-sweep disarms it within one address.
- Nothing is persisted from it: only a scan or an explicit edit writes flash.
  A cold node therefore re-derives on each boot until its first real scan, which
  is the same lifecycle the YAML seed had.
- `query_address` stays, as an override rather than a required seed. A
  `broadcast` entity still needs it — "everyone" has no member to derive — and
  pinning a particular member (a plain lamp rather than one sharing an input
  device) is a judgement no sweep can make. `dali_test.yaml` keeps it so the
  override stays under test; the README example and `_local` site configs no
  longer need it.
- `export config` no longer drafts a `query_address` into the group entities it
  proposes. It names one member in a comment instead, so the export cannot
  reintroduce the hardcode it was just used to remove.
- **Run on the 1k bus with every `query_address` removed from the YAML, and it
  did the job.** With no seed and no snapshot in flash it seeded groups 0, 2
  and 6 from the bus; the refresh then polled them via a5, a4 and a2, which are
  the lowest member of each group and so exactly what `dali_group_map_pick()`
  should return. It reported `group mask 0x00B8` — groups 3, 4, 5, 7 — as
  unseedable, which two `discover` runs appeared to confirm: a1 did not answer
  at all, and a0, a13, a15 answered QUERY STATUS but returned no group data.
  A third run then read all sixteen devices including `a13 groups=[3]` and
  `a15 groups=[7]`, so the correct reading is that this gear is intermittent
  and the sweep caught it on a bad run — not that those groups are empty. See
  Installation State.
- That is an installation finding, not a firmware one, and it is still the case
  for the feature: those four groups had `query_address` pointing at gear that
  answers unreliably, so their state readback was unreliable too, and nothing
  said so. The hardcode was not providing a dependable reading, only hiding
  that the reading was in doubt. How much of seven days of Home Assistant
  history for those entities is bus-confirmed and how much is optimistic
  command state cannot be separated after the fact.
- One real defect, in observability rather than behaviour: the per-address
  `ESP_LOGD` lines and the arming `ESP_LOGI` are emitted within the first
  fractions of a second of `loop()`, before a network log viewer attaches, so
  on a device watched over the API the entire walk is invisible and only the
  closing line survives. That was enough to make a working sweep look like a
  total failure for two rounds of diagnosis. The close now always prints
  `walked N, M answered, K silent` plus the last failing address, error and
  step, so the summary alone distinguishes an empty bus from a broken query
  path. Per-address detail still needs a serial capture.
- The cost model is still unmeasured. The visible window did not contain the
  start of the walk, so no timing can be read off it; the worst case — a
  configured group with no gear, forcing the full 0-63 walk with a reply
  timeout per step — has not been timed. On this bus the walk does run to 63,
  because four groups went unseeded, and boot was not visibly delayed.
- Open design question the hardware raised: a single pass asks each address
  once, and the query sequence's own retries were not enough to catch
  intermittent gear. Re-asking non-answering addresses while groups are still
  wanted would fix it at the cost of boot traffic. Not implemented — it needs
  a count of how often a pass actually misses, which one boot cannot give.

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

Not verified as of that date, and worth stating plainly (the first three are
cleared by the 2026-08-14 entry above):

- The 2k site has not been reflashed, so `poll_on_event` has no hardware result.
  Everything above about the poll rates is measurement of the old behavior plus
  a compile of the new.
- ESPHome now acquires and caches a per-short-address MIN/MAX/curve profile for
  refreshes, uses physical-output interpolation for standard and linear curves,
  and accepts `min_level`, `max_level`, and `dimming_curve` overrides. The
  profile query and HA mapping are host-tested and compile in a local ESPHome
  fixture, but have no hardware result yet.
- The Home Assistant on-code floor moved from brightness code 1 to code 3. The
  percent slider emits `round(255 * pct / 100)`, so code 3 is the lowest it can
  produce and codes 1-2 survive only in an explicit `brightness:` service call.
  With MIN pinned to code 1 the gear's floor rendered as 0 % — indistinguishable
  from OFF — and the slider could not return to it: on the 85..254 window both
  sites report, 1 % landed on level 106, leaving levels 85-105 unreachable from
  the UI. Codes 3..255 now span the window, so 1 % is MIN exactly and codes 1-2
  clamp onto it. The cost is two codes of resolution: 50 % and 100 % do not move
  and 10 % shifts by two levels. This is also what finally makes the "a
  requested 1 % now sends level 85" claim above literally true rather than
  approximately so. Host-tested in `test_light_profile`; no hardware result yet.
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
  typed line means is portable and host-tested. `components/dali/dali_cli.c` owns
  tokenizing, the verb table, argument validation, the named command tables, and
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
  every declared keyword is recognized and appears in the usage line.
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
  collision/activity plumbing is now host-tested as described in the 2026-08-25
  slice; real-bus collision waveforms and arbitration remain open below.
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
  (unaddressed), RANDOMISE. INITIALISE and RANDOMISE are send-twice, so three
  logical steps become five forward frames. No step retries, because a repeated
  RANDOMISE would hand out a fresh set of random addresses.
- That grouping also closed a leftover-state bug. If INITIALISE went out and
  RANDOMISE then failed, the old code returned the error without a TERMINATE,
  leaving the gear in initialisation state for the full fifteen minutes with
  nothing on the bus aware of it. The 2026-08-25 cleanup now conservatively
  issues TERMINATE after every admitted opening sequence, even when cancellation
  leaves local progress unknown; a queue-admission failure needs no cleanup.
  Fault-injection vectors cover operation and cleanup failures independently.
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
  the query step. A reply-window timeout on COMPARE or VERIFY is a negative answer
  with `DALI_OK`; a failure on any earlier step is returned as the error it was.
  Qualified malformed activity on COMPARE is YES, while an ambiguous waveform,
  overflow, or a decoded byte other than `0xFF` aborts rather than becoming NO.
  Previously a failed SEARCH ADDR write or dropped collision could walk the
  binary search past a real device. Independent host vectors cover these cases.
- The software side of the COMPARE collision inversion is therefore closed, but
  the classifier has not seen a captured overlapping `0xFF` response on hardware.
  Commissioning remains supported only with one unaddressed device until that HIL
  result exists; Part 103 event quiescence is also still absent.
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
- Dispatch is one table. `dali_cli_resolve()` tokenizes, looks the verb up, and
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
- `dali-starter.yaml` provides discovery and diagnostics, including scan, identify,
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
- The ESPHome TCP diagnostic shell exposes the shared guarded `commission` verb
  only when `allow_commissioning: true`; the Home Assistant text command surface
  still blocks raw commissioning primitives and has no dedicated workflow entity.
- `esphome/dali_esphome.h` is an unused legacy placeholder.

### Next-release API migrations

The RX-observation and cleanup work changes public source interfaces. External
callers must update `DaliPhyRxCallback` to receive `DaliPhyRxObservation`;
`DaliSchedOps` appends `get_last_tx_end_us`, and `DaliTransport` appends
`transact_cleanup`. `dali_stats_t` gains `reply_rx_activity`,
`DaliCommissioningResult` gains termination/cleanup state, and `DaliError` gains
`DALI_ERR_RX_ACTIVITY`. Existing designated initializers remain valid for the
appended optional callbacks, but positional initializers and callback adapters
must be rebuilt and reviewed.

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
`transact` and `ctx`: designated initializers are unaffected, but any positional
initializer must be updated. `DALI_MEMORY_QUERY_RETRIES` is removed, because
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

Sites live only under ignored `_local/` now. What the repo root tracks is about
the component rather than about a site. Check the source column before reasoning
about what is running on a device — the pin, not the working tree, decides what
gets built.

| Configuration | Component source | Role |
|---|---|---|
| `dali-starter.yaml` | `ref: dev` | Tracked starter/commissioning firmware; node `dali-starter` |
| `dali_test.yaml` | `type: local` | Tracked CI coverage config; fictitious layout, never flashed |
| `_local/dali-1k.yaml` | `ref: v1.0.5-dev` | Deploy copy; 16 control gear and group entities for groups 0/2/3/4/5/6/7 |
| `_local/dali-2k.yaml` | `ref: v1.0.5-dev` | Deploy copy; group 0 lighting, HA console, and Steinel HF 360 II polling |

The tracked site copies `dali_1k.yaml` and `dali_2k.yaml` are gone. Tracking a
real deployment to obtain CI coverage was the wrong trade: they carried an
address layout nobody else could use, and they had to be edited whenever the
site changed. `dali_test.yaml` replaces them and keeps the property that
mattered — `type: local` with `path: esphome/components`, so a configuration and
the component it configures always agree within a commit — while owing nothing
to any hardware. The `_local` copies keep the git pin because they are what gets
flashed and the operator chooses when to move.

### 1k bus: gear that replies just before the attribution window opens

Recorded 2026-08-25 from three `discover` runs, a `capture export`, and the
group seed sweep. Sixteen control gear; four of them answer unreliably, and the
split follows the fixture type rather than the address.

| Group | Entity | Gear | Behaviour |
|---|---|---|---|
| 0, 2, 6 | `... siin` (ceiling) | a2-a12, a14 | Answer everything, every run: `LED`, v4, `groups=[N]` |
| 3 | Elutuba ledriba | a13 | Was intermittent; **fixed** by the decoded-frame window, polls via a13 |
| 7 | Köök ledriba | a15 | Was intermittent; **fixed**, polls via a15 |
| 4 | Söögituba ledriba | a1 | Was intermittent; **fixed**, polls via a1 |
| 5 | Väike koridor | a0 | **Still failing.** Answers status, version and actual level; never answers device type or groups, in any run |

Every unreliable address is a LED-strip driver or the corridor fixture; every
solid one is a ceiling fixture on a v4 DT6 driver. **They are intermittent, not
mute**: run 3 read all sixteen, including the group membership of a13 and a15
that the first two runs missed entirely. An earlier reading of this table as
"answers commands but not queries" was wrong, and so was the conclusion drawn
from it that those groups were empty.

**Root cause, from two narrow captures of `query a13 groups-0-7`.** The device
is not intermittent in any useful sense: it replies correctly every time, and the
controller discards most of those replies.

```text
tx 0x1BC0           rx 0x08  since_tx_us 12742   rejected
tx 0x1BC0 (retry)   rx 0x08  since_tx_us 12936   rejected  -> "timeout"
tx 0x1BC0           rx 0x08  since_tx_us 13120   accepted  -> "0x08"
```

`sched_observation_in_reply_window()` tests the observation's **first** edge
against `DALI_REPLY_WINDOW_OPEN_US` (5500 us), while the `since_tx_us` in a
trace or capture record is its **last** edge. A backward frame spans nine bit
periods, 7500 us, which the data confirms independently: the accept/reject
boundary lies between 12936 and 13120, so the span is 12936..13120 minus 5500,
i.e. 7436-7620. Converting each observation to its first edge gives 5242 us and
5436 us for the two rejected replies against 5620 us for the accepted one.

**a13 settles in roughly 5.24-5.62 ms, straddling the 5.5 ms window open.**
The ceiling drivers settle at 6.4-7.0 ms and clear it every time, which is why
the failure follows fixture type rather than address. IEC 62386-101 gives 5.5 ms
as the *minimum* settling time, so this gear is marginally out of spec — fast by
up to 5% — and the controller enforces that minimum strictly enough to make four
of sixteen fixtures unreadable.

This also explains the `rx_ignored_outside_reply` deltas the scan reports (58,
77, then 113 across three runs). They are these early replies being discarded,
counted once per attempt including retries. Not noise, and not a trailing
artefact of the backward frame; an earlier note in this file said both and was
wrong.

**The bus is otherwise healthy and electrically quiet.** Replies that do land
arrive 6.4-7.4 ms after TX bus release, dead centre of the 5.5-10.5 ms range with
its 7 ms nominal, and the 27 ms close is nowhere near being approached. A capture
holding the tail of a scan across addresses 38 through 63 — twenty-six
consecutive empty addresses, each probed as both control gear and Part 103
control device — contains zero RX records. No noise, no ringing on an idle line.

Consequences worth holding:

- Retries do not help, and cannot: the gear is not randomly failing, it is
  sitting on a threshold with roughly 200 us of jitter, so most attempts fall the
  same side of it. This is why the group seed sweep's per-sequence retries did
  not rescue it, and why two `discover` runs disagreed.
- Those four group entities have unreliable state readback. How much of their
  Home Assistant history is bus-confirmed and how much is optimistic command
  state cannot be separated after the fact.
- `capture` holds `SHELL_CAPTURE_MAX` = 128 records against a `discover` that
  generates roughly 1900, so a full scan can never be captured — only its tail.
  Characterising one device needs a narrow capture around a single query. Both
  captures that settled this were six records long.

**Remedy, applied 2026-08-25: a second open edge for decoded frames.**
`DALI_REPLY_WINDOW_OPEN_DECODED_US` now applies to an observation that decoded as
a complete 8-bit backward frame; `DALI_REPLY_WINDOW_OPEN_US` (5500 us, the
standard's minimum) still applies to everything else. It is derived from
`DALI_SETTLE_MS` (2000 us) rather than chosen — see below for why a hand-picked
margin does not survive contact with this bus.
`sched_observation_in_reply_window()` and
`sched_observation_can_match_active_reply()` take the distinction as a parameter,
and only the decoded-backward-frame branch of
`dali_sched_notify_rx_observation()` passes it.

The asymmetry is where the safety lives. The strict edge exists to stop
undecodable activity being read as a reply, because COMPARE maps qualified
activity to YES and so invents gear that is not there — the one error a
commissioning walk must not make. A fully decoded backward frame arriving while
a query is outstanding carries no such ambiguity: our own 16-bit transmission
cannot decode as one, ringing cannot, and another master's forward frame is
caught by the intervening-frame branch. What is left is the PHY's RX self-echo
suppression, which is exactly `DALI_SETTLE_MS`.

It was first set to a hand-picked 4500 us, and a0 overtook it within the hour —
see below. Deriving it means there is no margin left to chase.

Two host vectors in `test/test_scheduler.c` pin both halves: a decoded frame at
the measured 5242 us is accepted with no `rx_ignored_outside_reply`, and
malformed activity at the same 5242 us is still rejected and still times out.
`test_timestamped_phy_callback_rejects_early_and_late_backward_frames` moved to
the decoded edge, since that is the boundary that now applies to it. All 26
suites pass; `dali_scheduler.c` compiles clean against the ESP-IDF flag set.

**Hardware result, same day: three of the four fixed.** The seed sweep's
unseeded mask went from `0x00B8` to `0x0020` — group 5 alone — and the refresh
now polls group 3 via a13, group 4 via a1 and group 7 via a15, all three of which
had never had a bus-confirmed reading. Home Assistant published real states for
them off that readback. The sweep summary read
`walked 64 address(es), 15 answered, 49 silent`.

**a0 is the one holdout, and it does not look like the same fault.** The bus has
16 devices in 64 addresses, so 49 silent is 48 empty addresses plus exactly one
device; the six group representatives plus nine other gear account for the 15
that answered, which leaves a0. It is not intermittent in the way the strip
drivers were: across three `discover` runs it has answered QUERY STATUS, QUERY
VERSION and QUERY ACTUAL LEVEL every time it was present, and answered QUERY
DEVICE TYPE and QUERY GROUPS never — including the run where a13 and a15 both
reported their groups. A selective failure by opcode is a different shape from a
settling time sitting on a threshold.

**Resolved by a six-record capture, and it was neither candidate cleanly.** With
`status a0` and `query a0 groups-0-7` in the same capture:

```text
tx 0x0190  rx 0x00  since_tx_us 11756   settling 4.26 ms   rejected
tx 0x0190  rx 0x00  since_tx_us 11622   settling 4.12 ms   rejected -> "timeout"
tx 0x01C0  rx 0x20  since_tx_us 13348   settling 5.85 ms   accepted -> "0x20"
```

`0x20` is bit 5: **a0 is in group 5**, the entity is correctly configured, and
the group is not empty. The never-on history was a red herring. A later
`query a0 device-type` returned `6`, so it is DT6 as expected, and that is the
third opcode it had "never" answered.

So the apparent selectivity by opcode was coincidence. a0's settling time varies
from 4.12 to 5.85 ms on the same device between consecutive queries, straddling
whatever edge is set — which is what made the failures look like they followed
the command rather than the clock. At 4.12 ms it is 25% faster than the
standard's minimum. This is what moved `DALI_REPLY_WINDOW_OPEN_DECODED_US` from
a chosen 4500 us to the derived `DALI_SETTLE_MS`; any fixed margin is one sloppy
driver away from being overtaken.

Not yet re-run on the bus with the derived edge — and it may not need to be for
this installation. On the 4500 us build all seven groups now read from the bus:
group 0 via a5, 2 via a4, 3 via a13, 4 via a1, 5 via a0, 6 via a2, 7 via a15,
with no `query_address` anywhere in the YAML. a0 answered on that build too;
its 4.12 ms samples would still be rejected there, so the derived edge is about
making it dependable rather than possible.

That boot ran no seed sweep, correctly: the membership had been restored from
flash. Which is itself evidence the window change worked, because
`dali_discovery_inventory_has_complete_group_data()` returns false if any present
control gear lacks group data, and `set_group_membership_snapshot()` refuses an
incomplete snapshot. Before the change no `discover` on this bus could ever be
complete — a0, a1, a13 or a15 were always missing some — so nothing was ever
persisted and the sweep armed every boot. One complete run afterwards persisted
a verified map and the sweep has had nothing to do since. The cold-start seed is
a fallback this node has now outgrown, which is the intended shape.

A consequence for verification: the sweep will not re-run here without a flash
erase, so the "16 answered" reading is not obtainable on this node. The verified
map supersedes it anyway.


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

- Complete and prove bus timing beyond the local own-forward-frame guard. Precise
  TX-end and backward/malformed observation timestamps are now exported and
  host-tested; HIL must validate both attribution edges (5.5 ms undecodable,
  2 ms decoded) against the 27 ms close, physical collision behavior, and
  external-frame cases. DALI-2 priority/backoff remains open, as
  does a deadline-aware PHY call when a repeat would cross the 100 ms limit.
- Validate the COMPARE collision fix on hardware. Qualified undecodable activity
  in the reply window now reaches commissioning as `DALI_ERR_RX_ACTIVITY` and
  counts as YES; ambiguous activity or overflow fails closed instead of becoming
  silence. Host vectors cover the plumbing and prevent a phantom NO, but no
  overlapping backward-frame capture proves the heuristic yet. Keep the
  single-unaddressed-device warning until HIL exercises real collisions.
- Cite `DALI_REPLY_WINDOW_OPEN_US` from the standard text. It was lowered from
  7,000 to 5,500 us on 2026-08-25: 7 ms is the nominal forward-to-backward
  settling time, and attributing from the nominal rather than from the range
  minimum would have timed out any compliant gear answering at the fast end. The
  5.5 ms figure has the same provenance problem the 7 ms figure had — it is a
  recollection of the IEC 62386-101 range, not a reading of the clause here — so
  the citation is still owed even though the direction of the change is the safe
  one. Attributing early is recoverable; attributing late discards real replies.
  Guard against the regression this closed: before timestamped attribution, any
  decoded backward frame in `WAIT_REPLY` was accepted regardless of arrival time,
  so too high a value presents as gear that used to answer going quiet. Only
  this constant is owed a citation: `DALI_REPLY_WINDOW_OPEN_DECODED_US`, added
  the same day for observations that decoded as a complete backward frame, is
  derived from `DALI_SETTLE_MS` rather than quoted from anything.
- Confirm the post-RANDOMISE settle time against the standard text, then verify a
  commissioning run on a bus. `DALI_COMMISSIONING_RANDOMISE_SETTLE_MS` was raised
  from 15 ms to 100 ms on 2026-08-24 on the strength of Espressif's `esp_dali`
  citing IEC 62386-102 §11.3 for the time gear needs to generate a 24-bit random
  address; the clause has not been read here, and no commissioning run has
  happened since the change. If 15 ms was genuinely too short, gear could have
  been unsearchable at the first COMPARE, which presents exactly like the former
  silence/collision inversion — so a visible improvement here would look like a
  partial fix for that and would not be one.
- Strengthen commissioning collision handling, equal-random-address recovery, and
  the separation of gear and control-device address spaces.

### P0 — Transaction and runtime reliability

- If new scheduler clients are added outside `DaliComponent`, add a true
  scheduler admission reservation for scans. The current quiescence check plus
  component-wide gate covers present local producers but is not an atomic
  check-and-reserve primitive.
- Give the remaining fire-and-forget scheduler clients the same
  confirmed-transmission treatment the light entities now have. Console `OK`,
  the refresh pump, and headless dispatch still report or act on admission
  rather than transmission; only light writes distinguish the two.

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
- The ESPHome console shares `dali_cli`'s tokenizer, argument parsers, named
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

- Keep one intentional ESPHome source-inclusion path. The unused component
  `CMakeLists.txt` is gone and the Python-copy/wrapper route is authoritative;
  what is left is the second candidate in `_protocol_source_dir()`,
  `esphome/components/dali/protocol/`, which no build in this repo exercises.
  Either make it a real packaging layout or drop it.
- Enforce or document the actual ESP-IDF and ESPHome version requirements; the
  current `manifest.json` is not enforcement for normal external components.
  Half-answered: `idf-build.yml` pins IDF 6.0.1, so the native requirement is now
  stated somewhere that fails when it stops holding. The ESPHome side is still
  advisory — CI installs whatever `pip install esphome` resolves to, so
  `manifest.json`'s `esphome_version: ">=2026.6.0"` is tested against the latest
  release and against nothing else. Note also that the two builds compile the same
  `components/dali` C with different toolchains: the native build uses IDF 6.0.1,
  ESPHome uses whatever its `framework: type: esp-idf` pins through PlatformIO.
- Keep local filenames hyphenated in all documentation. `dali_commands.md` is
  the file that has to track both verb surfaces; it needs re-checking whenever
  either table changes.
- Complete release provenance: project SPDX identifiers and the full vendored
  Unity MIT license/third-party notice.
- Document the next-release C migration before tagging it. Accumulated so far:
  the intentionally changed `DaliInputEvent` and `DaliDispatchKey` field
  names/layout; `dali_cli.{c,h}` moved from `main/` to `components/dali/`;
  `DaliCommandId` gained three enumerators before `DALI_CMD_COUNT`
  (`DALI_CMD_QUERY_DEVICE_CONTENT_DTR0/1/2`), so any persisted or wire-shared
  numeric command id is unaffected but `DALI_CMD_COUNT` itself moved;
  `dali_stats_t` gained `tx_frames_ok` and `reply_rx_activity` at the end;
  `DaliPhyRxCallback` changed signature — it now takes a `DaliPhyRxObservation *`
  rather than a `DaliFrame *`, which breaks any out-of-tree PHY consumer at
  compile time; `DaliSchedOps` gained the optional `get_last_tx_end_us`;
  `DaliTransport` gained the optional `transact_cleanup` and `delay_ms` — and
  `dali_commissioning_commission_unaddressed()` now *requires* `delay_ms`,
  returning `DALI_ERR_INVALID` without transmitting when it is absent, so an
  out-of-tree transport that commissions must supply one; `DaliError` gained
  `DALI_ERR_RX_ACTIVITY = 12`, so any switch over it outside this repo needs a
  new arm; `DaliDiscoveryDeviceInfo` gained `has_undecodable_activity` and
  `DaliDiscoveryInventory` gained `undecodable_count`, changing both layouts; `DaliCliCommandSpec` and
  `DaliCliInstanceConfig` gained fields, so brace-initialized tables outside
  this repo need updating. Additive since: `dali_control_continuous_up/down()`,
  `dali_cli_format_response()`, `dali_cli_format_status()`, and
  `dali_cli_special_is_commissioning()`. One output change comes with them —
  `dali_cli_print_response()` now prints a status byte's head line as
  `status: 0x04 arc-on` rather than `status: 0x04`, before the same per-field
  block, so anything scraping native CLI output for that exact line needs
  updating.
  Also new: `components/dali/dali_error.c` is an additional translation unit, so
  an out-of-tree build that lists sources by hand must add it or fail to link
  `dali_error_name()`/`dali_error_text()`. Its second operator-visible output
  change: every error a shell, native CLI, or Home Assistant `command_result`
  reports is now a name rather than a number — `status: intervened` where the
  CLI printed `status: ERR 10`, `commission: ERR rx activity` where the shell
  printed `commission: ERR 12`, and `rx activity` where the text entity
  published `err`. Anything matching on those strings or parsing the number out
  of them needs updating; this belongs in the release notes next to the console
  reply-format change, which has the same audience.
  `DaliCommandId` gained `DALI_CMD_START_QUIESCENT_MODE` and
  `DALI_CMD_STOP_QUIESCENT_MODE` before `DALI_CMD_COUNT`, and `DaliCliCommandId`
  gained `DALI_CLI_CMD_QUIESCENT` before `DALI_CLI_CMD_COUNT`, so both counts moved
  again and any out-of-tree switch over the CLI ids needs a new arm. Additive:
  `dali_cmd_device_broadcast()`, `dali_build_device_broadcast_command()`, and
  `dali_input_build_quiescent_mode[_broadcast]()`.
  The spelling work adds one operator-visible break, smaller than it could have
  been: `special randomize` is now `special randomise`, with no alias, so a
  stored Home Assistant command using the old spelling is rejected as an unknown
  special. `special initialise` is unchanged. On the C side `DALI_CMD_RANDOMIZE`
  became `DALI_CMD_RANDOMISE` and `dali_cmd_randomize()` became
  `dali_cmd_randomise()`; the other renamed identifiers are listed under the
  2026-08-25 entry above, and the words outside the command surface
  (`tokenize`, `recognize`, `quantize`, `serialize`, `normalize`) changed with
  no API impact.
  One release step this forces: `dali-starter.yaml` and the README example pin
  `ref: v1.2.0`, which predates the rename, so the documented spelling and the
  firmware an operator actually flashes disagree until that pin is bumped to the
  next tag. CI cannot catch it — the starter config is validated against the tag
  it names, not against this tree.
  `DaliCommissioningOptions` gained `quiesce_control_devices` and
  `DaliCommissioningResult` gained `quiescence_requested`, `quiescence_started`,
  `quiescence_release_attempted`, `quiescent_state_unknown`, and
  `quiescence_error`, changing both layouts; the option is off when the struct is
  zero-initialized, so behaviour is unchanged for a caller that does not set it.
  One runtime behaviour change to announce with them: a query that meets
  `MALFORMED` or `OVERFLOW` inside its reply window now re-sends once if it holds
  a retry budget, instead of failing on the first blip. Retry-safe commands only,
  so no command repeats that could not already repeat on a timeout — but a noisy
  bus will show more `tx_retries` and fewer aborted sequences than before.
- Announce the ESPHome console verb renames in the release notes. They are the
  operator-visible half of the `dali_cli` adoption and have no aliases — the
  rename table has never been written down — it is owed, not merely misfiled,
  and the list below is the raw material for it. Anything in Home Assistant that writes a command
  string to the `text:` entity (scripts, automations, dashboard buttons) needs
  updating. The target spelling lands back where the console started: a short
  address is `a<N>` or a bare number, as it was before the shared tables. Only
  v1.2.0's short-lived `s<N>` is gone, and it now fails as `bad target`, so a
  script updated to `s<N>` for that one release needs updating again. Beyond
  that, `memread`/`memwrite`
  became `devmem read`/`devmem write`, `iquery a0:1 x` became `iquery 0 1 x`,
  `query a0 actual-level` became `query a0 actual`, `config <t> <name> <dtr0>`
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
- Add a dedicated Home Assistant commissioning workflow/UI only after the shared
  path is hardware-proven. The ESPHome TCP diagnostic shell already exposes the
  shared guarded verb behind `allow_commissioning: true`.
- Improve scene/fade UX. A level-changing command now arms a deferred re-read,
  but the refresh it triggers is a full pass over every light entity rather than
  a query of the one that moved, and nothing reads a final value after a
  transition longer than the arming window.
- Add capture replay, parser fuzzing, and hardware-in-loop tests for timing,
  collisions, queue pressure, bus faults, and power restoration.
- Add DT1 and other device types only when an installation requires them, following
  the shared DT6/DT8 module pattern.
- Add Part 103 special-command frames — `0xC1` first byte, opcode in the second —
  as a command-table frame kind. Control-device commissioning needs it, and it
  would let gear commissioning expel control devices with a Part 103 TERMINATE
  rather than only quiescing them. `dali_protocol.md` records the opcode space
  and the two encodings that differ from their Part 102 namesakes. Worth doing
  when device commissioning is actually wanted, not before.

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
| `components/dali/dali_cli.c/.h` | Portable CLI core: tokenizing, verb table, validation, formatting. Shared by the native app and the ESPHome console |
| `components/dali/dali_dim_curve.c/.h` | IEC 62386-102 arc power level ↔ light output conversion |
| `main/main.c` | Native ESP-IDF diagnostic application entry point |
| `main/dali_diag.c/.h` | Device half of the serial CLI: task, transports, workflows |
| `esphome/components/dali` | Active ESPHome external component |
| `dali-starter.yaml` | Tracked starter/commissioning firmware |
| `dali_test.yaml` | Tracked CI coverage config; the widest configuration this repo can compile against its own tree |
| `_local/dali-diag-local.yaml` | Ignored compile-test copy of the diagnostic firmware |
| `_local/secrets.yaml` | Ignored, untracked, and holds live credentials — see Installation State |
| `_local/dali-1k.yaml` | Ignored first-floor site firmware |
| `_local/dali-2k.yaml` | Ignored second-floor site firmware |
| `test/` | Host suites and the vendored Unity runner |
| `tools/` | `dali-shell` and other operator-side scripts |
| `dali_commands.md` | Every verb and named command table, both surfaces |
| `dali_protocol.md` | Frame layouts, opcode tables by IEC part, event decoding |
| `commissioning_readme.md` | Commissioning workflow: flash, walk the bus, export a config |
| `dali_capability_matrix.md` | Per-capability API/verb/vector/hardware/ESPHome status |
| `steinel_bank2_reference.md` | Installation-specific Steinel memory observations |

## Build and Test Commands

ESPHome configuration/build:

```powershell
esphome config  dali_test.yaml               # cheapest check; builds the in-repo component
esphome compile dali_test.yaml               # the working tree, every platform
esphome compile dali-starter.yaml            # whatever ref it pins, not the working tree
esphome compile _local/dali-1k.yaml          # deploy copies; whatever they pin
esphome compile _local/dali-2k.yaml
```

`esphome config` is the cheap check and is what to reach for first: it validates
the schema without touching a build directory, so it cannot collide with a
compile already running in another terminal. It does not run `to_code()`, so a
codegen error still needs a compile to surface.

To prove uncommitted component changes actually build, compile `dali_test.yaml`
— it resolves `type: local` against `esphome/components`, so it compiles the
working tree by construction, and it declares every platform and every optional
block on purpose. `_local/dali-diag-local.yaml` is the diagnostic equivalent,
with `path: ../esphome/components` for its location.

An option added to the schema without being added to `dali_test.yaml` is an
option nothing compiles. Extend that file in the same change.

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

### Continuous integration

Four workflows in `.github/workflows/`. Everything but `host-tests.yml` was added
on 2026-08-12 and has not yet been observed passing on GitHub.

| Workflow | Job | Covers | Trigger |
|---|---|---|---|
| `host-tests.yml` | `test` | 26 host suites, cmake/ctest over `test/` | push main/dev, PR to main |
| `idf-build.yml` | `build` | native firmware, `idf.py build` for esp32 on IDF 6.0.1 | push main/dev, PR to main |
| `esphome-build.yml` | `sources` | sources ↔ `proto_dali_*.c` shims ↔ CMakeLists `SRCS` agree | push main/dev, PR to main |
| | `discover` | finds the tracked `dali*.yaml` and sorts them by component source | " |
| | `config` | Python schema and pin validators, one check per discovered config | " |
| | `compile` | ESPHome C++ and vendored C, per locally-sourced config, in parallel | " |
| `release-packaging.yml` | `clean-checkout` | `esphome compile` from a git tag in an empty directory | tag `v*`, manual |

Notes an operator needs:

- `esphome-build.yml` names no configuration. `discover` runs
  `git ls-files 'dali*.yaml'`, so a config added to the repo is covered without
  editing the workflow, and a removed one stops being referenced by a job that
  would then fail looking for it.
- `discover` sorts what it finds by whether `external_components` says
  `type: local`. Those build the tree under test and are compiled;
  `dali_test.yaml` is the only one today and exists for that purpose. A config
  pinning `type: git` at a ref is validated only — ESPHome would fetch and
  compile that ref instead of the branch, so a 20-minute build would report on
  code the pull request never touched.
- Validating a pinned config is still worth its minute, and a failure there does
  not mean the branch is broken. It means the tracked config has drifted out of
  schema with the ref it names, and either the pin or the config needs updating
  before release. The per-config `config` matrix keeps that legible in the checks
  list rather than buried in a shared log.
- If no tracked config uses `type: local`, `compile` is skipped and the ESPHome
  C++ layer gets no CI coverage at all. `discover` emits a `::warning::` for
  that case, but a run can still go green — do not delete `dali_test.yaml`
  without replacing what it covers.
- `secrets.yaml` is gitignored, so every ESPHome job writes its own dummy one at
  the values the tracked configs reference. `dali_test.yaml` needs none of them:
  its credentials are inline dummies, so it validates in a bare checkout,
  including a fork's first CI run. The clean-room job needs none either — the
  component's only ESPHome dependency is `esp32`, so its consumer config carries
  no wifi, api, or ota.
- `release-packaging.yml` deliberately never runs `actions/checkout` and caches
  nothing. Run it inside a repo checkout and it passes for the wrong reason.
  `workflow_dispatch` takes a ref, so a branch can be packaging-tested before it
  is tagged.

## Documentation Policy

- Keep this file limited to current verified state, constraints, and open work.
- Record completed changes in Git history or a changelog; remove them from the
  active backlog.
- Keep verb and argument detail in `dali_commands.md`, frame and opcode detail
  in `dali_protocol.md`, and per-capability API/verb/vector/hardware status in
  `dali_capability_matrix.md`.
- Label claims as host-tested, hardware-verified, or still unverified.
- Do not add session-log TODO files; merge active work into the prioritized list
  above.
