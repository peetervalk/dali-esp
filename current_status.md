# DALI-ESP Current Status

What `dev` does, described as expected behaviour. This is an annex to
`AGENTS.md`, which holds the architecture, the layer rules, the ISR and timing
constraints, the documentation rules, and the build, test and CI notes.

**Nothing here is a verification claim.** Other files hold that:

| File | What it holds |
|---|---|
| `todo.md` | Every expectation below that a real bus has not yet confirmed, each with the procedure that would confirm it; also the development backlog |
| `dali_capability_matrix.md` | Per capability, whether it has run on a bus |
| `project_log.md` | The dated evidence |
| `CHANGELOG.md` | Per-release changes, including what is unreleased on `dev` |
| `dali_commands.md` | Verb and argument detail |
| `dali_protocol.md` | Frames and opcodes |

## Release State

| | |
|---|---|
| Latest tag | `v2.0.0`, 2026-09-18. `main` is the tag plus `7f26fef`, which moved the sample configs' pins to `v2.0.0`. The tagged tree itself still pins `v1.3.0` |
| Next release | `v3.0.0`. `dev` breaks the backup format and the C API; the notes collect in `CHANGELOG.md` under *Unreleased* |
| `dev` vs `main` | `dev` carries everything since the tag |

Two defects in `v2.0.0` that `dev` fixes matter to anyone running the tag:

- **Both Part 103 addressing encodings are wrong.**
  - `address d<N> set d<M>` sends the device to 2M+1, and a device-space
    `restore apply` does the same.
  - `commission devices` finds no unaddressed device.
  - Do not use them on the tag. Gear addressing is unaffected.
- **The TCP shell runs whatever a browser POSTs to its port.** That includes
  commissioning where `allow_commissioning: true`, which `dali-starter.yaml`
  sets. On the tag, keep it off wherever a browser is used on that network.

`CHANGELOG.md`, *Unreleased*, lists the rest under *Fixed*.

`dali-starter.yaml` and the README example pin the tag, so a new installation
gets released code. Pointing an installation at `dev` is a deliberate move:
compile and test that build first.

**What is deployed is not recorded here.** The site configurations live in
Home Assistant. Nothing in this repository is evidence of what a device runs,
or of which ref it was built from.

The project is not DALI Alliance certified, and nothing here claims
conformance.

## Where The Aims Stand

| Aim | State |
|---|---|
| CLI completeness | Every shared capability has a typed verb. That includes memory, DT6, DT8, input-device query and configuration, vendor helpers and control-device memory. It also covers gear and control-device commissioning and re-addressing, backup and restore of addresses, groups and instance settings, CONTINUOUS UP/DOWN, arc power MASK, and send-twice `raw2`. Which of these has met a bus is the matrix's job |
| ESP32 / ESPHome controller | Runs two installations: brightness control, coupler observation, sensor polling, diagnostics and discovery. It is not yet a general, fully state-correct controller; see *Known Limitations* |
| Protocol separation | The C stack in `components/dali` has no ESPHome dependency. Every frame the ESPHome layer sends is built by a shared builder. Only the wiring is ESPHome-bound, and host tests cannot reach it: console dispatch, the refresh pump, and the entity registries |
| Standards | Selected workflows behave as the standard requires on real buses. That is not conformance. Some tests repeat implementation constants rather than independent vectors derived from the standard |

Protocol and state correctness come before broad new device support.

## Hardware

- **Board:** ESP32-DevKitC-VE (ESP32-WROVER-E) with a MikroE DALI 2 Click. TX
  is on GPIO18 and RX on GPIO19.
- **GPIO16/17:** the WROVER-E's PSRAM uses them. The pin schema does not
  reject them, because they are ordinary pins on WROOM and other variants.
- **Installations:** two run the ESPHome controller, called 1k and 2k
  throughout. Their bus layouts are at the top of `todo.md`.
- **Couplers:** existing DALI-1 push-button couplers in direct-control mode
  drive the lamps themselves. The controller observes their frames to keep
  Home Assistant in step.

## What The Code Does

### Bus layer

- **PHY.**
  - A 104 µs GPTIMER tick clocks TX.
  - RX is buffer-first: the edge ISR writes a fixed ring buffer, and a
    task-context decoder turns it into frames.
  - Allocation is static throughout. `dali_phy_init()` checks every
    acquisition and releases what earlier steps took before it returns an
    error.
  - Core affinity is not hardcoded.
  - 16- and 24-bit frames are both first-class.
- **Frame end.** The TX ISR stamps the end of the forward frame's last data
  bit. `dali_phy_tx()` reports success only once the ISR reaches
  `DALI_PHY_TX_DONE`, and returns `DALI_ERR_TIMING` when a frame does not
  complete in time.
- **Reply attribution** runs from that stamp, with two opening edges:

  | Observation | Accepted from | Constant |
  |---|---|---|
  | Decoded backward frame | 3.664 ms | `DALI_REPLY_WINDOW_OPEN_DECODED_US`, derived from `DALI_SETTLE_MS` |
  | Undecodable activity | 5.5 ms, the standard's minimum | `DALI_REPLY_WINDOW_OPEN_US` |

  The window closes at 28.664 ms. The asymmetry is the safety: undecodable
  activity must not read as a reply, because COMPARE maps it to YES and would
  invent gear that is not there.
- **Undecodable activity.** Frame-like activity inside the window is
  `DALI_ERR_RX_ACTIVITY`. COMPARE alone reads it as YES; every other query
  keeps it as an error.
- **Sequences.** Scheduler sequences run contiguously and report a
  `DaliSequenceResult`: the overall error, the failing step, the steps
  attempted, and one backward frame per reply-bearing step. Every multi-frame
  workflow is built on them:
  - memory reads, and the control-device memory write
  - discovery's ENABLE DEVICE TYPE pairs, and the group query
  - commissioning's search/COMPARE probe and its PROGRAM/VERIFY pair
  - DT6/DT8 command grouping
  - Part 103 DTR loads

  Only genuine single-frame queries use `transact` directly.
- **Retries.**
  - A retry-safe command is re-sent once on a timeout, after
    `DALI_REPLY_TIMEOUT_BACKOFF_US`.
  - It is also re-sent on a MALFORMED or OVERFLOW observation inside its reply
    window.
  - Nothing that could not already repeat is repeated.
- **Events.** Unsolicited event frames reach their subscribers in every
  scheduler state. An event that lands inside a reply window aborts that
  transaction with `DALI_ERR_INTERVENED`.
- **Waiting.** A blocking caller that stops waiting gets
  `DALI_ERR_WAIT_EXPIRED`, never the `DALI_ERR_TIMEOUT` that means silence. The
  wait is sized from the transport's retry budget.
- **Admission is observable.** `dali_sched_queue_stats()` reports depth,
  capacity, high-water and admitted count. Rejections are split into
  queue-full and reset-barrier. A rejection is dropped work; the scheduler
  never retries a refused submission.
- **Transport.** `dali_transport` is the single bus abstraction the higher
  modules share. Only its whole-sequence entry point is atomic, and that entry
  point is optional, so a caller that needs atomicity asks
  `dali_transport_supports_atomic_sequence()`.

### Protocol and device support

- **Part 102 control gear:**
  - DAPC and arc power MASK
  - every level command, CONTINUOUS UP/DOWN and GO TO SCENE included
  - 34 named queries
  - 19 configuration commands, with each DTR0-consuming one sent as a single
    sequence
  - 18 special commands
  - send-twice
- **DT6 (IEC 62386-207):** 5 configuration commands and 19 queries. Each goes
  out with its ENABLE DEVICE TYPE 6, and its DTR0 load if it has one, as one
  sequence.
- **DT8 (IEC 62386-209):** temporary set, step, activate, store and query
  commands, and the four-step 16-bit colour read. It is reachable from the
  shell only.
- **DT1 and the other device types are not implemented.**
- **Part 103 instance commands** take the whole instance byte:

  | Byte | Selects |
  |---|---|
  | `0`–`31` | one instance by number |
  | `0x80\|G` | instance group G |
  | `0xC0\|T` | every instance of type T |
  | `0xFF` | every instance |

- **Input-device configuration.** Builders for the Part 103 generic instance
  configuration and for Parts 301 (type 1), 303 (type 3) and 304 (type 4) have
  an independently audited opcode surface.
- **Event decoding.** Part 103 events decode into canonical source fields for
  all five normal schemes. Command and reserved frames are rejected, and all
  ten event-information bits are kept.
- **Event source matching.** `dali_event_source` answers one question: could
  this event have come from the instance at (address, instance)?
  - Device/Instance events match exactly.
  - Other schemes match on the fields they carry, narrowed by a profile read
    from the instance: its type, scheme and three instance groups.
  - An unknown fact never excludes a match, and the profile's scheme is never
    used to exclude one.
- **Memory.**
  - The Part 102 helper reads a gear's Bank 0 identity block.
  - The Part 103 form reads a control device's own Bank 0 over 24-bit framing.
  - Block reads go out as one sequence.
  - `devmem write` sends its unlock and write as one queue entry.
- **Quiescent mode.** Part 103 START/STOP QUIESCENT MODE can be sent per device
  or by broadcast.
- **Vendor helpers.** Lunatone sensor scaling queries, and the Steinel HF 360 II
  instance profile and value conversions.
- **The two address spaces are independent.** A unit that is both gear and a
  control device answers each from a separate Bank 0. On 2k the two spaces of
  one unit report different GTINs, so nothing may infer that gear address N
  and device address N are the same unit.

### Discovery

- **The walk.** `scan` and `discover` walk both address spaces.
  - Each present gear has its groups, device type, version, actual level and
    level profile read, along with QUERY NUMBER OF INSTANCES.
  - Each control device has its instances enumerated: type, resolution,
    status, event scheme, event priority and three instance groups.
  - Bank 0 identity is read in each space independently.
- **Contested addresses.** Two kinds of address are contested:

  | How it shows | What happens to it |
  |---|---|
  | The status reply is undecodable activity | Reserved, not listed, and held out of every free pool |
  | The Bank 0 identity read collides twice running: units of one product that answer every status probe in step | Listed, but marked and named |

  `dali_discovery_gear_address_contested()` is the one test, and the planner,
  the snapshot, the commissioning audit and `backup save` all use it.
- **Quiescence.** Every operator-driven walk brackets itself with broadcast
  START/STOP QUIESCENT MODE, and releases on every exit path:
  - `scan` and `discover`
  - both commissioning pre-scans and the post-scan
  - `backup save`
  - the `restore` scans

  Two walks deliberately do not take the bracket. `find switches` exists to
  hear events. The integration's periodic scan runs unattended, and silencing
  occupancy for minutes is a trade nobody is present to accept. A release
  that fails prints a line naming `quiescent off all` as the fix.
- **The event note.** A walk names the events that arrived during it as
  expected traffic on a bus with sensors, not as a timing fault.
- **Duration.** A walk costs about 0.34 s per empty address and 1.79 s per
  present gear, so walk length depends on what is on the bus: about 45 s for
  16 gear and 29 s for 5.

### Commissioning

- **`commission unaddressed [first] [max]`** runs the Part 102 walk:
  1. a pre-scan for occupied and contested addresses
  2. INITIALISE for unaddressed gear, then RANDOMISE
  3. a binary search by COMPARE
  4. PROGRAM SHORT ADDRESS, VERIFY and WITHDRAW for each unit found

  It assigns the lowest free addresses, and never touches addressed gear.
- **Brackets.** The walk is bracketed by broadcast START/STOP QUIESCENT MODE
  and by Part 103 TERMINATE, sent before INITIALISE, again right after it, and
  in the unwind. Quiescence stops control devices transmitting into a COMPARE
  window. TERMINATE stops one answering COMPARE from its own addressing state.
- **Equal random addresses** are detected at VERIFY. The pair is de-addressed
  with PROGRAM SHORT ADDRESS `0xFF`, dropped from the search, and left for a
  second run to place.
- **Cleanup.** Every exit after the opening sequence converges on one Part 102
  TERMINATE. It goes through a cleanup transport that ignores a disconnected
  front end, so closing the terminal does not cancel the safety unwind.
- **The post-scan audit** runs on every exit that could have written an
  address. It sorts the run's addresses into confirmed, contested and silent.
  It also names two classes the run did not claim:
  - an address occupied now that was free before and was never recorded
  - an address newly contested that the run never assigned
- **`commission devices`** is the same walk in the Part 103 space:
  - INITIALISE `0x7F` selects unaddressed devices.
  - It is bracketed by quiescence and a Part 102 TERMINATE.
  - Its post-scan names addresses as `d<N>`.
- **Policy.** Both walks need `DALI_SHELL_ALLOW_COMMISSION`. Over TCP that means
  `allow_commissioning: true`.

### Addressing, backup and restore

- **`address`** is the checked tier over re-addressing.
  - `<aN>` reaches gear. `<dN>` reaches control devices, and the `d` is
    mandatory.
  - `set` and `clear` exist in both spaces. `add` and `remove` exist for gear
    only, because nothing reads Part 103 device groups back.
  - Every arm proves its result by reading it back.
  - `set` and `clear` need the commissioning policy. `add` and `remove` are
    ungated.
- **The checked write.** Every `address` arm and every `restore apply` move
  writes through `dali_restore_write_short_address()`:
  1. It loads DTR0 and reads it back with QUERY CONTENT DTR0: gear `0x98`,
     device `0x36`.
  2. It sends the SET SHORT ADDRESS pair only once the unit holds the value.
  3. A wrong, silent or unreadable read-back gets one more load. If that fails
     too, nothing more is sent.

  Three cases report specially:
  - Clearing a contested address sends the pair unchecked when the read-back
    collides, and says so.
  - When the destination is silent afterwards, the source is probed too, so a
    unit that stayed is told apart from one that answers nowhere.
  - A transport error says whether SET SHORT ADDRESS had already gone out.
- **`backup save`** records which physical unit holds which short address in
  both spaces, keyed by the unit's Bank 0 identification number. It also
  records:
  - each gear's group mask
  - each control device's instance settings: type, enabled, event scheme,
    priority, filter and three instance groups, up to 64 instances

  It leaves contested addresses out and says so, flags entries it cannot
  anchor, and warns when a broadcast QUERY MISSING SHORT ADDRESS shows
  unaddressed gear.
- **The format is version 2.** A full blob is 3,208 bytes, and a version-1
  blob is refused. `dali_snapshot_decode()` validates a blob in full before it
  writes anything.
  - On ESPHome the backup persists to NVS as a 3,216-byte record, written by
    the main loop.
  - The native build has no store. `backup export` prints the `backup import`
    script that reproduces the backup.
- **`restore plan` and `restore apply`** turn a backup plus a live bus into an
  ordered list of plain addressed moves. There is no INITIALISE window, so a
  restore is interruptible at any point and converges on re-run.
  - Cycles are broken by staging through a spare address.
  - A unit the backup never saw is moved aside.
  - Contested addresses are reserved, and reported as `contested` or `target
    contested`.
  - `apply` confirms each move before sending the next: the destination
    answers, the source is silent, and the identification number there is the
    unit's. It stops at the first move that fails.
  - Confirmed gear moves carry the integration's group map with them.
- **`restore groups [apply]`** is a separate planner, and `restore apply` never
  reaches it.
  - It diffs each gear's recorded group mask against the bus, matched by
    identification number, and writes the ADD/REMOVE bits wherever the gear
    answers now.
  - It skips gear whose groups the backup never read, or that will not read
    back.
  - It reads every gear back afterwards, and flags `MISMATCH`.
- **`restore instances [apply]`** finds each control device by identification
  number. It writes each recorded instance field that differs, ENABLE or
  DISABLE last, and reads it back.
  - It records "disabled" only when QUERY INSTANCE STATUS agrees with a silent
    QUERY INSTANCE ENABLED.
  - Type-specific settings, such as Part 303 timers, are not recorded.
- **Scenes are not captured.**

### Diagnostic shell

- **One implementation, two bindings.**
  - `components/dali/dali_shell.c` is the shell: one session owning every
    verb, the blocking transport, and the caches a workflow builds up.
  - `components/dali/dali_cli.c` is its portable core: tokenizing, the verb
    tables, argument validation and reply formatting. It has no ESP-IDF,
    FreeRTOS or bus dependency.
  - The bindings are `main/dali_diag.c` (UART0) and
    `esphome/components/dali/dali_shell_tcp.cpp` (TCP).
  - A binding moves bytes and owns a session's lifetime. A surface that must
    refuse a verb declares a `DALI_SHELL_ALLOW_*` policy.
- **Dispatch is one table.** `dali_cli_resolve()` checks the argument count
  before any handler runs, so trailing tokens are refused. `help`, `list` and
  `schema` are generated from the same tables.
- **`raw` and `raw2`** send one arbitrary 16- or 24-bit frame, once or as a
  send-twice pair. A frame that is itself a commissioning command needs the
  commissioning policy, as the named verbs do. `dali_cli_raw_frame_is_commissioning()`
  decides which frames those are.
- **Bus claim.** A multi-frame workflow claims the bus through
  `try_claim_bus()`. That raises the scan gate, then waits up to 5 s for queued
  work to drain. A claim that cannot drain is refused.
- **The TCP binding:**

  | Behaviour | How |
  |---|---|
  | One session at a time | A second client waits in the listen backlog until the first ends |
  | Browser requests are refused | A connection that opens with an HTTP request line is closed before any of its lines is dispatched. The device logs the sender's address at WARN. `dali_cli_peer_sniff()` decides, within the first line |
  | A client that stops reading is dropped | A send that makes no progress for 10 s (`SO_SNDTIMEO`) marks the peer lost, which ends a running workflow at its next step |
  | A vanished client is reclaimed | TCP keepalive (30 s idle, then 3 probes 5 s apart) frees it in about 45 s |
  | An unused terminal is closed | after `idle_timeout` |
  | Sockets are counted | The listener and its one client are registered with ESPHome's socket count, adding two lwIP sockets |

### ESPHome integration

- **Lights.**
  - They expose brightness only. DT8 is not mapped to colour traits.
  - Brightness maps through each gear's own MIN/MAX LEVEL window and DT6
    dimming curve. These are read at boot, on every refresh, after a scan, and
    after any command that moves them.
  - A group or broadcast entity uses the union of its known members' windows.
  - YAML `min_level`, `max_level` and `dimming_curve` override what was read.
- **Group lights.**
  - A group entity polls one member, picked from membership read off the bus.
    No `query_address` is needed.
  - Only a complete group discovery replaces the membership, which persists to
    flash. A failed query keeps the previous map and withholds the generated
    YAML.
  - `group forget` retires a departed member.
  - A re-address confirmed by `address` or `restore apply` moves the member.
  - A clear retires the integration's entry. The gear keeps its groups and
    still follows group and broadcast commands.
- **Writes.**
  - A light suppresses a redundant command only against state a scheduler
    completion confirmed, never against an enqueue.
  - A rejected enqueue keeps the target and retries. A failed transmission
    invalidates the cache and re-arms one bounded retry.
  - One command per light is in flight at a time. "Confirmed" means
    transmitted, not acknowledged by the device.
  - ESPHome's restore write is told apart by being the first `write_state()`
    after `setup()`. A bus reading pushed back is told apart by matching the
    exact `(is_on, level)` pair sent. There is no startup time window.
- **Cross-task state.** Light state and sensor values cross from the DALI task
  in one packed mailbox each. Pending observations coalesce to the latest.
- **Sensors.**
  - Polling is authoritative, and each reading is one sequence.
  - An event that could be the sensor's own requests an immediate poll.
    Device/Instance events match exactly; other schemes match by inference
    through `dali_event_source`, logged as `event poll requested (inferred)`.
  - The source profile is read after boot, after a scan, after a shell
    workflow, and after an `iconfig` to the sensor's device. It is read again,
    at most every five minutes, when an event arrives in a scheme it did not
    expect.
  - A sensor whose instance is off scheme 2 is logged once. That is a WARN
    when it polls on events or a dispatch rule keys on its device.
  - Event information is never published as a sensor value.
- **Headless dispatch.** YAML rules act on observed frames: legacy 16-bit
  coupler commands, and Part 103 events keyed by short address, instance,
  instance group, device group or instance type. An `observe` rule keeps Home
  Assistant in step with a coupler without retransmitting.
- **The scan gate.** Every component-owned producer observes it:
  - Refresh and due sensor polls stay pending.
  - Identify pauses.
  - Console and diagnostic actions are refused.
  - Home Assistant light writes keep their target and retry.

  The local-only `queue` verb is the exception. Headless events are drained so
  their queue cannot overflow, but actions observed during a scan are dropped,
  not replayed.
- **`bus_fault`** separates current availability from history. A frame that
  clocks out in full turns `Bus stuck (N total)` back into `OK (N past
  faults)`.
- **The console** (`text:` platform) runs the shell's verb set, minus anything
  that needs a terminal or a blocking transport: no `backup`, `restore`,
  `commission`, `address` or `dt8`. It also refuses the commissioning
  spellings of `special`, `config`, `raw` and `raw2`.
  - `OK` means queued.
  - An asynchronous command publishes `pending`, then its result.
  - Replies are decoded by the same function the shell prints through.
  - A level a console verb sets reaches the light entity on the next refresh;
    `dt6 select-curve` starts that refresh itself.
- **The Identify button** blinks the target address for 10 s, then restores
  its level. The log reads `back to level N`, `switched off again`, or `left
  at min` when the level could not be read.
- **Pins.**
  - The GPIO schema is board-aware and refuses TX equal to RX.
  - It declares the modes `dali_phy_init()` requests, so an input-only pin
    named as TX is refused.
  - No pin number is hardcoded.
  - A failure to create the DALI task fails the component.
- **sdkconfig.** The component sets `CONFIG_GPTIMER_ISR_CACHE_SAFE` and
  `CONFIG_GPIO_CTRL_FUNC_IN_IRAM`, as the native `sdkconfig.defaults` does, so
  the TX bit clock keeps running through flash writes.

## Known Limitations

Facts about the code as it stands. Each has its fix, decision or measurement in
`todo.md` under the heading named.

**Bus timing and arbitration**

- **No collision arbitration.** The controller has no proven collision
  detection or arbitration strategy. DALI-2 priority and backoff are not
  implemented. Direct-control couplers transmit too, so simultaneous traffic
  remains a risk. *Space forward frames from received frames*.
- **Forward frames follow received frames too closely.** Ours follows a reply
  by about 2.9 ms and an event by as little as 1.7 ms. DALI-2 devices wait at
  least 13.5 ms after any frame, by priority. *Space forward frames from
  received frames*.
- **The RX edge interrupt is not IRAM-safe.** A frame that arrives during an
  NVS commit (light state, the group map, a backup) can be lost or garbled.
  *The RX edge interrupt is not IRAM-safe*.
- **ESPHome's loop shares core 1 with the DALI tasks.** The PHY's pre-TX idle
  check busy-waits there: 1.67 ms per frame, 33 ms per attempt on a stuck bus.
  *The PHY busy-waits on the loop's core*.

**Scheduling and scans**

- **A scan is not reserved.** The TCP shell's single-frame verbs can interleave
  with a button scan. *Reserve the scheduler for a scan*.
- **Some clients act on admission, not transmission:** the console's `OK`, the
  refresh pump, and headless dispatch. *Confirm transmission for every
  scheduler client*.
- **One intervening frame ends a scan.** Any per-address error other than
  TIMEOUT, MALFORMED or RX_ACTIVITY stops the walk. *One intervening frame
  aborts a whole scan*.
- **`discover` can drop an input-device enumeration silently.** The device line
  shows N instances, with no enumeration and no error. *Report a failed
  input-device enumeration*.

**Shared addresses**

- **Some shared addresses stay invisible.** Two units that answer alike are
  found only through a colliding identity read. Gear with an empty Bank 0, or a
  collision that reads as silence or as a malformed reply, stays listed as one
  unit.
- **A shared control-device address can read as silence.** Then `address d<N>
  clear` answers `does not answer`. The by-hand clear is device DTR0 `0xFF`
  (`raw C130FF len=24`), then `raw2 <addr>FE14 len=24`. *Clearing a shared
  device address*.

**Backup and restore**

- **`restore_find_spare()` can stage onto a missing unit's recorded address.**
  A unit that was only unpowered then returns to a contested address.
- **Part 103 device groups are decode-only.** Discovery does not read them, the
  backup does not record them, and `address <dN> add` is refused. A replaced
  control device gets its address back, but not its device groups. *Part 103
  device groups*.
- **Backups do not hold scenes or type-specific instance settings,** such as
  Part 303 hold and report timers.

**Writes and read-back**

- **No memory or configuration write reads itself back.** `iconfig`, `devmem
  write` and `config` report transmitted, not applied. Read the value back.
- **Quiescent state cannot be read back.** QUERY QUIESCENT MODE is not
  implemented. A device left quiescent with `quiescent on` stays silent until
  `quiescent off`, or until its own timeout: 15 minutes in TI's firmware, and
  unknown for the Steinel. That looks the same as a dead sensor.

**Home Assistant**

- **An input sensor holds its last value** when its device stops answering.
  Nothing marks it stale. *What an input sensor shows when its device stops
  answering*.
- **No addressing fault reaches Home Assistant.** `bus_fault` reports PHY
  liveness only, and contested addresses appear only in logs. A group light
  entity is not told when its membership changes. *Addressing faults and
  membership changes reach Home Assistant*.
- **Entity limits drop silently.** The 33rd light, 17th sensor and 33rd
  dispatch rule are dropped with a boot-time log line. *Entity limits fail
  silently*.
- **Light entities are brightness-only,** and DT8 is reachable from the shell
  alone.
- **Brightness maps linearly onto light output,** and bypasses ESPHome's
  `gamma_correct`. *The brightness mapping*.

**ESPHome internals**

- **The DALI PHY can disable ESPHome pin interrupts that attach after it.** The
  workaround is `use_interrupt: false` on a `gpio` binary sensor. *The DALI PHY
  can break every other GPIO interrupt*.
- **`load_address_backup()` reads ESPHome preferences from the shell task,**
  which races the loop's `save()`.

**Tools**

- **Native `trace on` can block the DALI task** once the UART TX buffer fills.
- **Find Couplers prints one summary,** truncated with a count.

## Operational Constraints

- Do not imply DALI Alliance certification or complete IEC 62386 coverage.
- Treat every input-device configuration write and memory write as transmitted
  until a read-back confirms it.
- Build every new multi-frame workflow on `DaliSequence`.
  - Issuing dependent frames as separate transactions brings back the class of
    bug the migration removed.
  - The tell is a query whose answer depends on a register, or an enumeration
    pointer, that the same query or the frame just before it modifies.
- Do not use GPIO16 or GPIO17 on the WROVER-E. Nothing enforces this.
- **Static DRAM: `dali_test.yaml` links at 141,792 B of the 176.5 KiB static
  window.** The diagnostic shell is the largest single item in it.
  - `dali_shell.c` is compiled unconditionally through the
    `proto_dali_shell.c` shim. Nothing outside `dali_shell_tcp.cpp` references
    it, so `--gc-sections` drops all of it when a config has no `shell:` block.
  - Every file-scope buffer added to the shell is paid for on each board that
    runs it.
  - The shell caches 16 devices of 32 instance records, so one byte per record
    costs 512 B.
  - The version-2 backup holds its 768-byte instance table twice in blob form,
    once as the shell's staging copy and once as the integration's flash
    record, and its 1,536-byte model once.
- **`sram1_as_iram` is inert at this IRAM level.** `.iram0.text` ends at
  `0x40093464`, about 77 KiB of the default 128 KiB.
  - IDF takes D/IRAM out of the heap only in proportion to what the app's IRAM
    uses above `0x400A0000`: `_sram1_iram_len` in `memory.ld.in`, clamped at
    zero. Below that line the option costs nothing and buys nothing.
  - Above the line, the heap pays byte for byte, and the image needs a
    bootloader that knows the region.

## Configurations In This Repository

**The live site configurations are in Home Assistant, not here.** To learn what
a device runs, ask Home Assistant or the device.

| Configuration | Component source | Role |
|---|---|---|
| `dali-starter.yaml` | `ref:` the current tag | Starter and commissioning firmware: the config to flash first on a new bus |
| `dali_test.yaml` | `type: local` | CI coverage config. A fictitious layout, never flashed, and the widest configuration this repository compiles against its own tree |

`dali_test.yaml` is `type: local` with `path: esphome/components`, so a
configuration and the component it configures always agree within a commit.
Tracked copies of real sites were tried and dropped. They carried an address
layout nobody else could use, and needed editing whenever a site changed.

## Source Layout

| Path | Role |
|---|---|
| `components/dali/` | Reusable C protocol, PHY, scheduler, transport, discovery, commissioning, memory, device types, dispatch |
| `components/dali/dali_shell.c/.h` | The diagnostic CLI as a reusable session: every verb, the blocking transport, the workflow caches |
| `components/dali/dali_cli.c/.h` | Portable CLI core: tokenizing, verb tables, validation, formatting, and peer classification. Shared by both front ends |
| `components/dali/dali_commissioning.*` | Part 102 control-gear commissioning walk |
| `components/dali/dali_device_commissioning.*` | Part 103 control-device commissioning walk |
| `components/dali/dali_commissioning_audit.*` | The post-scan diff both walks check themselves against |
| `components/dali/dali_snapshot.*` | Records which physical unit (by Bank 0 id) holds which short address, plus group masks and instance settings |
| `components/dali/dali_restore.*` | Turns a snapshot plus a live bus into moves, group edits and instance writes; the checked short-address write |
| `components/dali/dali_group_map.*` | Group-to-member bookkeeping; picks a group light's poll representative |
| `components/dali/dali_event_source.*` | Could this event be that instance's? The matcher behind event-triggered sensor polls, and the profile read that narrows it |
| `components/dali/dali_light_write.h` | Header-only desired/in-flight/confirmed write arbitration |
| `components/dali/dali_refresh_cursor.h` | Header-only refresh-pump cursor |
| `components/dali/dali_dim_curve.*` | IEC 62386-102 arc power level ↔ light output conversion |
| `components/dali/dali_lunatone.*`, `dali_steinel.*` | Vendor-specific helpers |
| `main/main.c` | Native ESP-IDF diagnostic application entry point |
| `main/dali_diag.c/.h` | UART0 binding for the shell; moves bytes only |
| `esphome/components/dali/` | Active ESPHome external component |
| `esphome/components/dali/dali_shell_tcp.cpp` | TCP binding for the shell |
| `esphome/components/dali/proto_dali_*.c` | Shims that pull the vendored C in behind ESPHome's source glob |
| `dali-starter.yaml` | Tracked starter and commissioning firmware |
| `dali_test.yaml` | Tracked CI coverage config |
| `test/` | 34 host suites and the vendored Unity runner |
| `tools/dali-shell` | Operator-side client for the TCP shell |
| `AGENTS.md` | Architecture, layer rules, timing/ISR constraints, documentation rules, build, test and CI |
| `todo.md` | Hardware verification procedures, then the development backlog |
| `project_log.md` | Verification history and investigations |
| `CHANGELOG.md` | Per-release changes, with migrations; *Unreleased* collects what `dev` has since the last tag |
| `dali_commands.md` | Every verb and named command table, both surfaces |
| `dali_protocol.md` | Frame layouts, opcode tables by IEC part, event decoding |
| `commissioning_readme.md` | Commissioning workflow: flash, walk the bus, export a config, change a commissioned bus |
| `dali_capability_matrix.md` | Per-capability API/verb/vector/hardware/ESPHome status |
| `steinel_bank2_reference.md` | Steinel HF 360 II: instance layout and Bank 2 tuning |
