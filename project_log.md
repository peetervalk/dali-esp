# DALI-ESP Project Log

Append-only history. Split out of `current_status.md` on 2026-09-03, because
that file had grown to 140 KB and was being read in full at the start of every
session to answer questions that only its last few sections could answer.

**Nothing here describes current state.** Read `current_status.md` for that, and
`AGENTS.md` for the architecture and the rules. This file holds the evidence
behind those claims: what was verified, when, on what tree, and the
investigations whose conclusions have since been folded into the code.

Two kinds of entry live here:

- **Verification history** — dated records of what was exercised, locally or on
  hardware, and what that pass did and did not cover.
- **Investigations** — the reasoning behind a fix, kept because the reasoning is
  the part that is expensive to reconstruct and easy to get wrong twice.

Other material lives elsewhere:

- **Unreleased changes** used to collect at the end of this file. Since
  2026-10-02 they go straight into `CHANGELOG.md`, under *Unreleased*.
- **Open work**, meaning what still needs a bus and the development backlog,
  is in `todo.md`.

Entries written before 2026-10-02 that point at a P0, P1 or P2 item in
`current_status.md` mean the prioritized list that is now `todo.md`.

Add new entries at the top of their section. The existing verification entries
are grouped roughly by campaign rather than strictly by date, and several refer
to each other by position, so leave their order alone. Never edit an entry to
reflect a later finding: add a new one that supersedes it, and say what it
supersedes.

---

# Verification history

### Moved from `current_status.md` on 2026-10-02 (records that had no entry here)

`current_status.md` now describes capabilities only, without dates. Its dated
material already had entries here, except for the records below. They are kept
as they stood there, under their own dates.

**At the `v2.0.0` tag, 2026-09-18.**

- 31/31 host suites built and passed, locally and in CI.
- `dali_test.yaml` passed `esphome config` and `esphome compile` on ESPHome
  2026.9.0. That config is `type: local`, so this validated the Python schema
  in `esphome/components/dali/__init__.py` against the tree. It also compiled
  the C++ layer and the vendored C from it.
- The native ESP-IDF firmware built in CI, compiling the same
  `components/dali` C under a second toolchain.
- The tag built as an external component in an empty directory, with no
  checkout around it (`release-packaging.yml`). That is the only check that a
  consumer can fetch and build it.
- It was not a hardware pass. The real-bus results behind `v2.0.0` came from
  `dev` during development, not from a flashed `v2.0.0` build.

**Static DRAM and the shell's share, 2026-09-04.** Two builds of the same `dev`
tree for the classic ESP32 with esp-idf: `dali_test.yaml`, and the same config
without its `shell:` block.

| Segment | With `shell:` | Without | Region |
|---|---|---|---|
| Static DRAM (`.data` + `.bss` + `.noinit`) | 133.9 KiB (76%) | 69.1 KiB (39%) | 176.5 KiB (`dram0_0_seg`) |
| IRAM (`.text` + `.vectors`) | 76.7 KiB (60%) | 76.7 KiB (60%) | 128 KiB (`iram0_0_seg`) |
| App image | 1,053,739 B (57%) | 973,083 B (53%) | 1.75 MB partition |

- The shell cost 64.8 KiB of RAM and 78.8 KiB of flash.
- Its session caches were 62 KiB of static `.bss` between them: the
  input-instance cache, two capture rings, two inventories and the held backup.
- `dali_shell.c` is compiled unconditionally through the `proto_dali_shell.c`
  shim. Nothing outside `dali_shell_tcp.cpp` references it, so `--gc-sections`
  drops all of it when the block is absent.

**Static DRAM since then.**

| Date | Static DRAM | Note |
|---|---|---|
| 2026-09-25 | 138,248 B | 1.5 KiB of it is the restore plan's per-move identification number, added that day so `restore apply` can confirm each move |
| 2026-10-01 | 138,656 B | |
| 2026-10-01 | 141,728 B | the version-2 backup |
| 2026-10-02 | 141,792 B | |

The entries below have the detail.

**IRAM end.**

- `.iram0.text` ended at `0x40093464` on 2026-09-25, with the cache-safe
  GPTIMER and GPIO options on.
- `current_status.md` later recorded `0x4009369F`, for a build it did not
  date.
- The 2026-10-02 build in the next entry reads `0x40093464` again, as
  `_iram_text_end` in `xtensa-esp32-elf-nm`.

All of these are far below `0x400A0000`, the line above which `sram1_as_iram`
starts to cost heap.

### Verified locally on 2026-10-02 (TCP shell hardening; uncommitted on `dev`)

Three fixes to the TCP front end, found while assessing a browser GUI for the
shell. A GUI would make each of them worse, so they come first. Nothing touched
a device or a bus.

- **A browser could drive the shell.** A cross-site form, or a no-cors fetch with
  a text/plain body, POSTs to `http://<device>:2323/` with no CORS preflight.
  2323 is not on the browsers' blocked-port list. Nothing in
  `dali_shell_feed_byte()` or the TCP loop told such a request from a shell
  client, and an error line does not end a session. So the request line and the
  headers resolved as unknown verbs, and then every line of the body ran as a
  command. On a build with `allow_commissioning: true`, which
  `dali-starter.yaml` sets, that reaches `commission` and RANDOMISE. Browsers
  that ask before a public page reaches the local network narrow this; they do
  not close it.
  - The fix classifies the start of each connection in `dali_cli_peer_sniff()`.
    An uppercase method token of up to 20 characters, one space, then `/` or
    `*` is HTTP. That is every request form a browser sends to an origin
    server, `OPTIONS *` preflights and WebSocket handshakes included. Verbs are
    lowercase and matched case-sensitively, so nothing the shell accepts can
    open that way.
  - It decides before any CR or LF. So the binding sniffs each byte before
    feeding it and holds nothing back: no line can reach dispatch ahead of the
    verdict. The partial line an HTTP peer leaves in the shell's line buffer is
    cleared by the next attach.
  - Only the first line is examined, because a browser cannot put bytes ahead
    of its own request line. The binding logs the sender's address at WARN,
    writes one refusal line, and closes.
- **The send timeout** (the P1 item). `SO_SNDTIMEO` is 10 s. lwIP returns a
  partial count when a timed-out send moved some bytes, and -1 with
  `EWOULDBLOCK` when it moved none. `write_cb()` already retried the first and
  marked the peer lost on the second, so only the option and a comment
  changed. 10 s rather than "a few": a send blocks only once the 5,760-byte
  send buffer is full, which a live reader drains in milliseconds, while a lost
  segment on poor Wi-Fi costs a retransmission backoff of a second or more.
- **Keepalive.** Probes start after 30 s of silence, every 5 s, and give up
  after 3, so a vanished peer is reclaimed in about 45 s. That matters most
  with `idle_timeout: 0`, where a dead idle session used to hold the shell
  until reboot.
- **Socket registration.** ESPHome sizes `CONFIG_LWIP_MAX_SOCKETS` to the sum of
  what components register through `socket.consume_sockets()`, and the shell
  registered nothing. It now registers one TCP listener and one TCP client. One
  client is enough: `serve()` runs each session to completion before the next
  `accept()`.
- **Docs.** The binding header and `commissioning_readme.md` both said a second
  connection is accepted and told the shell is busy. The comment on `listen()`
  in the binding already said otherwise: nothing is accepted while a session
  runs, so a second client waits in the backlog. Both now say so.
  `tools/dali-shell`'s `probe_shell()` docstring makes the same assumption
  and is untouched; a node with a session open fails its probe.
  `dali-starter.yaml` and the README example pin `v2.0.0`, so their
  `idle_timeout` comments still describe the tag; see the unreleased changes.

Results:

- 34/34 host suites. `test_cli` has 7 new vectors (97 tests), including the
  attack's full shape: a POST whose body carries `scan`. A mutation pass killed
  all six mutants: the `*` target dropped, the method bound off by one, a
  leading hyphen accepted, CR/LF leaving the verdict open, the verdict not
  final, and lowercase accepted as a method.
- `dali_test.yaml` compiles on ESPHome 2026.9.0 (IDF 5.5.5) with no warnings
  in the component. The build log reads `Setting CONFIG_LWIP_MAX_SOCKETS to 14
  (TCP=7 [api=3, captive_portal=3, dali.shell=1], UDP=3 [...], TCP_LISTEN=4
  [api=1, dali.shell=1, ota=1, web_server_base=1])`, against 12 before. Static
  RAM is 141,792 B, +64 B. lwIP's `sockets` table accounts for 40 B of it: 280 B
  for 14 slots. Flash is 1,036,283 B. The new ELF carries both new log strings.
- The native IDF 6.0.1 build passes, `dali_cli.c` recompiled.

Not covered: any device. The `current_status.md` P1 item *Run the TCP shell
hardening on a device* lists the three checks.

### Verified locally on 2026-10-01 (backup format v2: instance settings; uncommitted on `dev`)

The option chosen in *Instance settings in the backup*, under Investigations:
the snapshot format goes to version 2 and records each control device's instance
settings, and `restore instances` puts them back. Nothing touched a bus.

- **Model and codec.** `DaliInstanceSettings` holds type, enabled, event
  scheme, priority, filter and the three instance groups, each optional. A
  snapshot holds up to 64 of them, each tied to its device's entry index. The
  wire format puts the instance count in the old reserved byte 6 and appends
  12-byte instance records after the entries. The decoder takes version 2
  only, and validates every instance record before writing anything: it must
  name a device entry, appear once, use known flag bits and hold sendable
  values.
- **Read, decide, locate, write** in `dali_restore`. The read takes about
  eleven queries per instance. It records "disabled" only when QUERY INSTANCE
  ENABLED is silent *and* QUERY INSTANCE STATUS shows instanceActive clear, so
  a lost YES is never saved as disabled. The write mask names each recorded
  field the device reports differently or not at all, never a priority outside
  2-5. A device is located by identification number, with contested addresses
  excluded. The write sends one DTR-load-plus-send-twice sequence per field,
  ENABLE or DISABLE last.
- **Shell.** `backup save` reads every instance of every recorded control device
  inside its bus claim and names any it left out or that did not answer.
  `backup status` lists them under their device. `backup import` names a
  version-1 blob when it refuses one. `restore instances [apply]` prints the
  differing fields as `now -> recorded`, writes and reads back per instance,
  and is gated like `restore groups apply`.
- **ESPHome.** No code change beyond a comment: the persisted record is sized by
  `DALI_SNAPSHOT_BLOB_MAX` and grows with it.

Results:

- 34/34 host suites. `test_snapshot` has 6 new vectors (to 32) and its
  full-snapshot vector now fills the instance table too. The new suite
  `test_restore_instances` has 15, against a mock control device with real
  registers that applies the writes, so the read-back vectors say what a device
  would report.
- `dali_test.yaml` compiles on ESPHome 2026.9.0. Static RAM is 141,728 B,
  +3,072 B: the 768-byte instance table twice in blob form and the 1,536-byte
  model once.
- The native IDF 6.0.1 build passes and carries the new verb strings.

Not covered: any bus, and no mutation pass. Every backup and restore bus result
recorded before this entry was taken in the version-1 format.

### Verified locally on 2026-10-01 (event-source matching and instance groups; uncommitted on `dev`)

The work the ninth session's finding led to: sensors match events of every
scheme, and instance groups can be read, addressed and dispatched on. Reasoning
in *The Steinel's events lost their device address*, under Investigations.
Nothing touched a bus.

- **`dali_event_source`**, a new pure-C module: the matcher, the scheme-change
  test, and the five-query profile read (type, scheme, three instance groups)
  with its parser. `test_event_source` holds 19 vectors, among them the 2k
  Steinel's two heartbeats in both schemes it has been seen in.
- **The integration** asks each sensor's matcher instead of comparing a
  Device/Instance address. A source profile per sensor is read after boot,
  after a scan, after a shell workflow and after an `iconfig` to its device:
  one read at a time, only while the scheduler queue is at most half full. A
  read cancelled by a scheduler reset keeps the old profile. An event in a
  scheme the profile did not expect marks the sensor for a re-read, at most
  every five minutes. A non-2 scheme is reported once, and a change whenever it
  happens.
- **Instance selectors.** `dali_build_instance_command()` accepts `0x80|G` and
  `0xC0|T`; `iquery` and `iconfig` on both surfaces take `gN`, `tN` and `all`.
  A new shell hook, `instance_config_applied`, lets a TCP-shell `iconfig`
  trigger the profile re-read; the console path does it directly.
- **Discovery** reads each instance's event scheme, priority and three instance
  groups, shown by `instances` and `export inventory`. The record's three enum
  fields are now stored as bytes to pay for that (see the RAM figure below).
- **Dispatch keys** gain `group_kind` and an instance-type match; YAML gains
  `address_kind: instance_group` / `device_group` and `instance_type:`, and
  `export config` writes them back.
- **`dali_cmd_instance_group()` removed.**

Results:

- 33/33 host suites. New vectors beyond `test_event_source`: 1 in `test_cli`,
  1 in `test_protocol`, 2 in `test_discovery`, 2 in `test_dispatch`. Every
  `test_dispatch` key initializer gained the three new fields, since a
  positional initializer that stops early fails `-Wmissing-field-initializers`.
- `esphome config dali_test.yaml` is valid with two new dispatch rules
  exercising `instance_group`, `instance_type` and `device_group`. Three
  deliberately invalid copies are refused with the intended messages:
  `instance_group` on a legacy frame, `instance_type` on a legacy frame, and a
  short-address rule naming both an instance and an instance type.
- `dali_test.yaml` compiles on ESPHome 2026.9.0, and the firmware carries the
  new strings. Static RAM is 138,656 B. The new instance fields alone had made
  it 143,776 B, because the shell caches 16 devices of 32 instance records;
  narrowing the three enum fields to bytes brought it under the 139,416 B of the
  build before them.
- The native IDF 6.0.1 build passes and carries the new shell strings.

Not covered: any bus, and no mutation pass. The group and type selectors rest
on TI's decoder and recall of the standard, so their first bus run is also
their first independent check.

### Verified on hardware 2026-10-01, ninth session (2k bus: the Steinel's event scheme, `dev`)

The operator reported the Steinel's occupancy as noticeably slower over the
preceding days. Driven from the `dali-shell` script against the 2k node, with
the ESPHome logger at DEBUG. The node was reflashed before the session; the ref
is not recorded, and nothing found depends on it.

`instances 0`:

```text
 0: type=4 light, usable=standard, source=standard, enabled=yes, resolution=11, status=0x02
 1: type=3 occupancy, usable=standard, source=standard, enabled=yes, resolution=2, status=0x02
 2: type=0 generic, usable=unverified, source=standard, resolution=16, status=0x00
 3: type=0 generic, usable=unverified, source=standard, resolution=8, status=0x00
```

Instance 1, through `iquery 0 1`:

| Query | Reply | Reading |
|---|---|---|
| `type` | 3 | occupancy |
| `enabled` | yes (`0xFF`) | |
| `status` | `0x02` | active, no error |
| `error` | timeout | |
| `event-scheme` | 0, read twice | instance scheme: no device address |
| `event-priority` | 4, read twice | |
| `event-filter0` | `0x07` | |
| `occ-report-timer` | 30 | 30 s |
| `occ-deadtime` | 10 | 500 ms |
| `occ-capabilities` | `0x00` | no Part 303 range or sensitivity control |
| `occ-detection-range`, `occ-sensitivity` | 255 each | MASK |
| `input-value` | 0 | vacant, no movement |
| `input-value-latch` | timeout | nothing past a 2-bit value |

Instance 0 read `event-scheme` 0 and `event-priority` 4. `occ-hold-timer` was
not read.

- **Before the fix, no event matched a sensor.** The DEBUG log carried `rx
  type=4 inst=0 evt=1` every 3.0 s and `rx type=3 inst=1 evt=12` every 30.0 s,
  and no `event poll requested` line. These are the two heartbeats the
  2026-09-04 capture recorded as `0x008001` and `0x00840C`, decoded under the
  instance scheme. Lux published every 30 s, on its poll interval.
- **`iconfig 0 <i> set-event-scheme 2` landed on both instances.** Each printed
  `transmitted; verify with iquery`, and `iquery` then read 2 for each. This is
  the first `iconfig` write read back on a bus.
- **Events trigger polls again.** From 10:41:23.8 lux events read `rx a0
  inst=0 evt=1`, with no poll request, consistent with lux carrying
  `poll_on_event: false`. The next occupancy heartbeat arrived as `rx a0
  inst=1 evt=12` at 10:41:55.074, and `event poll requested: addr=0 inst=1
  info=12 raw=00840C` followed at 10:41:55.077. That raw value is the
  2026-09-04 frame, byte for byte. The 10:42:25 heartbeat did the same.

Not covered:

- A vacant-to-occupied transition after the fix. Every occupancy event in the
  log is the 30 s heartbeat, `evt=12`.
- The poll's result. The component logs none, and the occupancy entity's own
  state line does not appear in the log, where lux, temperature and humidity
  do.
- What set both instances to scheme 0. See *The Steinel's events lost their
  device address*, under Investigations.
- Whether the scheme survives a bus power cycle.

### Verified locally on 2026-09-30 (shared-address detection; uncommitted on `dev`)

Implements the fix proposed in *Two units that answer alike share an address
invisibly*, under Investigations, for the eighth session's finding below.
Nothing touched a bus.

- `discovery_enrich_device()` keeps the Bank 0 identity read's error. On
  `DALI_ERR_RX_ACTIVITY` it reads once more, and a second collision sets
  `has_identity_collision` on an entry that stays `present`. Silence and
  malformed replies are not classified. The walk counts the address in
  `identity_collision_count`, from the status path and from the device-probe
  path, where a decoded QUERY GROUPS can find gear too.
- `dali_discovery_gear_address_contested()` is the one test for a contested
  gear address, undecodable or collided. The planner's collector, the
  snapshot, the commissioning audit's occupancy and `backup save` read it.
- The planner reports every reserved address as a new `contested` conflict,
  whether or not a move wanted it, in both address spaces and in the group
  plan. The proposal had only the reservation. Without the report the bench
  would have lost `restore plan`'s one pointer to a3, since a reserved address
  is no longer reported as `identity unknown`.
- The snapshot leaves a collided gear entry out rather than recording it as one
  unanchored unit. The device entry at the same number is kept.
- The audit counts a collided address as contested, not confirmed. That is also
  what an equal-random-address collision between two units of one product
  looks like after the walk.
- `shell_fill_missing_identities()` retries collided addresses too, through the
  new `dali_discovery_inventory_store_identity()`: a read that decodes there
  withdraws the collision.
- Shell: `discover` and `scan` append `, contested` to the unit's line and print
  a note naming the address with the remedy. `inventory` marks and counts it,
  `backup save`'s contested block covers both kinds, and the restore remedy line
  follows a `contested` conflict as well as `target contested`. The ESPHome scan
  logs the addresses as warnings.
- 12 new vectors: 5 in `test_discovery` (to 68), 4 in `test_restore_plan` (to
  41), and 1 each in `test_restore_groups` (20), `test_snapshot` (26) and
  `test_commissioning_audit` (18). Two restore vectors that read `conflicts[0]`
  now find their conflict by kind. 32/32 suites pass.
- 13 mutants, each killed: no retry; retry on any failure; never classify;
  classify any failure; each of the two walk paths not counted; the helper
  ignoring collisions; `store_identity` keeping the flag; no `contested` report
  in either plan; and the collector, the snapshot and the audit each reading
  undecodable activity only.
- The native IDF 6.0.1 build and a `dali_test.yaml` compile on ESPHome 2026.9.0
  both pass, and both images carry the new strings. The five changed C files
  re-run through their IDF compile commands, and `dali_scan.cpp` through its
  ESPHome one, with `-fsyntax-only`: no diagnostics.

Not covered: any bus. In particular, whether a collided identity read arrives
as `RX_ACTIVITY` at all; a collision that reads as silence or as a malformed
reply is still missed.

### Verified on hardware 2026-09-30, eighth session (2k bus: two lamps on one short address, `dev`)

The bench that two P1 items in `current_status.md` asked for: two drivers put
on one short address on purpose, after a `backup save`. Run twice. The operator
pasted the opening of the first run and all of the second; the first run's
clear and walk are reported, not pasted. `discover` printed the version as
`v2.0`, which only `e46216e` onwards does, so the build carries the DTR0
read-back. The exact ref and the surface are not recorded.

Bus at start: 5 LED gear at a0–a4, all off and in group 0, and control devices
at d0 and d1. `backup save` recorded `7 entries from 5 address(es)`, stored 141
bytes, and named no unanchored entry.

- **`config-dtr0 a4 set-short-address-dtr0 7` put a4's lamp on a3**, on top of
  the lamp already there. 7 is `(3 << 1) | 1`, the gear encoding of a3. The two
  lamps are the same product in the same state: status `0x00`, level 0, group
  0, version 2.0.
- **Every walk read a3 as one unit and the other lamp as gone.** `discover` and
  `scan` listed a0–a3 and `4 device(s) found`, with a3 `present, LED,
  status=0x00, v2.0, level=0, groups=[0]` and no contested note. The commission
  pre-scan read `occupied=4`, and the walk found no unaddressed gear. Nothing
  named a3, and the operator's reading was that a lamp had vanished with no way
  to tell where. Why, read from source and not captured: two units answering
  in step with the same byte superimpose into one valid frame, and the scan
  calls an address contested only when QUERY STATUS fails to decode. See *Two
  units that answer alike share an address invisibly*, under Investigations.
- **`restore plan` was the only output that pointed at a3.** On the second run
  it read `5 matched, 5 already correct, 0 move(s)`, meaning a0–a2 and d0–d1,
  and three conflicts: `gear a3: identity unknown`, `gear a3: not on bus` and
  `gear a4: not on bus`. The Bank 0 read at a3 failed, as two different
  identification numbers answering at once should. No move went onto a3, so the
  planner stayed safe, but through its unidentified-unit path: the contested
  reservation was never set. `target contested` could not have appeared,
  because the unit the backup places on a3 is one of the two sharing it, so no
  move aims there.
- **`address 3 clear` freed both lamps through the single-unit path.** It
  printed the anchored-entry note for a3, `a3 -> unaddressed (DTR0=255)` and
  `a3 cleared -- gear on the bus now reports no short address`. The contested
  arm did not open. No `DTR0 needed a second load` line, so the read-back
  passed on the first load with both units answering it: DTR0 is loaded by
  broadcast, so they held the same value. That is the read-back's first bus
  reading. The broadcast QUERY MISSING SHORT ADDRESS read one decoded YES from
  two unaddressed units, so `more than one unit` did not appear: every unit
  answers YES with the same byte. The fifth session left that third reading
  unseen; it now looks like one that cannot be relied on.
- **`commission unaddressed` separated them.** It read `occupied=3`, gave short
  3 to random `0x6E0DE5` and short 4 to `0xD81575`, with QUERY SHORT ADDRESS
  echoing `0x07` and `0x09`, and its post-scan found 5 and confirmed 2 of 2. By
  the operator's report, the first run's clear and walk did the same.

Not covered:

- Which lamp the walk put on a3 and which on a4. No `restore plan` after the
  second walk is in the output.
- Which error the Bank 0 read at a3 returned: `identity unknown` covers every
  failure.
- `address <aN> clear`'s contested arm, `target contested` and the post-scan
  audit's contested path. Units that answer alike reach none of them.

### Verified locally on 2026-09-30 (DTR0 read-back, clear lines, version display; uncommitted on `dev`)

Implements the fix proposed in *A lost DTR0 load re-addresses a unit to
whatever DTR0 last held*, under Investigations, and two findings of the seventh
session below. Nothing touched a bus.

- `dali_restore_write_short_address()` sends DTR0 and QUERY CONTENT DTR0 at the
  unit as one sequence, and SET SHORT ADDRESS as a second only when the unit
  read the value back. After a wrong, silent or unreadable read-back it loads
  once more, then gives up with nothing further sent. `restore apply` and all
  four `address` arms write through it. The frames still come from
  `dali_restore_build_move_sequence()`, so that builder's vectors keep pinning
  them.
- `dali_restore_confirm_move()` asks the source when the target is silent.
  `TARGET_SILENT` now means the unit stayed, and the new `UNIT_MISSING` that it
  answers at neither end. `address set` in both spaces reports the same
  distinction.
- `restore apply` reports a move to the integration when it is confirmed, no
  longer when it is sent. The `config_applied` call is gone, and with it the
  device log's `short address changed ... stale until the next scan` before
  every restore move.
- The ESPHome clear lines say what changed: the integration stopped polling the
  address for group state, and the gear keeps its groups.
- `discover` and `inventory` print the version as major.minor through
  `dali_cli_format_gear_version()`, and anything below 2.0 as the raw byte.
- `test_restore_confirm` grew from 17 vectors to 33. The mock bus now moves
  units as SET SHORT ADDRESS would, and can make the unit miss a DTR0 load or a
  pair. One vector pins the hazard on the builder's bare frames: a unit that
  misses the load lands on a5, on top of the unit there. The write's vectors
  run the same bus and leave every unit where it belongs. `test_cli` gained two
  vectors, to 89. 32/32 suites pass.
- Six mutants, each killed: the read-back always accepted (5 vectors fail), no
  second load (5), no source probe after a silent target (1), a collided
  read-back never allowed (1), `pair_attempted` never set (1), and the version
  halved again (1).
- `dali_shell.c`, `dali_cli.c` and `dali_restore.c` type-check clean with the
  IDF 6.0.1 flags of `dali_group_map.c` from `build/compile_commands.json`.
  `dali_component.cpp` type-checks clean with the flags of the 2026-08-13
  `dali-1k` ESPHome build tree, whose headers predate 2026.9. An error injected
  into each edited function was caught by both checks.

Not covered: any bus; an ESPHome compile at 2026.9.1; any control device
answering QUERY CONTENT DTR0 (`0x36`), which the device arms now depend on.

### Verified on hardware 2026-09-29, seventh session (1k bus: `identify`, `backup save`, `restore apply` reporting and its stop, `dev`)

The bus run the three local entries below asked for, on 1k, where the lamps sit
in seven groups. Driven from the `dali-shell` script against the 1k node, with
the device log captured alongside. The user reports the build as the latest
commit, `398eda1`. The device log reads ESPHome 2026.9.1, compiled 23:12 that
evening; Home Assistant and the workstation had both just moved to 2026.9.1.

Bus at start: 16 LED gear at a0–a15, in groups 0 and 2–7. The lamps at a2 and
a5 are in g6 and g0.

- **`identify` puts a mid level back.** From level 160 at a0, `identify 0`
  ended `identify: done, level 160 restored`, and the next `discover` read a0
  at 160.
- **`backup save` names gear it cannot record.** With a5 cleared, it read
  `recorded 15 entries from 15 address(es)`, then `gear on the bus reports no
  short address and is NOT recorded here` and the remedy. Once `commission
  unaddressed` had put the unit back, it recorded 16 of 16 and printed neither
  line. The broadcast QUERY MISSING SHORT ADDRESS behind the note read YES,
  then silence.
- **An unaddressed unit acts on its group commands.** a5 was cleared and
  re-commissioned twice. Each walk assigned a5, the lowest free address, with
  fresh randoms `0x4CDA55` and `0x3749A2`, and each post-scan confirmed 1 of 1.
  Between the second clear and its walk, `level g0 90` went out. The next
  `discover` read a5 at 90 in group 0, where the `off g0` before the clear had
  left it off. A cleared unit keeps its group registers, as `dali_commands.md`
  says, and still acts on them. This is the first bus reading of the second
  half.
- **The device log says the opposite.** Each clear logged `a5 cleared: retired
  from every group it was known in`, then `a5 no longer answers ... the unit
  comes back only through 'commission unaddressed'`. What was retired is the
  integration's group-map entry, which only chooses the address a group light
  polls. The user flagged the first line as misleading; the second overstates
  the same way. P1 item in `current_status.md`.
- **A swap by hand, and a restore that stopped.** `address a5 set a16`,
  `address a2 set a5` and `address a16 set a2` swapped the two lamps. Each move
  was confirmed and logged `group membership followed the move`. `restore plan`
  read `16 matched, 14 already correct, 3 move(s)`, staging a2 -> a16, the
  lowest free address. The first `restore apply` confirmed that hop, then
  printed `2/3 a5 -> a2: sent, not confirmed: nothing answers at the target`
  and `stopped after 1 of 3 move(s)`, and sent nothing further. That is the
  first bus run of the stop after a failed move, which the 2k entries below
  list as host-tested only. The next `restore plan` read 2 moves, a5 -> a2 and
  a16 -> a5, so the unit at a5 had not moved. A second `restore apply`
  confirmed both, and `discover` showed every lamp at its recorded address and
  in its recorded group. Why the move failed is under Investigations, *A lost
  DTR0 load re-addresses a unit to whatever DTR0 last held*.
- **Each confirmed gear move reaches the integration.** The device log has
  `group membership followed the move` for each of the three restore moves
  that were confirmed, and nothing of the kind for the one that was not.
- **Every restore move also logs `short address changed ... stale until the
  next scan`, before it is confirmed.** `shell_restore_apply_move()` calls the
  `config_applied` hook as soon as the frames are sent. The line was false for
  the move that failed, and for the three that landed the next line
  contradicts it. `address` makes no such call and logged none.
- ESPHome logged `dali took a long time for an operation (338 ms)` once, at
  23:27:43, before the first clear. Not investigated.
- `dali_test.yaml` passes `esphome config` on 2026.9.1 on the workstation.

Not covered:

- `identify` from max and from off, and the ESPHome Identify button.
- Which of the failed move's three frames the unit missed.
- `restore groups`, and every contested-address path.

### Verified locally on 2026-09-29 (`identify` puts the level back; uncommitted on `dev`)

Fixes the `identify` report in the sixth-session entry below. Nothing touched a
bus.

- `cmd_identify()` reads QUERY ACTUAL LEVEL before blinking. Afterwards it
  sends OFF for 0 or DAPC for 1–254 through the abort-exempt path, so a front
  end that disconnects mid-blink still gets the lamp put back. A failed read,
  or MASK, means no restore: the shell says so before the blink, and the lamp
  ends at min as before.
- The ESPHome Identify button queues the same query in its first slot and the
  same restore when its 10 s are up. It relies on the scheduler running its
  queue in order, so the query answers before the first half-blink, and on
  every completion callback firing, a reset included. When the level is
  unknown it ends on RECALL MIN LEVEL, as the shell does. A refused final frame
  is retried for up to 2 s, then logged. The address is taken once, at the
  start.
- `dali_shell.c` and `dali_cli.c` type-check clean with the IDF 6.0.1 flags,
  and an error injected into the new restore call was caught.
  `dali_component.cpp` type-checks clean with the flags of the 2026-08-13
  `dali-1k` ESPHome build tree, whose ESPHome headers predate 2026.9. The new
  code uses no ESPHome API beyond logging and `millis()`. An error injected
  into it was caught.
- 32/32 host suites pass after the help-text change in `dali_cli.c`. No host
  vector reaches either identify path.

Not covered: any bus, and an ESPHome compile at the current version.

### Verified on hardware 2026-09-29, sixth session (1k bus: reply-window reference, `dev`)

The first bus result for the frame-end reference that the stack-review fixes
introduced. One narrow capture, on the 1k node the user had just flashed with
current `dev`. The exact ref is not recorded. The user expects it to include
`985b6c6`, the commit that carries the fixes in the entry below.

```text
> query a13 groups-0-7
groups-0-7: 0x08
tx 0x1BC0  timestamp_us 75934772
rx 0x08    timestamp_us 75949062  since_tx_us 14290
```

- The reply was accepted on the first attempt: two records, no retry. `0x08`
  is group 3, which a13 has read since August.
- In August the same query to the same gear read `since_tx_us` 12742, 12936
  and 13120, measured from the old stamp after the stop bits. Moving the stamp
  to the frame end predicts those plus 1664, which is 14406–14784. The reading
  is 116 us under that span, and 1170–1548 us above the August readings. a13
  wandered 378 us across its three August samples, so one sample cannot pin the
  offset closer than that. It does place the stamp: a scheduler that had fallen
  back to the task clock at TX return would read below the August figures, not
  above them.
- From the frame end, a13 settles in 14290 − 7500 = 6790 us. That is inside
  5.5–10.5 ms and near the 7 ms nominal. The August samples, re-anchored, give
  6906–7284 us.
- The 2026-09-25 local entry named `dali_phy_tx()`'s completion loop and the
  ISR's frame-end stamp as what a bus should see first. Both have now run:
  every frame the node sends goes through that loop, and this capture shows the
  stamp it leaves.

Not covered: the undecodable edge, moved to 5.5 ms from the frame end, which
judges overlapping replies rather than a single decoded one; the decoded edge
at its boundary (3.664 ms), which a13 clears by about 3 ms; and the 28.664 ms
close.

The user also reported that `identify` leaves the lamp at min level, whether it
started at max or off. That is what the code does. `cmd_identify()` sends
RECALL MAX LEVEL and RECALL MIN LEVEL five times each, 1 s apart, ending on
MIN, and never reads the level it started from. The ESPHome identify button is
built the same way: it starts on MIN, sends a half-blink every 500 ms for 10 s
and restores nothing, and which half it ends on depends on loop timing. The
button is code reading only. Fixed in the entry above.

### Verified locally on 2026-09-29 (`restore apply` reports its moves, `backup save` warns about unaddressed gear; uncommitted on `dev`)

Two fixes to `dali_shell.c` for what the fifth 2k session found. The first is
described under Investigations (*`restore apply` publishes the scan it planned
from*). Nothing touched a bus.

- `restore apply` calls `short_address_moved(from, to)` after each gear move it
  confirms, in plan order. It no longer publishes the planning scan through
  `inventory_changed` when it finishes. An unconfirmed move reports nothing,
  and device moves report nothing, as before. The hook's contract in
  `dali_shell.h` now names both callers.
- `backup save` sends a broadcast QUERY MISSING SHORT ADDRESS inside its bus
  claim, after the identity reads. A YES prints that gear on the bus has no
  short address and is not recorded, with the remedy. RX activity reads as
  more than one such unit. An unreadable answer says a unit may be missing.
  The save still completes. The control-device space has no counterpart,
  because Part 103 QUERY MISSING SHORT ADDRESS is not implemented.
- `dali_shell.c` type-checks clean with the IDF 6.0.1 flags of
  `dali_group_map.c` from `build/compile_commands.json`. The committed version
  is also clean, so no diagnostic is new. The same check flagged an error
  injected at the edited line, which shows it compiles the device code rather
  than skipping it.
- No host vector covers either change, because no suite links the shell. No
  host suite includes `dali_shell.h` either, so the 32 suites were not re-run.
  On the ESPHome side only a comment in `dali_component.cpp` changed.

Not covered: any bus, and an ESPHome compile.

### Verified on hardware 2026-09-29, fifth session (2k bus: move-aside, `address` clear, backup export/import, `dev`)

Covers the last address-restore path the entries below left without a bus
result: moving aside a unit the backup has never seen. It is also the first bus
run of `address <aN> clear`, `backup export` and `backup import`. Driven from
the `dali-shell` script against the 2k node. The output carries the same
markers as the entries below; the exact flashed ref is not recorded.

Bus at start: as the entry below left it, with every unit at its recorded
address.

**2k's d1 is the Casambi CBU-DCS kept there for testing**, confirmed by the
operator in this session. Its GTIN in the backup is 6430082060120 (GS1
Finland), and its identification number is `CA5A000E000000FF`. Entries below
that call d1 "the DALI-2 PB coupler" or "the coupler" mean this unit. Nothing
they measured changes, only the name.

The fixture was built in four steps: export the backup, clear a4's lamp, save
a new backup without that lamp, then free a3 so that `commission unaddressed`
puts the lamp there. That leaves a unit no backup knows holding an address a
recorded unit is owed.

- **`backup export`** printed the 7-entry backup as an import script of 141
  bytes.
- **`address a4 clear` worked, and the missing-address check gave the two
  readings one unit can produce.** It printed the stored backup's anchor note,
  sent DTR0 = 255, and reported `a4 cleared -- gear on the bus now reports no
  short address`. The broadcast QUERY MISSING SHORT ADDRESS read silence before
  the write and a decoded YES after. That is the check's first bus reading. The
  third reading, RX activity from several missing units, did not arise.
- **`backup save` recorded `6 entries from 4 address(es)`, and said nothing
  about the lamp it could not see.** See the P1 item in `current_status.md`.
- **`commission unaddressed` put the cleared lamp on a3.** First `address a3
  set a4` (DTR0 = 9) freed a3. The walk then read `occupied=4`, found random
  `0x5B529F` and assigned short 3, with QUERY SHORT ADDRESS echoing `0x07`. Its
  post-scan confirmed 1 of 1. The walk takes the lowest free address, and a3
  was it.
- **`restore plan` moved the unknown unit aside before placing the recorded
  one.** It read `6 matched, 5 already correct, 2 move(s)`: `a3 -> a5 (not in
  the backup, moved aside)`, then `a4 -> a3`, with the conflict `gear a3: not in
  backup`. `restore apply` confirmed both moves. The moved-aside lamp was
  confirmed by its own identification number, read at a5.
- **The operator reading holds up.** The P1 item that asked for this run
  warned that a displacement and a placement look alike in the apply log. They
  do (`1/2 a3 -> a5: OK`, `2/2 a4 -> a3: OK`), but `apply` reprints the plan,
  note included, directly above them. The one line that could mislead is the
  conflict: it names a3, the address the unit was leaving, which a recorded
  lamp holds once the apply finishes.
- **`backup import` brought the 7-entry backup back intact.** It printed `7
  entries from 141 byte(s)` and `stored (141 bytes)`. The decode checks the
  exact length, so every chunk arrived exactly once. The paste displayed
  garbled on screen. The terminal echoed the part of the paste that arrived
  while the script was waiting for the device's answer to `begin`. The script
  then read the buffered lines one at a time, and nothing was sent twice.
- **The imported backup drove the last restore.** It read `7 matched, 6
  already correct, 1 move(s)`, `a5 -> a4`, and the move was confirmed.
  `discover` then showed every unit where the session found it, with a4 in
  group 0. The lamp kept its group registers through the clear and the
  re-commission, as `dali_commands.md` says a de-addressed unit does.

Found while reading the apply path for this entry: `restore apply` hands the
integration its pre-move scan. See *`restore apply` publishes the scan it
planned from*, under Investigations. 2k cannot show it, because every lamp
there is in g0.

Not covered:

- The stop after a failed move, and a contested target in either planner.
  Both are host-tested only.
- `address <aN> clear` against a shared address, and `address <dN> clear` on
  any unit.
- `restore groups`.

### Verified on hardware 2026-09-29, fourth session (2k bus: swaps in both spaces, `dev`)

Covers two gaps in the entry below: no gear move had run under `restore
apply`'s confirmation, and the planner's staging hop had never sent a frame.
Driven from the `dali-shell` script against the 2k node. The output carries the
same markers as the entry below; the exact flashed ref is not recorded.

Bus at start: as the entry below left it, with every unit at its recorded
address.

- **A device swap is staged through a spare address.** `address d0 set d2`,
  `address d1 set d0` and `address d2 set d1` swapped the Steinel and the
  Casambi, with device DTR0 2, 0 and 1, each one confirmed. `restore plan` read
  `7 matched, 5 already correct, 3 move(s)`. The Casambi went d0 -> d2
  `(staging, placed by a later step)`, then the Steinel d1 -> d0, then the
  Casambi d2 -> d1. `restore apply` confirmed all three.
- **A gear swap is staged the same way, and gear moves now run under the
  confirmation.** `address a0 set a5`, `address a4 set a0` and `address a5 set
  a4` swapped the lamps recorded at a0 and a4. DTR0 was 11, 1 and 9, which is
  `2N+1` as on 2026-09-03. The plan staged a0 -> a5, then placed a4 -> a0 and
  a5 -> a4, and `restore apply` confirmed all three. For gear, the
  confirmation reads the identification number over Part 102 memory, so both
  of its read paths have now run.
- Each plan staged through the lowest free address in its space, d2 and a5,
  which is the rule in `restore_find_spare()`. No recorded unit was missing,
  so the P1 defect in that function (it can pick a missing unit's recorded
  address) had no chance to show.
- **`address` refuses an occupied destination.** `address a0 set a3` answered
  `a3 already answers; refusing to move a0 onto it`. The 2026-09-03 session
  had only seen this check pass.
- **The session ended with the bus matching the backup.** `backup status`
  listed the 7 entries as `loaded from storage`. `restore plan` read `7
  matched, 7 already correct, 0 move(s)` and `bus matches the backup; nothing
  to do`.

Not covered:

- No move failed, so the stop after a failed move is still host-tested only.
- No unit that the backup has never seen was on the bus, so the move-aside
  path has not sent a frame.
- Neither `address aN clear` nor `address dN clear` ran.

### Verified on hardware 2026-09-29, third session (2k bus: device-space moves, `dev`)

The device-space moves that the entry below left with host vectors only. Driven
from the `dali-shell` script against the 2k node. The build carries `48009a8`
and the stack-review fixes: `address` printed `device DTR0=2` for d2, and
`restore apply` ended with `each confirmed on the bus`. The exact flashed ref is
not recorded.

Bus at start, per `discover`: a0–a4 lamps in group 0; d0 the Steinel (4 input
instances), d1 the Casambi CBU-DCS (1).

- **`address dN set dM` loads DTR0 raw and lands.** `address d1 set d2` ran
  first, then `address d0 set d1`. Each loaded DTR0 with the destination and
  ended `dM confirmed, dN silent`. On `v2.0.0` the first would have sent the
  Casambi to d5.
- **`restore plan` found both devices by identification number at their new
  addresses, and put the way back in order.** After the first move it read
  `7 matched, 6 already correct, 1 move(s)`, d2 -> d1. After the second it read
  `5 already correct, 2 move(s)`, with d1 -> d0 ahead of d2 -> d1. The Casambi
  cannot return to d1 until the Steinel has left it.
- **`restore apply` made both moves and confirmed each one before sending the
  next.** It printed `1/2 d1 -> d0: OK`, `2/2 d2 -> d1: OK`, and `2 move(s)
  applied, each confirmed on the bus`. Each `OK` means
  `dali_restore_confirm_move()` passed: the destination answered QUERY NUMBER
  OF INSTANCES, the source was silent, and the Bank 0 identification number
  read at the destination matched the backup's. This is the first bus run of
  the confirmation and of any device-space `restore apply`. It also met the
  case the confirmation was added for (*`restore apply` does not confirm a move
  before the next one depends on it*, under Investigations).
- The lamps read `status=0x04, level=85` again. The `0x00, 0` that the entry
  below left unexplained was the lamps being off (status bit 2 is lamp arc
  power on), not an effect of the reflash.

Not covered:

- No move failed, so stopping after a failed move has host vectors only.
- No gear move has run under the confirmation. The one gear move on record
  predates it.
- No cycle arose, so the planner's staging hop has not sent a frame.
- `address dN clear` did not run.

No `restore plan` followed the apply. It was not needed: the two confirmations
had already read both identification numbers at their recorded addresses.

### Verified on hardware 2026-09-29, later (2k bus: control-device commissioning, `dev`)

The first bus run of the Part 103 commissioning walk, and the recovery the
earlier 2k entry left open. Driven from the `dali-shell` script against the 2k
node on a `dev` build carrying the encoding fix, committed as `48009a8`; the
exact flashed ref is not recorded.

Bus at start: a0–a4 lamps; the Steinel and the PB coupler both unaddressed.

- **`commission devices` found both devices with INITIALISE `0x7F`.** Pre-scan
  `occupied=0`; the search found random `0xE1F4F7` and programmed it to d0,
  then `0xFE0E00` to d1; `no more unaddressed devices`; TERMINATE. The post-scan
  found 5 addresses and confirmed 2 of 2 assignments. This settles INITIALISE
  on the bus, the first control-device COMPARE, SEARCHADDR, PROGRAM SHORT
  ADDRESS and VERIFY traffic, and the device post-scan. The walk takes its
  quiescence bracket unconditionally and printed no bracket failure.
- **`discover` put each unit back where it was:** d0 with 4 input instances,
  the Steinel, and d1 with 1, the coupler. The Steinel drew the lower random
  address and the walk assigns in search order, so the original layout came
  back by chance.
- **`restore plan` matched all 7 units to the backup taken before the earlier
  session** — `7 matched, 7 already correct, 0 move(s)` — so it read both
  device identification numbers through the new addresses.

Not covered: no device-space move ran, so the raw DTR0 in `address dN set` and
in `restore apply` has host vectors only. Equal random addresses did not arise.
The lamps read `status=0x00, level=0` against `0x04, 85` in the earlier
session. Nothing in this session addressed gear, and the node was reflashed in
between; the change is not attributed here.

### Verified locally on 2026-09-29 (Part 103 encoding fix, uncommitted on `dev`)

The fix for the two encodings the next entry found wrong on 2k. Nothing touched
a bus.

- `address d<N> set d<M>`, `address d<N> clear` and `restore apply` build their
  frames with the new `dali_restore_build_move_sequence()`: device DTR0 raw,
  gear DTR0 `(a << 1) | 1`, `0xFF` unencoded in both. Before, no host suite
  linked the shell, so nothing pinned those frames — which is how both writers
  loaded the gear form into devices unseen.
- `DALI_DEVICE_INITIALISE_UNADDRESSED_PARAM` is `0x7F`. The device-commissioning
  mock used to assert the constant, so it passed whatever the walk sent; it now
  selects devices by the parameter the way a device does (`0xFF` all, `0x7F`
  unaddressed, `0..63` that address), and a new vector puts an addressed device
  at d0 beside an unaddressed one.
- 32/32 host suites build and pass with MSYS2 UCRT64. `test_restore_confirm`
  has 17 vectors (4 new: device, gear, clear, bad arguments);
  `test_device_commissioning` 30 (1 new, 1 renamed).
- Mutation-checked. Restoring `0x00` fails 12 device-commissioning vectors —
  the in-order assignment reads "Expected 2 Was 0", which is the 2k result —
  and the d0 vector sees the addressed device taken. Restoring the gear
  encoding for devices fails the device-move vector (`0xC13005` for d2).

### Verified on hardware 2026-09-29 (2k bus: both Part 103 addressing encodings, `v2.0.0`)

Settles the two open encodings in *A device-side source sides with Beckhoff on
both Part 103 encodings*, under Investigations; that entry stays as written.
Driven from the `dali-shell` script against the 2k node, which the operator
reports runs `v2.0.0`. Nothing in the code changed.

Bus at start: a0–a4 lamps in group 0; d0 the Steinel (4 instances, id
`05259F98068FE190`), d1 the DALI-2 PB coupler (1 instance, id
`CA5A000E000000FF`). `backup save` recorded 7 entries from 5 addresses,
including both device identities.

**Device SET SHORT ADDRESS (DTR0) reads DTR0 raw.** `address d1 set d2` loaded
device DTR0 = 5 and reported d2 silent. `raw 05FE35 len=24 wait` (QUERY NUMBER
OF INSTANCES at d2) timed out; `raw 0BFE35 len=24 wait` (d5) answered 1. The
coupler went to 5, as TI, Tasmota and Beckhoff have it, so `(a << 1) | 1` is
wrong for the device space. On `v2.0.0` and `dev` alike, `address dN set dM`
sends a device to 2M+1, and every device-space move in `restore apply` does the
same.

**The way-back recipe made a collision.** The investigation entry's "back from
d5" frames return a unit to d0, because they were written for the Steinel.
Run against the coupler, `dtrcheck 5 0 0` and `raw2 0BFE14 len=24` put it on d0
beside the Steinel. `raw 01FE35 len=24 wait` then read `ERR malformed`;
`discover` listed neither d0 nor d1 as an input device and noted 2 RX
observations ignored. The recipe should have said "to the address it came
from".

**`address d0 clear` could not reach its contested arm.** It answered "d0 does
not answer; nothing to clear": the presence probe read the two colliding
replies as a timeout, not as undecodable activity, and only the latter opens
the contested path. A collision of these two units is silence to the probe.

**Cleared by hand**: device DTR0 = `0xFF` (`raw C130FF len=24`), then SET SHORT
ADDRESS at d0 sent twice (`raw2 01FE14 len=24`). `0xFF` means "none" under
either encoding. The `commission devices` pre-scan that followed read
`occupied=0`, so both units left d0 and neither holds an address.

**INITIALISE (device) `0x00` does not select unaddressed devices.** With both
units unaddressed and no other control device on the bus, `commission devices`
reported `no more unaddressed devices`, `assigned=0`. Every Part 103 special
opcode this stack sends matches TI's device-side decoder (TERMINATE `00`
through QUERY SHORT ADDRESS `0A`, DTR0 `30`), which leaves INITIALISE's
parameter as the one input that disagrees with the independent sources. Read
as TI and Beckhoff have it, `0x00` selected the device at d0 and d0 was empty.
Not excluded: a fault further into the walk, since RANDOMISE, SEARCHADDR and
COMPARE have never run on a bus. A `0x7F` rerun separates the two.

**Left on the bus:** a0–a4 unchanged. The Steinel and the coupler both hold no
short address, so everything keyed to d0 and d1 is dark until a build with both
encodings fixed commissions them and `restore plan` / `restore apply` returns
each to its recorded address by identification number. That depends on the
backup taken at the start: a `backup save` before the restore would record both
units as absent.

### Verified locally on 2026-09-25 (stack-review fixes, uncommitted on `dev`)

Seven findings from the *Stack review* below, fixed in the working tree on top of
`7f26fef`; what changed is in *Stack review follow-up*, under Investigations.
Nothing is committed and nothing touched a bus.

- 32/32 host suites build and pass with MSYS2 UCRT64. `test_restore_confirm`
  is new: 13 vectors against a mock bus of gear and device units that tracks
  DTRs and auto-increments memory reads. With new and rewritten vectors,
  `test_scheduler` has 87, `test_cli` 87, `test_protocol` 73,
  `test_restore_plan` 37 and `test_transport` 15.
- Three mutations, each caught and reverted:
  - routing events only in `SCHED_IDLE` and `SCHED_WAIT_REPLY`, as before,
    fails `test_event_during_tx_guard_is_routed`,
    `test_24bit_rx_during_settle_is_routed` and
    `test_event_on_the_wire_before_a_queued_query_is_routed`;
  - anchoring the reply window to the TX return instead of the frame end fails
    `test_precise_phy_tx_end_is_preferred_over_delayed_task_clock` and
    `test_window_uses_frame_end_and_guard_uses_tx_return`;
  - arming the 22 Te guard from the frame end, which shortens it by the stop
    bits, fails the latter.
- The native firmware builds on IDF 6.0.1: `dali_esp.bin` is `0x474f0` bytes,
  72 % of the app partition free.
- `dali_test.yaml` compiles on ESPHome 2026.9.0 (IDF 5.5.5). Its generated
  sdkconfig has `CONFIG_GPTIMER_ISR_CACHE_SAFE`,
  `CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM` and `CONFIG_GPIO_CTRL_FUNC_IN_IRAM` set,
  plus `CONFIG_ESP_TIMER_IN_IRAM`, which the TX ISR also relies on. Static RAM
  is 138,248 B (76.5 %). The first compile read 139,000 B; reordering
  `DaliRestoreMove` so its enums lead recovered 752 B. IRAM ends at
  `0x40093464`, below the `0x400A0000` line the `sram1_as_iram` note in
  `current_status.md` depends on.
- No host vector reaches the ESPHome- and FreeRTOS-bound parts: the drain in
  `try_claim_bus()`, the console's `raw` refusal, the sdkconfig options,
  `dali_phy_tx()`'s completion loop, and the ISR's frame-end stamp. The last
  two are what a bus should see first.

### Verified locally on 2026-09-25 (host suites and CI state, `7f26fef`)

31/31 host suites build and pass with the MSYS2 UCRT64 toolchain. CI is green at
`7f26fef` on both `main` and `dev` (Host Tests, ESP-IDF Build, ESPHome Build),
and the `v2.0.0` Release Packaging run passed. No ESPHome or IDF build was run
locally and nothing touched a bus. Done as part of the stack review under
Investigations, which is where the findings are.

### Verified on hardware 2026-09-04 (2k bus: retry and event-rate counters, `b64f81f`)

One `discover` on 2k unquiesced, then `stats`; then `quiescent on all`, a second
`discover`, and a second `stats`. Lamps were off this session (`status=0x00`,
`level=0`, against `0x04`/85 the night before); five devices found on both
walks, identically, a5 still physically disconnected. The unquiesced walk noted
**36** control-device events, against 14-17 on 2026-09-03. The quiesced walk
printed no note.

Only the second interval is measurable: the two `stats` snapshots bracket
`quiescent on all` plus one quiesced walk, and nothing bracketed the first walk.
Across that interval:

| Counter | Delta |
|---|---:|
| TX retries | +132 |
| Reply timeouts | +264 |
| `rx_event_unroutable` | +0 |
| `rx_reply_early` / `late` / `superseded` | +0 |
| `unsolicited_events_routed` | +19 |
| Malformed, bus idle failures, ISR overruns | +0 |

Absolute figures at the first snapshot, used by the investigation entry: 3553
events routed, 242 unroutable, against an uptime of ~2 h 56 m — the 2k bus-status
sensor in Home Assistant last went `OK` at 22:11:29 on 2026-09-03.

What this pass establishes: `TX retries` is retired as a diagnostic for reply
loss, since a fully quiesced walk still produces 132 of them, and the 2k event
rate now has a number attached. What it does not: the unquiesced walk's counter
deltas were never captured, there is no 1k comparison, and the `7d8a4f5`
quiescence bracket is still unexercised — run 2 asserted quiescence by hand.

**Idle `capture`, same session.** 111 s, 128 records with 8 dropped, 40 events
and 23 polls. Device 0 / instance 0 emits `0x008001` every **3.000 s** — 37
intervals, all between 2998 and 3004 ms, `event_information` constant at 1.
Device 0 / instance 1 emits `0x00840C` every 30.01 s, also with a constant
payload. Both are timers rather than value reports, and they confirm the
recorded instance layout: 0 is lux, 1 is occupancy. 40 events over 111 s is
**0.36 events/s**, agreeing with the 0.36/s derived from the lifetime counters
above.

**Walk timing and a lost query, same session.** A `discover` on 2k takes **29
s**; two consecutive walks noted 33 control-device events each. Against the
3.000 s and 30.01 s timers, 29 s allows ~10.6 emitted frames, so the wire
carried at least 3.2x the idle rate. On the first of those two walks a0 was
detected as `input-device(4 instances)` but its enumeration line never printed —
`dali_discovery_query_input_device()` failed for a device the same walk had just
proved present, and the loop has no else branch, so nothing was reported. The
second walk enumerated it. Not covered: how often that happens, and whether it
stops under `quiescent on all`.

**Scan duration scales with present gear, not with empty addresses (1k, ref
unknown).** A 1k `discover` — 16 gear, 48 empty addresses, no note, no DALI-2
control devices — took **45 s**, against 29 s for a 2k walk with 5 gear and 59
empty addresses. Fewer timeouts and more time. Two measurements, two unknowns:

| | Cost |
|---|---:|
| Empty address | **~0.34 s** |
| Present device | **~1.79 s** |

(59a + 5d = 29, 48a + 16d = 45; both walks reproduce to within 0.1 s.) A present
device costs about five empty ones, because `discovery_enrich_device()` runs
groups, device type, version, actual level, a gear-profile sequence, and QUERY
NUMBER OF INSTANCES — the last of which times out on every 1k gear address,
there being no control devices on that bus. The empty-address figure implies
~85 ms per timed-out attempt (four per address) against a protocol-timing
minimum near 55 ms, so roughly a third of a walk is round-trip latency rather
than wire time.

The consequence for anything that counts events per walk: walk length is a
property of bus composition, so those counts are not comparable across buses,
nor across sessions if gear is added or removed. The ref this 1k node runs is
not known — the local build tree for `dali-1k` was last compiled 2026-08-13 and
carries neither the split counters nor the quiescence bracket, but that dates
the last compile on this machine, not the flash.

### Verified on hardware 2026-09-03, 22:20 (2k bus: split RX counters, `b64f81f`)

The experiment the seven-way counter split was built for, run on the one ref
that can still run it. `b64f81f` carries the split counters and *not* the scan's
quiescence bracket, so it is the last build able to observe the event traffic
the bracket removes; from `7d8a4f5` on, an operator-driven walk silences the
thing being measured. Driven from the `dali-shell` script against the 2k node.

Bus at start: a0 (lamp + Steinel HF 360 II, 4 input instances), a1 (lamp +
Casambi CBU-DCS, 1 instance), a2-a4 lamps, all five in group 0. a5 — the LED
driver that contested a4 earlier the same day — was physically disconnected
before this session, so five devices is the expected result, not a shortfall.

Four `discover` runs, three unquiesced and a fourth under `quiescent on all`:

| Run | Devices | early/late note | events | other note |
|---|---:|---|---:|---|
| 1 | 5 | absent | 17 | absent |
| 2 | 5 | absent | 14 | absent |
| 3 | 5 | absent | 16 | absent |
| 4, `quiescent on all` | 5 | absent | none | absent |

Every run enumerated a0-a4 identically — `present, LED, status=0x04, v4,
level=85, groups=[0]`, with 4 and 1 input instances at a0 and a1. No address
drift between runs, no undecodable activity, no contested line.

**The absent notes are the result, not the counts.** `shell_discover_bus()`
prints the early/late line whenever `early_rx > 0 || late_rx > 0`, and the
other-observations line whenever `other_rx > 0`, where `other` sums
`rx_reply_superseded`, `rx_undecodable_ignored` and `rx_ignored_unclassified`.
Neither line appeared on any run. So across three unquiesced walks of 64
addresses each: **early = 0, late = 0, superseded = 0, undecodable = 0,
unclassified = 0**, and the 14-17 reported each time was
`rx_event_unroutable` + `rx_event_no_subscriber` in full. Those counts sit
inside the 11-39 band the old undifferentiated note reported on this bus, and
every one of them is control-device event traffic. Under quiescence the note
disappears entirely — the fourth run reproducing, on a build that can name what
it is silencing, the single quiesced scan of the earlier session that evening.

The conclusion this settles is in the investigation *The 2k "late gear" reading
was event traffic* below.

What this pass does not cover: the quiescence bracket added in `7d8a4f5` — run 4
asserted quiescence by hand, which is a different code path from the bracket and
exercises none of its release-on-exit behavior; the bracket's error and
cancellation unwinds; and any bus but 2k. 1k has no DALI-2 control device
emitting events, so it can reproduce neither arm of this experiment.

### Verified on hardware 2026-09-03 (2k bus: backup/restore, commissioning, contested)

First bus result for the commissioning and backup/restore work accumulated on
`dev` since `v1.1.1`. Driven from the `dali-shell` script against the 2k node, on a `dev` build carrying the reworked backup/restore
(at or near `7e9ee6e`). The exact flashed ref is not recorded — see the
Installation State note in `current_status.md`: nothing in this repository is
evidence of what a device runs.

Bus at start: a0 (lamp + Steinel HF 360 II, 4 input instances), a1 (lamp +
DALI-2 PB coupler, 1 instance), a2, a3 and a5 plain lamps, all in group 0. Plus
one unpowered LED driver holding a4, invisible to every probe. That unit
regaining power mid-session is what produced the collision below.

**Exercised, with a result:**

- `backup save` / `backup status` — four saves across the session, 6 to 8
  entries spanning both address spaces, gear identities and group masks
  recorded. A group edit made between two saves (`0x0001` to `0x0002`) appeared
  in the later one, so the snapshot reads the bus rather than a cache.
- `restore plan` / `restore apply` — a one-move plan computed against a live
  scan, applied, and confirmed by `discover`.
- `address <aN> set <aM>` — four moves. The destination-free pre-check and the
  both-ends read-back (`a4 confirmed, a5 silent`) fired correctly on every one.
  DTR0 encodings observed as 9, 13, 11 for a4, a6, a5, all `2N+1`.
- `address add g1` / `remove g0` — write plus membership read-back.
- `commission unaddressed` — two genuinely unaddressed units, full
  INITIALISE/randomise/COMPARE walk, randoms `0x96B1F4` and `0xEC97DF`,
  post-scan confirmed 2 of 2 with `QUERY SHORT ADDRESS` echoes `0x09` and
  `0x0B`.
- `quiescent on all` / `off all`, `identify`, `meminfo`, `status`, `off`,
  `config-dtr0`. `meminfo a5` returned the same identification the backup had
  recorded independently.

**The contested classification met a real collision.** When the unpowered a4
driver regained power, two pieces of gear answered a4 at once and the scan
reported `a4: contested` with the correct remedy text. This is the first
physical two-unit collision behind `RX_ACTIVITY`, which every commissioning
safety claim rests on and which until now had only host vectors. It is the scan
path (`has_undecodable_activity`) that was exercised; the commissioning
post-scan audit's own contested path still has no bus result, because the
commission run that followed was clean.

**Not exercised, still owed a bus:**

- Equal-random-address handling. The two commissioned units drew distinct
  randoms, so that path never ran. It remains the largest untested slice.
- `commission devices` (control-device commissioning), `backup import`,
  `backup export`, `restore groups`.
- DT6, DT8, memory writes, input-device configuration writes.

Three defects and one design gap came out of this session; each has an
investigation entry below.

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

### Verified locally on 2026-09-02 (ESPHome 2026.8.1 schema, suite re-run)

Working tree at `dev` `e26f442`, clean.

- All 26 host suites build and pass (`mingw32-make --directory build test`).
  No change from the 2026-08-26 pass; this is a re-run, not new coverage.
- `dali_test.yaml` passes `esphome config` on **ESPHome 2026.8.1**, one minor
  release above the 2026.7.4 recorded on 2026-08-26. Because that config is
  `type: local`, this validates the Python schema in
  `esphome/components/dali/__init__.py` against the working-tree component.
- Scope, stated precisely: `esphome config` exercises the Python schema only. It
  does not compile the ESPHome C++ layer or the vendored C stack, so 2026.8.1
  has no compile result here and the `manifest.json` floor of `>=2026.6.0`
  remains advisory. `esphome compile` under 2026.8.1 is the missing half.
- Not a hardware pass. No flash, no bus.

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
  builds pass. These are host and compile results only: no flash, captured
  collision waveform, or commissioning run was used in this pass.
- Multi-device commissioning therefore remains restricted to the documented
  single-unaddressed-device envelope until HIL proves overlapping replies. Part
  103 quiescence, equal-random-address recovery, external-master arbitration, and
  the 100 ms post-RANDOMISE value still need standards/hardware validation.

### Verified locally on 2026-08-26 (cross-part TERMINATE, device-space contested)

Gaps 1, 2, and 5 of the mixed-device list. Gaps 3, 4, and 6 are untouched.

- **A Part 103 special-command path exists.** `DALI_CMD_FRAME_24BIT_SPECIAL` is a
  new frame kind — a 24-bit frame whose first byte is the fixed `0xC1`
  special-command address rather than a device address, which is what
  distinguishes it from the existing `DALI_CMD_FRAME_24BIT_DEV`.
  `DALI_CMD_DEVICE_TERMINATE` (opcode `0x00`) is the first and so far only
  command in it, built by `dali_build_device_special()`. The two TERMINATEs are
  deliberately not reachable through each other's builder; a vector pins that.
- **Gear commissioning brackets itself with it.** Three sends: before
  `INITIALISE`, immediately after the opening sequence, and in the cleanup
  unwind. Only the second one addresses the actual fault — a control device
  enters its own addressing state *because of* the Part 102 `INITIALISE`, so a
  send before it closes nothing relevant. The other two are hygiene and
  symmetry with the quiescence bracket.
- **The one judgment call, recorded rather than hidden.** That second send is a
  `0xC1`-prefixed frame transmitted while control gear sits in an initialise
  window, which is the cross-part interference direction `dali_protocol.md`
  warns about. Gear that mis-frames the 24-bit special as 16-bit reads `ENABLE
  DEVICE TYPE 0`, which qualifies only the next frame; the frames that follow
  are specials and are not device-type-qualified, so the stray enable expires
  without effect. Bounded argued risk against a phantom device that gets a short
  address programmed into nothing. If that trade is judged wrong, deleting the
  second send is a one-line change.
- **Opt-in and non-fatal**, matching quiescence exactly.
  `DaliCommissioningOptions.terminate_control_devices` is off in a
  zero-initialized struct, so an out-of-tree caller keeps the frames it already
  sent; the shell sets it. A failure records `cross_part_error` and the run
  continues — hardening is not a precondition. The shell prints a line only on
  failure, because nothing acknowledges a Part 103 TERMINATE and a success line
  would claim more than the bus said.
- **Quiescence does not cover this and the docs no longer imply it might.**
  Quiescent mode stops a control device transmitting on its own initiative. It
  does not stop the device entering addressing state on an `INITIALISE` it
  observed, nor answering a `COMPARE` it was addressed with.
- **The device address space stops being invisible.** The Part 103 instance
  probe in `dali_discovery_scan()` treated everything that was not `DALI_OK` as
  "no device", `DALI_ERR_RX_ACTIVITY` included — so two control devices sharing
  a device short address were dropped entirely rather than merely unreadable.
  Now recorded as `has_undecodable_device_activity` and counted in
  `undecodable_device_count`, reported by both `discover` surfaces as `dN:
  contested`.
- **Kept out of the gear mask on purpose.** The two address spaces are
  independent, so the device-space flag is separate from
  `has_undecodable_activity` and reserves nothing:
  `dali_commissioning_used_mask_from_inventory()` is unchanged and still
  gear-only. Reserving a gear address because a control device collided at the
  same number would be a different bug from the one this fixes. A vector asserts
  both flags and both counters independently.

Four new commissioning vectors and one new discovery vector; `test_commissioning`
goes 43 → 47, `test_discovery` 56 → 57, all 26 suites pass. Mutation-checked:
removing the post-`INITIALISE` TERMINATE fails two of the four. `dali_shell.c`
and all four ESPHome translation units compile clean against their real flag
sets. No bus has run any of it.

### Verified locally on 2026-08-26 (equal-random-address detection)

Step 2 and step 3 of the equal-random-address plan, which collapsed into one
change once the recovery turned out not to need a nested INITIALISE window.

- `VERIFY SHORT ADDRESS` now has three outcomes.
  `dali_commissioning_verify_from_sequence()` and
  `dali_commissioning_verify_short_address()` return
  `DaliCommissioningVerifyOutcome` — CONFIRMED, SILENT, MULTIPLE — instead of a
  bool. Qualified reply-window activity on the VERIFY step alone becomes
  MULTIPLE with `DALI_OK`; on the PROGRAM step it stays an error, because a
  collided write says nothing about how many devices exist. This is the one
  place COMPARE's activity rule is extended rather than copied: COMPARE folds
  activity into YES because for COMPARE the third state *is* YES.
- Why the inference holds: VERIFY is answered only by selected devices, and the
  PROGRAM in the same atomic sequence does not change the selected set. One
  responder decodes, two overlap and do not. It is the only point in the walk
  where co-selection is provable — COMPARE is a wired-OR and cannot distinguish
  one device from two.
- It is safe in both directions, which is what justifies acting on a heuristic.
  A false positive (one device with a marginal backward waveform) costs an extra
  search round and a different short address than expected, no damage. A false
  negative (twins whose replies happen to decode) leaves a duplicate for the
  post-scan to catch. The two layers compose; neither has to be perfect. It does
  inherit the quiescence caveat: a control device that never received the
  broadcast can still put frame-like activity into that window.
- Recovery, without a nested INITIALISE. On MULTIPLE the walk sends PROGRAM
  SHORT ADDRESS `0xFF` — taking the address back from both, since selection is
  by random address — then WITHDRAW, then continues. The short address is
  deliberately left unconsumed, so the next device found takes the address the
  pair gave back. The twins end unaddressed rather than sharing an address,
  which is where they started and what a later run handles: re-running
  re-randomises them, and colliding twice is 2^-24.
- One collision costs one address's worth of progress, not the run. Same
  reasoning the short-address scan already applies to one contested address
  among sixty-four.
- The WITHDRAW is not optional and its failure is not survivable. Without it the
  next `find_next_random_address()` converges on the same random address and the
  walk never terminates, so a transmit failure aborts the run. A withdraw that
  transmits but does not take is caught separately: the walk remembers the last
  duplicated random address and stops if it comes back, because nothing about
  retrying will change it.
- `DaliCommissioningResult` gains `duplicate_count`,
  `duplicate_random_addresses[4]`, and `duplicate_recovery_failed`;
  `DaliCommissioningEventKind` gains
  `DALI_COMMISSIONING_EVENT_DUPLICATE_RANDOM_ADDRESS`. The shell prints each
  collision as it happens and summarises at the end, on the failure path too —
  a de-addressed pair is unaddressed gear waiting for another run, not gear that
  vanished.
- Transmission, not confirmation. Two devices answering QUERY SHORT ADDRESS with
  the same `0xFF` collide exactly as they did before, so there is no clean
  readback for the de-address. Reported the way TERMINATE and quiescence are.

Three new host vectors and three updated ones; `test_commissioning` goes from 40
to 43 and all 26 suites pass. The mock gained a de-address path, a VERIFY that
answers for however many devices hold the address, and a WITHDRAW that can be
made a no-op to exercise the loop guard. Checked against a mutation: removing the
de-address fails two of the three new tests rather than passing quietly.
`dali_shell.c` compiles clean against the ESP-IDF flag set. No bus has run any
of it, and none is expected to — at ~1.2e-5 per run this is vector territory.

### Verified locally on 2026-08-26 (commissioning post-scan verification)

Prompted by a documentation-currency pass that turned into an audit of what the
commissioning walk can and cannot detect. The audit's findings are the P0 item
"Equal random address and mixed-device commissioning" below; this entry is the
one piece of it that needed no protocol work and shipped immediately.

`commission`'s post-scan printed a device count and nothing else. That is the
exact shape that hides an equal-random-address collision: two gear that RANDOMISE
to the same 24-bit value are selected together, programmed together, and
withdrawn together, so the walk reports one assignment, one short address ends up
with two gear on it, and every step returns `DALI_OK`. The post-scan already saw
it — QUERY STATUS to that address draws two overlapping replies and lands as
`has_undecodable_activity` — the result was simply never read.

- `cmd_commission` now snapshots the pre-scan as two 64-bit masks (gear present,
  answered undecodably) before the walk touches the bus, and diffs the post-scan
  against them. Masks rather than a second inventory because the shell keeps one
  4868-byte scratch buffer that the post-scan overwrites, and a second copy on
  the shell task's stack is the crash this project already fixed once.
- Every assignment is now classified: confirmed, contested, or silent. The run
  prints `post-scan confirmed N of M assignment(s)`, names each address that
  failed, and explains the contested case as the equal-random-address collision
  it almost certainly is — both gear hold the address, neither can be reached
  alone, and separating them is a physical job.
- Diffing against the pre-scan is what makes the report honest. A contested
  address that was already there is held out of the free pool and never
  assigned, so it must not be reported as damage this run did. Addresses that
  became contested without being assigned are named separately, because a run
  cannot program an address it never allocated: that is a bus that changed
  underneath the walk.
- Detection only. Recovery still needs the walk to notice co-selection at VERIFY
  and re-open a per-address INITIALISE window on the pair; see the P0 item.
- The failed-run path still returns before any post-scan. That is where a
  collision is most likely — an `RX_ACTIVITY` abort on VERIFY is the twin case —
  but the bus state after a failed TERMINATE is not one to scan without thinking
  about it first. Left as-is deliberately, recorded in the P0 item.

`dali_shell.c` compiles clean against the ESP-IDF flag set (`-Wall -Werror
-Wextra`). It is not in the host suite, so there is no vector for this and no bus
has run it: an equal-random-address collision is roughly a 1-in-10^5 event on a
20-device bus, which is precisely why it needs a mocked vector rather than a
hardware session.

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
  override stays under test; the README example and site configs no
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
  image). A local compile-test config builds the working-tree component with
  ESPHome 2026.7.4 (919611-byte OTA image); the shared-stack header changes did
  not disturb the ESPHome wrapper set.
- These changes are host- and compile-verified only. No new verb has been
  flashed or exercised on hardware, and neither site deployment has been re-tested.

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
  image). A local compile-test config builds the working-tree component with
  ESPHome 2026.7.4 (919515-byte image).
- These changes are host- and compile-verified only. They have not been flashed
  or exercised on hardware, and neither site deployment has been re-tested.

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
  image). A local compile-test config builds the working-tree component with
  ESPHome 2026.7.4 (ESP-IDF 5.5.5; 918608-byte OTA image).
- These audit fixes are host- and compile-verified only. They have not been
  flashed or exercised on hardware, and neither site deployment has been re-tested.

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
  The local compile-test config had been switched to the `v1.0.1` git source and
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
  external component through a local compile-test config; dispatch schema boundary
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

---

# Investigations

## The Steinel's events lost their device address — found 2026-10-01

Found in the ninth session. What the bus showed was observed; the integration's
side is read from source; what changed the scheme is not known.

Supersedes one sentence of *What a device RESET clears*, below: "The Steinel
emits in scheme 2" held on 2026-09-04 and did not hold on 2026-10-01, until the
session put it back.

### Why scheme 0 is slow rather than broken

Every input sensor is polled on its own `poll_interval`, 30 s by default,
whatever the bus does. An event only brings that poll forward.
`on_dali_unsolicited()` in `dali_component.cpp` sets `poll_requested` on a
registered sensor when three things hold:

- the event is a Device/Instance frame;
- its device address and instance number equal the sensor's;
- the sensor has `poll_on_event`.

The loop polls a requested sensor at once. The event information is never the
value.

A sensor's YAML carries an address and an instance number and nothing else that
could identify an event source. Each other scheme lacks one of the two:

| Scheme | Carries |
|---|---|
| 0, instance | instance type and number, no address |
| 1, device | address and instance type |
| 3, device group | device group and instance type |
| 4, instance group | instance group and instance type |

None can be matched to a sensor without metadata the YAML does not hold, and the
code declines to guess. Such an event is decoded, logged at DEBUG and queued for
headless dispatch, and the sensor waits for its interval. A state change then
reaches Home Assistant anywhere from 0 to `poll_interval` later, rather than one
query after the event. Headless dispatch splits the same way: `key_matches()` in
`dali_dispatch.c` matches a rule keyed on a short address only against events
that carry one, which are schemes 1 and 2.

Nothing reports the fallback.

### When it changed

The 2026-09-04 capture recorded `0x008001` and `0x00840C`. Both have bit 23
clear and bit 15 set, which is the Device/Instance layout. The ninth session
found 0 on both emitting instances, so both changed at some point between those
two dates.

A RESET is unlikely, on the evidence available:

- `steinel_bank2_reference.md` gives Steinel's defaults as hold timer 1, report
  timer 5 and event filter 7.
- The report timer reads 30, as the 30.01 s heartbeat of 2026-09-04 says it
  did then.
- The filter reads 7, which matches the default, so it tells neither way.

A RESET would also have returned Bank 2 to factory values, so `devmem read 0 2 4
10`, read against any earlier tuning, would settle it. `occ-hold-timer` against
the default of 1 is one more data point.

The one recorded change to the Steinel in the window is 2026-09-29:

1. its address was cleared to MASK by hand (`raw C130FF`, `raw2 01FE14`);
2. it sat unaddressed until `commission devices` put it on d0;
3. it was moved through d1 and d2 and back.

A Part 103 rule may switch schemes 1 and 2 to scheme 0 when the short address is
deleted, and not switch them back. That rule is recalled rather than sourced;
TI's `DALI_103_setShortAddress()` has no such step. If it exists it fits
everything seen: both instances at once, exactly 0, nothing else touched.

To separate the moves from the clear, on 2k with the scheme at 2:

1. `dtrcheck 0 0 5`, since the `address` arms now read DTR0 back.
2. `address d0 set d2`, `address d2 set d0`, then `iquery 0 1 event-scheme`.
3. `address d0 clear`, `commission devices`, then `iquery 0 1 event-scheme`.

### Event priority settles the emitter-spacing candidate

Both emitting instances read event priority 4. Instances 2 and 3 report
`status=0x00` and emit nothing; neither the 2026-09-04 capture nor the ninth
session's log has an event from them. *Whether that explains the backoff
depends on a0's event priority*, under Investigations, set the rule: at 4 or 5
the emitter-spacing mechanism does not explain 8/8. So it does not. The
decisive test named there, the pre-backoff build under `quiescent on all`, is
what remains.

### Instance settings in the backup: the decision it waits on

What the 2026-10-01 work did not do is put each instance's settings — event
scheme, priority, filter, instance groups — into the device backup so a restore
can put them back. The model change is small; where the bytes live is not:

- The snapshot holds up to 128 entries and a device can have 32 instances, so
  a per-entry instance table is out of the question. A bounded table of, say,
  64 instance records at about 12 bytes adds 768 bytes to the blob and to each
  RAM copy of it: the shell's `s_backup` and `s_backup_blob`, and the
  integration's `s_address_backup`.
- ESPHome loads a preference by its stored size. Growing
  `AddressBackupPersist` makes a backup saved by an older build unreadable, so
  an upgrade silently drops it unless the new build migrates it.

Three ways to do it:

1. **Blob v2.** One backup, a version bump, a decoder that still reads v1, and
   a persisted record that grows — with migration code, or with the loss
   accepted and stated in the release notes.
2. **A second record beside the first.** The address backup stays v1, so
   nothing stored is lost. Instance settings go in their own blob, under their
   own preference key and in their own export line. It costs a second buffer
   and a second import step.
3. **Declare it in YAML instead.** A sensor states the scheme it needs, and the
   integration writes it back when its profile read finds otherwise. That
   repairs the case that actually happened without any backup at all, but it
   makes the integration write to devices on its own initiative, which nothing
   in it does today.

### Decided: version 2, and old backups are dropped

The operator chose the first option on 2026-10-01: bump the blob format and
accept that older backups are lost. The decoder reads version 2 only, so an
exported version-1 file is refused (`backup import` names it as one), and a
backup an older ESPHome build stored no longer loads because the stored record
changed size. `ADDRESS_BACKUP_MAGIC` was kept: it is the NVS key, so the first
version-2 save overwrites the old entry rather than stranding it in the
partition. Implementation in the verification entry of the same date.

### The Part 303 event information

`evt=12` is `0x00C`. It arrives every 30 s, and the report timer is 30. That
fits bit 2 being the repeat flag of a report-timer event, which *Opcodes: a
second independent source* could not place from TI's code. If bits 0 and 1 are
movement and occupied and bit 3 is the movement-sensor type, the heartbeat reads
"vacant, no movement, repeat". That bit assignment is recalled, not sourced; the
matching period is the only bus evidence.

## Two units that answer alike share an address invisibly — found 2026-09-30

Found in the eighth session. What the bus did was observed; why is read from
source.

### Why nothing flagged it

The scan classifies an address by its QUERY STATUS reply alone
(`discovery_scan_walk()`): decoded is present, silence is absent, and
undecodable activity is contested. Two units on one address reply to the same
frame at nearly the same moment, and the bus carries the AND of the two. Where
their bits differ, a bit comes out low in both halves, and `decode_half_bits()`
rejects any bit that is not a clean low-high or high-low pair. So differing
replies make the address contested. Where the bytes agree and the timing is
close, the superposition is one valid frame. Two lamps of one product, off and
in one group, agree on status, level, groups, version and device type, which is
everything the walk asks before Bank 0.

Every walk goes through `discovery_enrich_device()`: `scan`, `discover`, both
commissioning scans, `backup save` and the `restore` refresh. It reads the
Bank 0 identity at every present gear address, and that read is the one question
two units cannot answer alike, because their identification numbers differ. At
a3 it failed, which is what `restore plan`'s `identity unknown` reports. But
enrichment keeps the identity only on `DALI_OK` and drops the error, so a
collision there becomes "no identity" and nothing more.
`dali_memory_read_from_sequence()` does return the sequence's error, so the
distinction is there one call up.

Safety held because an address shared this way still reads as occupied. The
commissioning pre-scan will not assign onto it, `address set` refuses it as a
destination, and the planner treats an unidentified unit as immovable. What was
lost is the location. To the operator a unit vanished, and nothing says it is
sharing an address rather than unpowered.

The YES/NO queries behave the same way for a different reason. Every unit
answers YES with `0xFF`, so several in step decode as one YES. The `more than
one unit` wording that `address clear` and `backup save` print on RX activity
from QUERY MISSING SHORT ADDRESS therefore appears only when the units' timing
garbles their replies.

### Fix, proposed and not implemented

Keep the identity read's error in `discovery_enrich_device()`. On a read that
fails with undecodable activity, read once more, so that one frame hit by a
DALI-1 coupler is not taken for a second unit. If it fails the same way again,
record the address as shared: it answers, and more than one unit is behind it.

- `scan` and `discover` name the address, with the `address <aN> clear` /
  `commission unaddressed` advice the contested note already gives.
- The planner reserves it, as it reserves a contested address. `backup save`
  warns instead of recording it as one unanchored unit.
- A new flag rather than `has_undecodable_activity`, because the address does
  answer, and the inventory's contested addresses are deliberately not
  `present`. The cost is that every reader of the contested flag must read both.

It adds no frames to a bus without a shared address. Gear with an empty Bank 0
stays invisible. For that gear, QUERY RANDOM ADDRESS is the fallback probe, and
it works only while the units' random addresses differ.

## A lost DTR0 load re-addresses a unit to whatever DTR0 last held — found 2026-09-30

Found by reading the move that failed in the seventh session. The miss was
observed once; the rest is read from source.

### What the bus showed

The second move of the first `restore apply`, a5 -> a2, sent DTR0 = `0x05` and
then SET SHORT ADDRESS DTR0 twice to a5, and the scheduler reported all three
frames sent. The confirmation found nothing at a2, and the next `restore plan`
found the unit still at a5. By the device log's timestamps the three frames
took about 70 ms, which is what three frames 22 Te apart take, so no stall
split the pair. The unit missed one of the three frames, and nothing in the
log says which.

### Why this miss was harmless, and the next one need not be

The move before it ended with its confirmation, which reads the identification
number from Bank 0 at the destination. That read loads DTR1 = 0 and DTR0 =
`0x0B` by broadcast, then advances DTR0 only in the unit it reads. So when move
2 began, every other unit on the bus held DTR0 = `0x0B`, the one at a5
included. `0x0B` is `(5 << 1) | 1`, the gear encoding of a5.

- If the unit missed the DTR0 frame, SET SHORT ADDRESS ran with `0x0B` and
  wrote a5 again.
- If it missed a frame of the pair, SET SHORT ADDRESS did not run.

Either way it stays at a5, which is what the re-plan found. The first case was
harmless only because the move started *from* a5. Every move after the first
in a plan begins with the same stale `0x0B`, so the same miss from any other
source puts the unit on a5, whoever holds it. On 2k's fourth session the
staging hop went to a5, so a miss on the next move would have put the second
lamp on top of the staged one. The confirmation would then have reported
`nothing answers at the target` and stopped. It reads the target first, so it
would not have said that the unit had left its source, or that a5 was now
contested.

`address set` and `address clear` send the same unverified pair, and a miss
there writes whatever the unit's DTR0 last held, typically an offset from the
last memory read. The device space has the same shape: its Bank 0 read leaves
the device DTR0 at `0x0B`, which Part 103 reads raw, as d11.

### Which frame is the likelier miss

The DTR0 frame was the only one of the three to follow another unit's backward
frame, the last identification byte from a16. The pair followed our own DTR0
frame by the 22 Te guard. The scheduler spaces nothing from a received frame,
so the DTR0 frame went out about 2.9 ms plus task latency after that reply,
where AN1220 waits 9.17 ms (*Nothing spaces a forward frame from a received
one*, below).

The evidence either way is thin:

- Gear moves that load DTR0 straight after another unit's reply, with a value
  other than the stale one: three on 2k, in the fourth and fifth sessions,
  landed; this one on 1k did not.
- The second apply's second move, a16 -> a5, also followed another unit's
  reply. But it wanted `0x0B`, the stale value, so it would have landed
  whether or not the load did. It tests nothing.
- Every `address set` loads DTR0 straight after the moving unit's own reply to
  the source probe. Three did so on 1k in this session, and all landed. A
  receiver that treats another unit's backward frame as a framing error, and
  waits longer before listening again, would miss only after another unit's
  reply. That is a guess about the drivers, not a reading.

### Fix, proposed and not implemented

Read DTR0 back from the source before sending the pair. Send DTR0 and QUERY
CONTENT DTR0 (`0x98`, device `0x36`) at `from` as one sequence, and the SET
SHORT ADDRESS pair as a second sequence only if the reply is the value loaded.
The unit then holds the right DTR0 whenever the pair runs, so a lost frame of
the pair can only do nothing, and a lost load is named when it happens rather
than surfacing as a silent target. A contested `clear` needs an exception,
because two units answering the read-back may collide. Separately, the
confirmation could probe the source when the target is silent, to tell "did not
move" from "moved somewhere else".

## `restore apply` publishes the scan it planned from — found 2026-09-29

Found by reading `cmd_restore()` for the fifth 2k session. No bus has shown it.

An apply ends by publishing `s_inventory` through the `inventory_changed` hook,
under a comment saying that short addresses moved and every cached view is
stale. But `s_inventory` holds what `shell_restore_refresh()` stored before
the plan was built: the planning scan, taken before the first move. The
component's `apply_inventory_snapshot()` rebuilds its group map from that scan
and marks the map for persisting. So after any apply that moves gear, each
moved unit's groups are filed under the address it left. A group light can
then take a poll representative outside its group, and the stale map survives
a reboot until the next scan. That path does not touch level profiles, so the
MIN/MAX cached at both ends of each move stays until a refresh re-reads it.

The `address` verb avoids this with a narrower hook. After confirming both
ends it calls `short_address_moved(from, to)`. The component then moves the
group entry, forgets the level profile at both addresses, logs that YAML
entities still name the old address, and requests a refresh. A restore move is
confirmed more strictly than an `address` move, because it also checks the
identification number at the destination, so it meets that hook's contract.

Fix: call `short_address_moved()` after each confirmed gear move, in plan
order, and drop the publish at the end of the apply. Calling it move by move
mirrors the bus, because the plan only moves a unit onto an address the bus
has already shown vacated. One side effect is that the hook's YAML warning also
fires for staging hops, which pass through addresses no entity names. Device
moves need nothing, because the integration caches no device address (see
`dali_shell.h`, beside the hooks).

## A device-side source sides with Beckhoff on both Part 103 encodings — found 2026-09-29

Supersedes the source table and the proposed bus check in *Stack review
follow-up* → *Part 103 addressing encodings: what the sources say*; that entry
stays as written. The bus-timing findings from the same reading are the next
entry. Nothing in the code changed, and nothing touched a bus.

Three sources were read against the open items in `current_status.md`:

- **TI MSPM0 SDK 2.11.00.07**, DALI middleware, in `_local/` and not tracked.
  `source/ti/dali/dali_103/` is a Part 103:2014 control device with Part
  303:2017 occupancy on top; `dali_102/` is Part 102:2014 gear with Part 207.
  It is the first *device-side* implementation this project has read —
  Beckhoff, `esp_dali`, Tasmota and python-dali are controllers or their
  documentation — so it is the first source that shows how a device decodes
  what a controller sends.
- **Espressif `esp_dali` v1.1.0** (2026-06-10), the current `components/dali`
  of `esp-iot-solution`, read from GitHub. It is where this project's Part 103
  values came from.
- **Silicon Labs AN1220** rev 0.3, a DALI-1 physical-layer note for the EFR32.
  Nothing on Part 103 or DALI-2; it is used only in the next entry.

### How far TI can be read

It is a demonstration stack, and its defects decide what it is evidence for:

- INITIALISE's address filter in `DALI_103_initialise()` returns early only
  when the parameter is `0x7F` and the device has an address. Any other
  parameter — `0x00`, or an address the device does not hold — opens the
  window.
- Instance-number addressing never matches: `DALI_ControlDevice_InstCheck()`
  returns `instMask << InstByte` with `instMask` still 0.
- Device-group addressing ANDs the membership mask with the group *number*.
- Event messages check neither quiescent mode nor the instance's event
  priority; they go out with whatever settle time the last transmission set.
- The Part 102 side answers `COMPARE` and QUERY MISSING SHORT ADDRESS with
  `0x00` for NO instead of staying silent, and its `INITIALISE` ignores its
  parameter.

So TI is evidence for opcodes, encodings and intent — what its authors meant a
device to do — not for edge behaviour, and never for conformance.

### The two encodings

**INITIALISE (device).** The cases `DALI_103_initialise()` distinguishes are
Beckhoff's: `0xFF` for every device, `0x7F` for devices whose short address is
MASK, and otherwise the parameter compared with the device's own raw short
address. The logic combining them is wrong, as above, but the cases are not in
doubt. Under them `0x00` selects d0.

**SET SHORT ADDRESS (device, `0x14`).** `DALI_103_setShortAddr()` sets MASK
when DTR0 is `0xFF`, stores DTR0 as it stands when it is below `0x40`, and
otherwise keeps the old address. The same SDK decodes the Part 102 command the
other way: `DALI_setShortAddress()` in `dali_102/dali_target_command.c` accepts
DTR0 only when `(DTR0 & 0x81) == 0x01` and stores `DTR0 >> 1`. One team, two
parts, two encodings on purpose — the argument Beckhoff's two INITIALISE pages
made, now from device firmware. Under TI's decoding this project's
`(M << 1) | 1` puts a device on d(2M+1) for M < 32 and is ignored for M ≥ 32,
as *Stack review follow-up* predicted.

PROGRAM SHORT ADDRESS, VERIFY SHORT ADDRESS and the QUERY SHORT ADDRESS reply
are raw in TI, which is what this project already sends and decodes. VERIFY is
answered only by a device in its initialisation state, which is what makes it
the probe for the bus check below.

**`esp_dali` v1.1.0** still sends `0x00` for unaddressed devices;
`dali_103_commission()` comments "0xFF selects all input devices; 0x00 selects
devices without short address". It defines device `0x14`, "[2x] Set input
device short address from DTR0", and never calls it. No source this project
has read ever gave `(a << 1) | 1` for the device command: that came from the
Part 102 analogy alone.

These two tables supersede the one in *Stack review follow-up*.

| INITIALISE (device) | What it is | No short address | One device | All |
|---|---|---|---|---|
| Beckhoff `FB_DALI103Initialise` | controller library | `0x7F` | `0x00`–`0x3F` | `0xFF` |
| TI `DALI_103_initialise()` | device firmware | `0x7F` | raw address | `0xFF` |
| Espressif `esp_dali` v1.1.0 | controller | `0x00` | — | `0xFF` |
| Tasmota `xdrv_75_dali.ino` | controller | `0xFF` | — | `0x7F` |

Tasmota still decides nothing, for the reason the earlier entry gives.

| Device SET SHORT ADDRESS | DTR0 encoding |
|---|---|
| TI `DALI_103_setShortAddr()` | raw `0`–`63`; `0xFF` clears; anything else ignored |
| Tasmota | raw: `0x14 // REPEAT - DTR0 0..63` |
| Beckhoff | raw, by inference from `shortAddress` `0…63, 255` |
| `esp_dali` v1.1.0 | opcode only; no encoding stated or used |
| this project | `(a << 1) \| 1`, from no source |

### TI's own bug changes the bus check

The check proposed on 2026-09-25 — INITIALISE `0x00` twice, VERIFY SHORT
ADDRESS 0, TERMINATE, with a YES meaning `0x00` selected the Steinel — has a
second explanation for a YES: a device that ignores INITIALISE's parameter
answers the same way, and TI's firmware is one. Its filter rejects only `0x7F`
on an addressed device, so the `0x7F` arm the earlier entry added would not
expose it either. What separates the two readings is an address nothing holds.
The revised check adds that arm and a positive control, confirms every
TERMINATE with a second VERIFY, and changes no address. It runs under
`quiescent on all` so that a0's own events cannot cost it a reply; see the next
entry.

```text
discover                  # the Steinel at d0, 4 instances; d63 empty
quiescent on all
special terminate         # close any Part 102 window
raw C10000 len=24         # TERMINATE (device): close any Part 103 window
raw C10900 len=24 wait    # VERIFY SHORT ADDRESS 0: baseline, expect timeout

# four arms, parameter PP = FF, 3F, 00, 7F, in that order
raw2 C101PP len=24        # INITIALISE (device) PP, sent twice
raw C10900 len=24 wait    # VERIFY 0: an FF reply = selected, timeout = not
raw C10000 len=24         # TERMINATE (device)
raw C10900 len=24 wait    # expect timeout: the window is closed

special terminate
quiescent off all
```

| `FF` | `3F` | `00` | `7F` | Reading |
|---|---|---|---|---|
| YES | — | YES | — | `0x00` selects d0, as Beckhoff and TI say; the constant becomes `0x7F` |
| YES | — | — | — | `0x00` does not select an addressed device; "unaddressed" is still unproven, since that needs an unaddressed device |
| YES | YES | any | any | The Steinel ignores the parameter: the other arms prove nothing, and neither value keeps an addressed device out |
| YES | — | any | YES | `0x7F` selected an addressed device, against both sources: stop and look |
| — | any | any | any | The probe is not getting through: repeat, and stop if `FF` stays silent |

`—` is a timeout. Undecodable activity on a VERIFY is inconclusive, since at
most one device should be answering. Run the four arms twice: one lost reply
must not decide a row. The INITIALISE frames are gated as commissioning
frames, so the TCP shell needs `allow_commissioning: true`; the native serial
CLI sends them as they are.

**SET SHORT ADDRESS can be settled on the same visit, before any code
changes**, at the cost of moving the Steinel off d0 for a minute.
`address d0 set d2` on the current firmware loads DTR0 = 5. A device that reads
DTR0 raw goes to d5, one that reads `(a << 1) | 1` goes to d2, and the verb's
own read-back shows which did not happen.

```text
discover                  # d2 and d5 must be empty in device space
backup save               # so 'restore plan' can find the unit by identity
quiescent on all
address d0 set d2         # loads device DTR0 = 5, reads both ends back
raw 05FE35 len=24 wait    # QUERY NUMBER OF INSTANCES at d2
raw 0BFE35 len=24 wait    # ... and at d5

# back from d5, if that is where it went; the verb would load 1, i.e. d1
dtrcheck 5 0 0            # device DTR0 = 0, read back at d5
raw2 0BFE14 len=24        # SET SHORT ADDRESS at d5, sent twice
raw 01FE35 len=24 wait    # 4 instances at d0 again

quiescent off all
```

| After `address d0 set d2` the Steinel answers at | Reading |
|---|---|
| d2 — the verb confirms d2 and d0 silent | the device reads `(a << 1) \| 1`; this project's encoding is right for it |
| d5 | raw, as TI, Tasmota and Beckhoff have it |
| d0 — d2 not confirmed, d0 still answers | the write was not taken; neither encoding shown |
| none of these | find it with `discover`, or with `restore plan` by identification number |

Moving it back depends on where it went. From d2, `address d2 set d0` loads
DTR0 = 1 and is correct under that reading. From d5 the verb would load 1 too,
which the raw reading sends to d1, so use the raw frames above. Do not use
`restore apply` to put it back: its device moves load the same `(a << 1) | 1`.

### Opcodes: a second independent source

`dali_protocol.md` said the Part 103 special-command opcodes were transcribed
from `esp_dali` and unverified. TI's device dispatch, `dali_cd_comm.c`, routes
every opcode this project sends to the same command:

- specials `0x00`–`0x0A`, `0x20`, `0x21` and `0x30`–`0x32`, under first byte
  `0xC1`;
- device commands `0x14`, `0x15`, `0x1D`, `0x1E`, `0x35`–`0x38` and `0x3C`;
- instance configuration `0x61`–`0x68`;
- the Part 303 opcodes it implements, `0x20`–`0x24` and `0x2C`–`0x2F`, with
  timer units of 10 s for hold, 1 s for report and 50 ms for deadtime;
- the bit-replicated occupancy value, `0x00`/`0x55`/`0xAA`/`0xFF`;
- all five event-source layouts, bit for bit, with scheme numbers 0–4 matching
  SET EVENT SCHEME;
- the 16 DT6 opcodes its gear side dispatches.

TI has no instance queries (`0x80` and up), no `0x69`/`0x6A`, and nothing from
Part 301 or 304, so those rows keep only their existing sources. It also
dispatches three specials with their own first byte — `0xC5` DIRECT WRITE
MEMORY, `0xC7` DTR1:DTR0 and `0xC9` DTR2:DTR1 — which this stack does not
send.

One `esp_dali` error turned up on the way: its `DALI_103_QUERY_SHORT_ADDRESS`
is `0x3F`, which is QUERY MANUFACTURER SPECIFIC MODE in TI's dispatch, and its
own comment says QUERY SHORT ADDRESS is not a device-level command. Nothing
here uses it.

The Steinel's 30 s occupancy frame from the 2026-09-04 capture, `0x00840C`,
decodes under TI's layout as device 0, instance 1, event information `0x00C`:
movement and occupied clear, bits 2 and 3 set. TI sets bit 3 for a movement
sensor and never sets bit 2, so it does not account for that frame; a typed
Part 303 profile still needs the standard's event table.

### Device-level opcodes this stack lacks

From TI's dispatch, with `esp_dali` agreeing where noted. They are in
`dali_protocol.md`, *Device-level commands*, marked not implemented.

- QUERY DEVICE GROUPS 0–7, 8–15, 16–23 and 24–31: `0x41`–`0x44`, one byte
  each (TI, `esp_dali`).
- ADD TO DEVICE GROUPS 0–15 and 16–31: `0x19` and `0x1A`; REMOVE FROM DEVICE
  GROUPS: `0x1B` and `0x1C`. Send-twice, with the 16-bit mask in DTR2:DTR1,
  DTR1 the low byte (TI only).
- QUERY MISSING SHORT ADDRESS: `0x33` (TI, `esp_dali`). The device counterpart
  of the broadcast query that backs `address <aN> clear`; `address <dN> clear`
  has only silence today.
- QUERY QUIESCENT MODE: `0x40` (TI). QUERY DEVICE STATUS: `0x30` (TI,
  `esp_dali`); its bits are 0 input-device error, 1 quiescent mode, 2 missing
  short address, 3 application active, 4 application-controller error, 5 power
  cycle seen, 6 reset state.

The device-group address byte is `10GGGGG1`, 32 groups.

### What a device RESET clears

TI's `DALI_103_reset()` sets device groups to 0, every instance's groups to
MASK, its event filter to the reset value and its event scheme to 0 — instance
addressing — and resets the random and search addresses. It keeps the short
address, and ignores commands for 300 ms. The Steinel emits in scheme 2,
device/instance (`0x008001`, `0x00840C`); after a RESET its events would carry
instance type and number and no device address, and would stop matching the
Device/Instance events that trigger this integration's sensor polls. Restoring
a reset device therefore needs each instance's configuration — scheme, filter,
priority, instance groups — as well as its device groups and address. These
are TI's reset values; the standard's table has not been checked.

### Quiescent mode

TI ends quiescent mode on its own 15 minutes after the last START QUIESCENT
MODE, and the initialisation state 15 minutes after the last INITIALISE
(`QUIESCENT_MODE_COUNTER` and `INITIALISE_STATE_COUNTER`, both 900 s);
`esp_dali` gives the same 15 minutes for initialisation. On a device that does
this, a release that never lands costs at most 15 minutes of silence. Whether
the Steinel does is not known. QUERY QUIESCENT MODE would let the shell read
each device's state back after a release instead of inferring it.

TI answers `COMPARE` on its initialisation state alone; quiescent mode does not
enter into it. That is one implementation agreeing with what 2k showed for
addressed queries, on the question `commissioning_readme.md` leaves open. Its
events ignore quiescent mode altogether, which is a bug, so it says nothing
about what quiescence silences.

### Documentation corrected alongside

- `dali_protocol.md`. The reply-window paragraph near the top still gave the
  27,000 µs close and 2,000 µs decoded opening that the stack-review fixes made
  28,664 and 3,664. The device-level table listed SET SHORT ADDRESS DTR0 and
  ENABLE WRITE MEMORY as not implemented, and the specials section said only
  TERMINATE was; all are implemented. Cross-Part Interference said the Part 102
  TERMINATE around `commission devices` did not exist; it does. The INITIALISE
  encoding was stated as fact. The file also gains the two memory commands the
  code already sent without a row, WRITE MEMORY LOCATION - NO REPLY (`0x21`)
  and the device READ MEMORY LOCATION (`0x3C`); the device-level rows above; a
  note on what the intervention row can reach; four TX-spacing rows under Bus
  Timing, with the reference stacks' spacing beside them; the DALI-1 argument
  for the frame-end reference from the next entry; and TI and AN1220 as
  sources.
- `current_status.md`: the P0 encoding item and its check, the P0 timing item,
  and the P1 items on the backoff, device groups and the quiescence bracket.
- `dali_commands.md` and `commissioning_readme.md`: the `address d0 set d4`
  example and the clear-then-`commission devices` workflow assumed the
  `esp_dali` encodings, and now say they are unproven. The readme's claim that
  a stray event frame becomes a false YES in a `COMPARE` window is corrected.
- `dali_capability_matrix.md`: the quiescent note allows for the device's own
  timeout, and the `address <dN> clear` note says `commission devices` settles
  it only once the INITIALISE parameter is settled.

## Nothing spaces a forward frame from a received one — found 2026-09-29

Adds to *A candidate mechanism for the retry backoff*, in *Stack review*, and
to *The 2k "late gear" reading was event traffic*; supersedes neither. Read
from source, like the previous entry, and not measured.

### What the references do

- **TI** (`dali_103/dali_timings.h`, `dali_103/dali_gpio_comm.c`). A backward
  frame waits 5.5 ms. A forward frame waits by priority: 13.5, 14.9, 16.3, 17.9
  or 19.5 ms for priorities 1–5. Those are minimums; the "14–21 ms" this log
  has quoted is priorities 2–5 with their maxima. SET EVENT PRIORITY accepts
  2–5, and TI defaults to 4. Both waits run from the stop condition of the last
  frame on the wire, whoever sent it, and the stop condition is a 2 ms timer
  from the last bus-falling edge: 1.2–1.6 ms after a frame's last edge,
  depending on its final bits. A collision is found by comparing each RX
  half-bit with what was sent, then handled by avoidance (stop) or recovery (a
  1.3 ms break, then a retry after 4.3 ms). The receiver accepts a send-twice
  pair whose two stop conditions are 2.4–94 ms apart — looser than this
  scheduler's bracket, which runs from before the first PHY call to after the
  second returns.
- **AN1220**, DALI-1. Forward frames at least 22 Te apart, and at least 22 Te
  after a backward frame; it pads every forward frame with 24 idle half-bits to
  guarantee both. The reply starts 7–22 Te after the forward frame's stop bits,
  and the controller declares no answer if none has started by 22 Te.
- **`esp_dali`**. A 25 ms reply timeout, which its comment derives as "7–22 Te
  + BF frame 22 Te = max ~18.3 ms" plus margin, then a fixed 20 ms after every
  transaction, whether or not anything answered.

### What this stack does

The 22 Te guard is armed from our own TX return only
(`sched_arm_tx_gap(tx_return_us, DALI_FORWARD_INTERFRAME_US)` in
`dali_scheduler.c`); a received frame arms nothing.
`wait_for_bus_idle_before_tx()` wants 2 bit periods, 1.666 ms, of idle, counted
from when it starts polling. RX closes a frame after 1.25 ms without an edge.

So our next forward frame follows a gear reply by about 2.9 ms plus task
latency, and follows an event by about 1.7 ms if a frame was already waiting in
the PHY when the event ended. TI would wait at least 13.5 ms after either,
AN1220 9.17 ms after a reply, and `esp_dali` 20 ms. `dali_protocol.md` already
said the scheduler derives no gap from received frames; these are the numbers.

### The intervened path cannot see a DALI-2 event

`sched_observation_in_reply_window()` accepts an observation only if its first
edge is at least 5.5 ms and its last edge at most 28.664 ms after our frame
end. A 24-bit frame spans 20.4–20.8 ms from first edge to last, so it qualifies
only if it starts 5.5–8.3 ms after our frame, and a DALI-2 device honouring
even priority 1 starts after 13.5 ms. A 16-bit forward frame spans 13.8–14.2 ms
and qualifies if it starts before about 14.9 ms, so a DALI-1 coupler can reach
the path; a DALI-2 event cannot.

Three consequences:

- "Never fired on either bus" is structural, and says nothing about whether
  events land in our reply windows.
- The counter on the intervened path proposed on 2026-09-04 would read zero
  for event traffic. It is still worth having for 16-bit frames.
- An event that starts inside a reply window outlasts it and is attributed to
  nothing: the transaction times out while the event is on the wire, and the
  event is routed as unsolicited once it completes. For `COMPARE` that is a
  NO, which is correct when no device matched. A clean event frame therefore
  cannot become a false YES; a garbled or truncated frame that begins and ends
  inside the window can. `commissioning_readme.md` said a stray event frame
  would do it, and now says this.

### The emitter mechanism, from the device side

The 2026-09-25 candidate was that a frame queued behind an event goes out
~1.7 ms after it, under a 2.4 ms stop condition, and that the emitter is the
unit most exposed. TI's receiver shows how an emitter loses such a frame. It
decodes a capture only when its own TX status is idle — the comment reads
"Decode captured bits only if the signal is received from another device" —
and that status returns to idle only at the stop condition after its own
frame. A frame whose first edge arrives earlier is folded into the capture of
the emitter's own and discarded with it. At 1.7 ms, TI's 2 ms timer leaves a
few tenths of a millisecond of margin; a 2.4 ms timer loses it for some bit
patterns. That fits the query to a0 being the one seen lost on 2026-09-04. Not
measured.

### Whether that explains the backoff depends on a0's event priority

Take a query that went unanswered — to an absent address, or to a present lamp
that lost it — with times from our frame end. a0's pending event starts after
its stop detection (1.2–1.6 ms) plus its priority's settle, and lasts 20.8 ms.
Our timeout is processed about 28.7 ms in, the backoff then holds 11.16 ms, and
the PHY counts its 1.666 ms of idle from when it starts polling. Without the
backoff the retry always goes out 1.7 ms after the event ends. With it,
nominally:

| a0 event priority | Event on the wire | Retry after the event ends |
|---:|---|---|
| 2 | ~16.5–37.3 ms | ~4.2 ms |
| 3 | ~17.9–38.7 ms | ~2.8 ms |
| 4 | ~19.5–40.3 ms | 1.7 ms |
| 5 | ~21.1–41.9 ms | 1.7 ms |

At priority 2 or 3 the backoff outlasts the event and the retry clears a 2.4 ms
stop condition; at 4 or 5 the event outlasts the backoff, and the retry lands
where it did without it. The timeout is counted in whole ticks, so the backoff
can end a millisecond or two either side of nominal: 2 usually clears, 5 never
does, and 3 and 4 depend on where the tick falls. A 2.4 ms stop detection at
a0, rather than TI's, makes every event 0.8 ms later and narrows each margin by
as much. `iquery 0 <instance> event-priority` for instances 0–3 is read-only
and settles which case 2k is in. If the answer is 4 or 5, this mechanism does
not explain 8/8, and the decisive test in the 2026-09-03 entry — the
pre-backoff build under `quiescent on all` — is still the way to close it.

### The frame-end reference, from the DALI-1 side

AN1220 and `esp_dali` put DALI-1's 7–22 Te reply window after the forward
frame's stop bits, which from its last data bit is 11–26 Te, or 4.58–10.83 ms.
DALI-2's 5.5–10.5 ms falls inside that only when measured from the last data
bit. Measured after the stop bits it would reach 12.2 ms from the last data
bit, and a DALI-1 controller that stops listening at 22 Te — AN1220's does —
would miss conformant DALI-2 gear. Backward compatibility therefore argues for
the reading `dali_frame.h` adopted on 2026-09-25. An inference from two
application notes, not a clause of IEC 62386-101.

### `DALI_REPLY_TIMEOUT_MS`

Its comment, under "Confirmed from IEC 62386-101", reads "22 ms spec + 3 ms
margin". AN1220's no-answer point is 22 Te, 9.17 ms after the stop bits, not
22 ms, and `esp_dali` reaches its own 25 ms another way. A DALI-2 reply that
starts at the 10.5 ms limit is complete about 18 ms after the frame end, so
attribution runs about 10 ms past the last conformant reply, into the time when
DALI-2 devices may begin transmitting. Not changed: the comment is code, and
the window's length is a stack decision.

## Stack review follow-up: seven fixes, the Part 103 evidence, and a GPIO ISR-service conflict — 2026-09-25

Supersedes the *Stack review* entry below for the seven findings fixed here;
that entry stays as written. Vectors and builds are in *Verified locally on
2026-09-25 (stack-review fixes)*. None of it has been on a bus.

### What changed, finding by finding

**Occupied addresses read as free through the TCP shell.** All three proposed
parts. `try_claim_bus()` raises the gate, then polls `dali_sched_is_quiescent()`
for up to `BUS_CLAIM_DRAIN_WAIT_MS` (5 s), and lowers the gate and refuses the
claim if the queue has not drained. Every blocking wait that expires — the
shell's single-frame, sequence and reset waits, and the scan task's frame wait —
returns the new `DALI_ERR_WAIT_EXPIRED`. Neither `scan_error_is_absent()` nor
the presence probes read that as absence, because it is neither TIMEOUT nor
MALFORMED. The single-frame wait is sized by
`dali_transport_transaction_timeout_ms()` from the retry budget;
`SHELL_SYNC_WAIT_MS` and `SCAN_SYNC_FRAME_WAIT_MS` are gone.

**`restore apply` confirmation.** `dali_restore_confirm_move()` probes the
destination (gear QUERY STATUS, device QUERY NUMBER OF INSTANCES), then the
source, then reads the 8-byte identification number at the destination in the
move's own space and compares it with the number the plan now carries on each
move. The shell stops at the first move that is not confirmed and says why. One
deviation from the proposal: every move is checked by identity, not only
placements. The plan knows the unit on a staging hop and on a displacement too
(for a displaced unit, the number read from the bus), so there was no reason to
settle for presence there. Not done: re-quiescing for the apply. A lost move is
now reported rather than silent; if one turns up on a bus, the bracket is the
next step.

**Reply-window reference.** The TX ISR stamps `s_tx_frame_end_us` as the first
stop half-bit goes out, and completion copies it to `s_last_tx_end_us`; RX
settle suppression is still armed at completion. The scheduler measures the
reply window and `since_tx_us` from the frame end, but keeps the 22 Te
inter-frame guard and the send-twice check on the TX return, so neither gets
1.664 ms shorter. `DALI_REPLY_WINDOW_OPEN_DECODED_US` is now
`DALI_TX_STOP_BITS_US + DALI_SETTLE_MS` = 3664 and `DALI_REPLY_WINDOW_CLOSE_US`
28664: the same instants as before, expressed from the new reference. Only the
undecodable edge moved, 1.664 ms earlier, to 5.5 ms from the frame end.

The reading of IEC 62386-101 behind it is still unchecked against the text; a
search on 2026-09-25 found no public source quoting the reference point. The
evidence is the bus. From the frame end, the four fast 1k drivers settle in
5.8–7.5 ms and the ceiling drivers in 8.1–9.1 ms, all inside 5.5–10.5 ms. From
after the stop bits, the four answer as early as 4.12 ms, below the minimum.
`dali_frame.h` and `dali_protocol.md` now say this rather than cite the
standard flatly.

**`raw`/`raw2` policy.** As proposed. `dali_cli_raw_frame_is_commissioning()`
covers the 16-bit specials `dali_cli_special_is_commissioning()` names, which
include the gear memory writes, so their raw spelling is gated as `special`
gates them. It also covers SET SHORT ADDRESS on any command address (short,
group, broadcast unaddressed, broadcast), the 24-bit `0xC1` addressing specials
by their middle byte, and device SET SHORT ADDRESS on instance byte `0xFE`.
TERMINATE, COMPARE, VERIFY and the queries pass. The shell checks it through
`DALI_SHELL_ALLOW_COMMISSION`; the console refuses outright.

**ESPHome TX cache safety and `dali_phy_tx()` completion.** `to_code()` adds
`CONFIG_GPTIMER_ISR_CACHE_SAFE` and `CONFIG_GPIO_CTRL_FUNC_IN_IRAM`. The ISR's
other callees were checked in the generated sdkconfig: `esp_timer_get_time()`
is in IRAM (`CONFIG_ESP_TIMER_IN_IRAM`), and `vTaskNotifyGiveFromISR()` stays
there under `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`, which moves only
non-ISR functions. `dali_phy_tx()` reclaims a DONE state a previous frame left,
drains any stale notification before starting the timer, and loops until the
ISR's state reads `DALI_PHY_TX_DONE` rather than trusting a notification. On
timeout it stops the timer, releases the line, drains the late notice, and
returns `DALI_ERR_TIMING`. It used to return `DALI_ERR_TIMEOUT`, the code upper
layers read as a silent reply window. Not done: the IRAM-safe RX interrupt,
because of the conflict at the end of this entry.

**Events dropped in `SCHED_TX` and `WAIT_SETTLE`.** As proposed.
`sched_route_unsolicited_or_ignore()` hands any 16- or 24-bit event frame to
the subscribers in every scheduler state; the intervening-frame check inside a
live reply window is unchanged. `rx_event_unroutable` is no longer incremented
and stays in `dali_stats_t`, so the struct layout and the `stats` line keep
their shape. The `discover` event note now sums routed, unroutable and
no-subscriber events, since counting only the unroutable bucket would now
report a quiet bus.

**`dali_sched_is_quiescent()` race**, from *Smaller findings*. `SCHED_IDLE`
sets `SCHED_TX` inside the critical section that pops the transaction.

`DaliRestoreMove` was also reordered so its two enums lead. With the
identification number appended after them, the 188-move static plan grew by
2,256 B; reordered, by 1,504 B.

### Part 103 addressing encodings: what the sources say

Not changed. The evidence now points one way on both encodings, but a harmless
bus check settles the first, and addressing constants are a stack change that
waits for a go-ahead.

INITIALISE (device) — three sources, three mappings:

| Source | Devices without a short address | One device, by address | All devices |
|---|---|---|---|
| Beckhoff `Tc3_DALI`, `FB_DALI103Initialise` | `0x7F` | `0x00`–`0x3F` | `0xFF` |
| Espressif `esp_dali` documentation | `0x00` | — | `0xFF` |
| Tasmota `xdrv_75_dali.ino` | `0xFF` | — | `0x7F` |

Beckhoff is the only one that documents all three cases, and its Part 102 page,
`FB_DALI102Initialise`, gives the familiar Part 102 table instead — `0x00` all,
`2#0AAA_AAA1` one address, `0xFF` unaddressed — so the Part 103 table is a
deliberate difference, not a copy. Espressif's `0x00` is where this project's
value came from; under Beckhoff's table it selects d0. Tasmota's values are
swapped against Beckhoff's, but its "all" mode clears every device address
before sending `0x7F`, which works under either table, so its behaviour decides
nothing. python-dali defines the command and documents no parameter values.

If Beckhoff is right, `commission devices` on 2k initialises the Steinel at d0
rather than unaddressed devices, randomises it, and programs it to the first
free address — off d0, so every sensor entity keyed to it goes quiet. The
planned "clear the Steinel, then re-commission it" test would find nothing
instead: once cleared, the Steinel has no address, and `0x00` selects the
device at d0.

Device SET SHORT ADDRESS (DTR0) is fed `(a << 1) | 1`. Tasmota's define reads
`0x14 // REPEAT - DTR0 0..63` and clears by loading MASK. Beckhoff gives the
Part 103 `shortAddress` variable as `0…63, 255` with 255 as MASK, and
`FB_DALI103ProgramShortAddress` as taking `0…63, 255`; no Beckhoff SET SHORT
ADDRESS page turned up to quote, so that half is inference. Raw is the likelier
encoding.
If it is raw, `address dN set dM` writes DTR0 = 2M+1: for M < 32 the unit lands
on d(2M+1) and the verb's read-back reports the failure; for M ≥ 32 the value is
neither 0–63 nor MASK and presumably ignored. `clear` is unaffected: `0xFF`
either way.

The check changes no address: INITIALISE (device) `0x00` twice, VERIFY SHORT
ADDRESS `0`, TERMINATE. A YES means `0x00` selected the Steinel at d0, as
Beckhoff's table says; silence means it did not. Repeating with `0x7F`
(expect silence, the Steinel is addressed) and `0xFF` (expect YES) covers the
rest of the table. Change the SET SHORT ADDRESS encoding only after that, and
prove it with one `address dN set dM`, whose read-back is the test.

### The DALI PHY can disable ESPHome's pin interrupts

`dali_phy_init()` installs the GPIO ISR service with
`gpio_install_isr_service(0)` and accepts `ESP_ERR_INVALID_STATE`, so it works
whoever installs first. ESPHome's `ESP32InternalGPIOPin::attach_interrupt()`
(`esp32/gpio.cpp`, 2026.9.0) does not return the favour. It keeps a static
`isr_service_installed` flag, calls
`gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3)` the first time, and on
anything but `ESP_OK` logs `attach_interrupt(): call to
gpio_install_isr_service() failed` and returns before `gpio_isr_handler_add()`.
The pin's interrupt type and enable are already set by then, so it interrupts
into a service with no handler for it.

`DaliComponent` and `GPIOBinarySensor` both return `setup_priority::HARDWARE`,
and ESPHome orders setup with a stable insertion sort, so registration order
decides. When the DALI component goes first, a `gpio` binary sensor never
updates: on ESP32 it is interrupt-driven by default (the schema falls back to
polling only for expander pins, ESP8266 GPIO16, and shared pins) and disables
its loop between interrupts. Anything else attaching a pin interrupt after
DALI's setup fails the same way. Neither tracked config has one, and this is
read from source, not reproduced. `use_interrupt: false` on the sensor is the
workaround.

The fix also decides the IRAM-safe RX item, so the options:

1. Install the RX handler after ESPHome's setup has run. ESPHome then always
   owns the service and DALI takes its accepted `ESP_ERR_INVALID_STATE` path.
   RX stays masked during flash writes, as it is today.
2. Attach RX through ESPHome's pin API. Same service, same limitation, but no
   ordering hazard by construction.
3. Upstream: have ESPHome accept an installed service. DALI could then keep
   installing first and choose `ESP_INTR_FLAG_IRAM` for everyone, which obliges
   every handler on the service, ESPHome's included, to be IRAM-safe.

## Stack review: faults where the code meets what the host suites stand in for — found 2026-09-25

A fresh read of the whole stack at `7f26fef` (`v2.0.0` plus the sample-config
pin bump), looking for bugs, edge cases, and claims in `current_status.md` that
no longer hold. No code changed and nothing touched a bus.

The core modules held up under a close read: the scheduler, the restore
planner, the commissioning walk, light write arbitration (including its
interleavings with stale readings), the snapshot codec, the CLI parsers, and
Part 103 event decoding. The findings cluster where the code meets something a
host test can only imitate: the IEC timing reference, the ESP-IDF interrupt
configuration, the blocking shell transport, other bus masters, and the network.
Nothing below is hardware-verified. Where a finding rests on my reading of an
IEC text rather than on the code, it says so and says what would settle it.

### Occupied addresses can read as free through the TCP shell

Three facts combine:

- `DaliComponent::try_claim_bus()` only raises the gate. It does not wait for
  work already queued by the refresh pump, sensor polls or light writes to
  drain. The button scan's task does (`scan_wait_for_quiescent_scheduler()`);
  none of the eight claimed shell workflows does.
- `shell_sched_sync_impl()` waits `SHELL_SYNC_WAIT_MS` = 200 ms for a single
  frame, whatever its retry budget and whatever is queued ahead of it.
- When that wait expires it returns `DALI_ERR_TIMEOUT`, the code the scheduler
  uses for a silent reply window.

QUERY STATUS to a present address answers in ~35 ms, so 165 ms of queued work
ahead of it is enough. That is not rare: `release_bus()` requests a full refresh
pass at the end of every claimed workflow, and every sensor poll that fell due
during the claim is admitted in the same loop pass (due timestamps do not
advance while paused). The next verb an operator types after `discover` is the
likeliest moment. A device-type enumeration is ~200 ms on its own. On 2k, four
Steinel sensor polls plus one refresh item is more.

When the wait expires on an occupied address, `shell_address_presence()` maps it
to `ABSENT`. `address aN set aM` then writes onto an occupied `aM`, and a
`commission` pre-scan offers an occupied address as free — a0 first, since it
is probed first. Native UART builds are unaffected: nothing else enqueues there.

Fix, in order of value:

1. Drain in `try_claim_bus()`: after raising the gate, wait (bounded, as the
   scan task does) for `dali_sched_is_quiescent()`, and refuse the claim if the
   queue does not drain. One change covers every claimed workflow.
2. Give an expired completion wait its own error, so no caller can read "we
   stopped waiting" as "the bus was silent". Presence probes and
   `scan_error_is_absent()` then treat it as unknown.
3. Size the single-frame wait from its retry budget, the way
   `dali_transport_sequence_timeout_ms()` sizes sequences.

### `restore apply` does not confirm a move before the next one depends on it

`shell_restore_apply_move()` reports transmission. A send-twice SET SHORT
ADDRESS pair that a unit silently discards reads as `OK`, and the loop moves on.
Another master can corrupt the pair and nothing here can detect a collision. In
a cycle, a staging hop that did not land is followed by the placement onto the
address it was meant to vacate. That puts two units on one address, from the
verb whose header says it never does. Quiescence is released at the end of the
planning scan, so control-device events are flowing during the apply.

Fix: after each move, probe `to` present and `from` silent, exactly as `address
set` does, and stop on anything else. For a placement, also compare the Bank 0
identification number at `to` with the snapshot entry. Consider re-quiescing for
the apply. The status file's "convergent on re-run" holds only once this is in.

### The reply window is measured from after the stop bits

`dali_phy_tx_isr()` stamps `s_last_tx_end_us` when `s_tx_half_bit_idx` reaches
the total, one half-bit after the last *stop* half-bit went out: 4 half-bits,
1.664 ms after the last data bit ended. Every attribution edge is relative to
that stamp.

My reading of IEC 62386-101:2014 is that the 5.5–10.5 ms forward-to-backward
settling time runs from the end of the forward frame's last bit, with the stop
condition inside it. That needs confirming against the standard text. If it
holds:

- `DALI_REPLY_WINDOW_OPEN_US` opens 7.16 ms after the frame, not 5.5 ms.
  Undecodable activity from conformant gear that starts between the two is
  counted `rx_undecodable_ignored`, and the window times out. That covers a
  COMPARE collision (read as NO), a contested QUERY STATUS (read as absent, so
  free), and VERIFY's MULTIPLE. The decoded edge is unaffected.
- The 1k figures were measured from the same stamp (*1k bus: gear that replies
  just before the attribution window opens*, below). 4.12–7.4 ms after "TX bus
  release" is 5.8–9.1 ms after the last data bit, inside the standard's range.
  The "non-conformant" conclusion there, and in the comment on
  `DALI_REPLY_WINDOW_OPEN_DECODED_US`, is probably a reference-point artefact.
  Those four drivers are exactly the ones whose collisions the undecodable edge
  discards.

Fix: stamp the frame end in the ISR when the first stop half-bit goes out (the
bus is released there), measure both open edges from it with their current
values, and add 1.664 ms to the close so the effective deadline is unchanged.
Leave the settle suppression where it is. Update the scheduler vectors and both
comments. Check on 1k with a narrow capture of a two-unit COMPARE.

### Part 103 addressing encodings are unverified and look wrong

`dali_protocol.md` already says the Part 103 special opcodes were "transcribed
from Espressif's `esp_dali` and are not verified against the standard text or on
a bus". Two parameter encodings built on them need checking before the P0
hardware session that plans to use them:

- `DALI_DEVICE_INITIALISE_UNADDRESSED_PARAM` is `0x00`. My understanding of IEC
  62386-103 is that INITIALISE (device) takes a raw short address (0–63 selects
  that device), `0x7F` selects devices without a short address, and `0xFF` all.
  If so, `commission devices` on 2k initialises the Steinel at d0, randomises
  it, and programs it to the first free device address. Every sensor entity
  keyed to address 0 goes quiet. The planned "clear the Steinel, then
  re-commission it" would find nothing, because `0x00` would then select a
  device that no longer exists.
- `DALI_CMD_DEVICE_SET_SHORT_ADDRESS_DTR0` is fed `(a << 1) | 1`. Part 103
  PROGRAM SHORT ADDRESS is raw (the project's notes agree), and I would expect
  SET SHORT ADDRESS (DTR0) in the same part to be raw too; the comment's reason,
  "because it reads DTR0", does not decide an encoding. If it is raw, `address
  dN set dM` lands at 2M+1, or is ignored for M ≥ 32, and a device-space
  `restore apply` misplaces units. `clear` is unaffected: 0xFF means none either
  way.

Fix: settle both from the standard text, cross-checked against an independent
implementation such as python-dali rather than `esp_dali` again. A
non-destructive bus check settles the first: on 2k, send INITIALISE (device)
`0x00` twice, VERIFY SHORT ADDRESS `0`, then TERMINATE. A YES from the Steinel
means `0x00` selected d0.

### `raw` and `raw2` bypass the commissioning policy

The console refuses the commissioning specials and SET SHORT ADDRESS,
explicitly "so the gate cannot be walked around by choosing the other
spelling". The shell's `special`, `config`, `address` and `restore apply` honour
`DALI_SHELL_ALLOW_COMMISSION`. But `raw2 A500 len=16` followed by `raw2 A700
len=16` (INITIALISE all, RANDOMISE) goes through on both. The TCP port is
unauthenticated, and `allow_commissioning: false` is the default.

Fix: one shared classifier in `dali_cli.c`, `dali_cli_raw_frame_is_commissioning()`,
covering:

- the 16-bit specials `dali_cli_special_is_commissioning()` names;
- addressed SET SHORT ADDRESS (odd address byte, opcode 0x80);
- the 24-bit Part 103 addressing specials (0xC1 0x01..0x08);
- device SET SHORT ADDRESS (instance 0xFE, opcode 0x14).

Refuse through the policy in `cmd_raw()` and in `console_raw_()`, and give the
classifier host vectors.

### The TX bit clock is not cache-safe in ESPHome builds

The native `sdkconfig.defaults` sets `CONFIG_GPTIMER_ISR_CACHE_SAFE`,
`CONFIG_GPIO_CTRL_FUNC_IN_IRAM` and related options.
`esphome/components/dali/__init__.py` sets no sdkconfig options at all. Both
generated ESPHome sdkconfigs on disk have `GPTIMER_ISR_CACHE_SAFE` and
`GPIO_CTRL_FUNC_IN_IRAM` unset. The RX handler goes through
`gpio_install_isr_service(0)`, without `ESP_INTR_FLAG_IRAM`, in both builds.

A non-IRAM interrupt is masked while the flash cache is off, which ESPHome does
for every NVS commit: light restore state, the group map, the address backup. A
commit during a transmission holds the bus at whatever half-bit it was on. The
frame is corrupted on the wire and still counted in `tx_frames_ok`, and RX edges
during a commit are lost. If the stall also trips the `dali_phy_tx()` timeout
at the wrong moment, a late DONE notification is left pending. The next
`ulTaskNotifyTake()` then returns at once and stops that frame before its start
bit, and it too is reported sent.

Fix:

- Call `add_idf_sdkconfig_option()` for `CONFIG_GPTIMER_ISR_CACHE_SAFE` and
  `CONFIG_GPIO_CTRL_FUNC_IN_IRAM` in `to_code()`.
- Register the RX interrupt as IRAM-safe, checking what happens when another
  component installs the shared ISR service first.
- In `dali_phy_tx()`, drain notifications before starting and after a timeout,
  and require `s_tx_state == DALI_PHY_TX_DONE` before reporting success.

Bench check: stream frames while forcing preference writes, and compare
`tx_frames_ok` against a capture.

### Events decoded while the scheduler is in `SCHED_TX` or `WAIT_SETTLE` are dropped

*Ignored events arrive in our gaps*, below, reads `rx_event_unroutable` as a
counter that overstates collisions. It is also lost data, outside walks too:
`sched_route_unsolicited_or_ignore()` gives such a frame to no subscriber, so
dispatch and poll-on-event never see it.

There is a deterministic path. With an event on the wire, a newly queued
frame's `dali_phy_tx()` busy-waits for it to end and then transmits.
`dali_phy_rx_process()` cannot run meanwhile, so the event completes only after
our TX, with the scheduler in `WAIT_SETTLE`. So any event that overlaps the
start of a locally queued frame is dropped. For headless dispatch, that is a
button press that does nothing.

Fix: route unsolicited 16/24-bit frames in every state, and keep the
intervening-frame invalidation for frames inside the active reply window. The
fan-out's contract (no blocking, no re-entry) already holds in any state, and
enqueueing from a subscriber is only a queue push. Host vector: an event
delivered in `SCHED_TX` and in `WAIT_SETTLE` reaches the subscribers.

### A candidate mechanism for the retry backoff

This adds to the collision reading in *Ignored events arrive in our gaps*
rather than replacing it.

`wait_for_bus_idle_before_tx()` wants 2 bit periods (1.67 ms) of idle, and the
scheduler's 22 Te spacing counts only from our own transmissions. So when a
control-device event ends while a frame is queued, that frame starts ~1.7 ms
later. That is under the 2.4 ms a DALI-2 receiver needs to see a stop
condition, and far under any multi-master forward-frame settling time. A
receiver that discards the frame produces a silent timeout.

The unit most exposed is the emitter, which has just finished transmitting:
a0, whose own QUERY NUMBER OF INSTANCES went missing on 2026-09-04. A retry
fired the instant the window closes (27 ms, longer than the Steinel's 14–21 ms
priority settling) tends to find the next event in progress and repeat the
pattern. The backoff breaks that phase.

Test: have the RX ISR keep the last edge time (it already has the timestamp),
count transmissions started less than 2.4 ms after it, and correlate that with
lost replies to present gear. If it tracks, require at least 2.4 ms since the
last observed edge — better, a proper settling time after another master's
forward frame — and re-measure without the backoff.

### Smaller findings

- `DaliComponent::load_address_backup()` calls `backup_pref_.load()` on the
  shell task. ESPHome 2026.9's `ESP32PreferenceBackend::load()` walks the
  unlocked `s_pending_save` vector, which `save()` (a `push_back`) and `sync()`
  mutate on the loop task. That is a data race with a reallocation in it. Load
  the blob once in `setup()` and serve `load_address_backup()` from
  `s_address_backup` only.
- `DaliShellServer` never sets `SO_SNDTIMEO`. A client that stops reading
  without closing, such as a laptop asleep mid-`discover`, blocks `send()`
  inside the workflow. The bus gate stays raised, and quiescence or an open
  INITIALISE window may stay in force, until lwIP gives up minutes later. The
  idle timeout is only checked between lines. Set a send timeout of a few
  seconds, treat `EAGAIN` as peer lost, and consider `SO_KEEPALIVE`.
- The 33rd light, 17th sensor and 33rd dispatch rule are dropped by
  `register_light()`, `register_input_sensor()` and `add_dispatch_entry()` with
  only a boot-time log line. An unregistered light never receives a level
  profile, so it can never transmit. Validate the counts in the schema, and size
  the light registry for 64 + 16 + 1.
- `discovery_scan_walk()` aborts the whole walk on any per-address error other
  than TIMEOUT, MALFORMED or RX_ACTIVITY. One `INTERVENED` from a coupler's
  button press ends a 45 s scan. Retry the address once; if the error persists,
  record the address as unreadable and reserved, and carry on.
- `restore_find_spare()` can stage or displace onto an address the backup
  records for a unit that is currently missing. If that unit is only unpowered,
  it comes back to a contested address. Prefer spares outside
  `dali_snapshot_used_mask()`.
- The `v2.0.0` tag's `dali-starter.yaml` and README pin `v1.3.0`; `7f26fef`
  moved them after tagging. Make the pin bump part of the release-prep commit,
  and have `release-packaging.yml` assert that a tag's starter config pins that
  tag.
- ESPHome 2026.9 pins `loopTask` to core 1 (`esp32/core.cpp`), the core
  `dali_worker_core()` gives the DALI, scan and shell tasks. Every "Core 0" note
  in the component, and the rationale in `dali_core_affinity.h`, is wrong.
  Synchronization is unaffected, since spinlocks and atomics work on one core.
  But the DALI task's busy-waits (1.67 ms per frame, 33 ms per attempt on a
  stuck bus) run at priority 10 on the loop's core. Correct the comments, and
  let the idle check use the ISR's last-edge time instead of polling.
- The TCP shell's single-frame verbs (`status`, `query`, `raw`, `config` and
  others) do not observe the scan gate, so they interleave with a button scan.
  The P0 item's "new scheduler client outside `DaliComponent`" already exists.
- `dali_sched_run()` pops under the lock but sets `SCHED_TX` after releasing
  it, so `dali_sched_is_quiescent()` can report quiescent while a popped
  transaction is about to transmit. Set the state inside the same critical
  section.
- `dali_rb_clear()` zeroes both indices. In the native build the ISRs run on
  core 0 (installed from `app_main`) and the DALI task on core 1, so a push in
  flight during `reset` can leave `head` far ahead of `tail`. Clear from the
  consumer side only, by setting `tail = head`.
- `dali_shell.h` exports `DALI_SHELL_CAPTURE_MAX` 64 and the switch-mapping and
  sensor-cache limits at 16. `dali_shell.c` uses private values of 128, 32 and
  64, and never reads the public ones.
- HA brightness maps linearly onto light output and ignores ESPHome's
  `gamma_correct` (default 2.8). A DALI light therefore sits much brighter at
  mid-slider than any other ESPHome light, and the dimmer half of the arc power
  range lives in the bottom few percent of the slider. This is a decision to
  make on purpose, not a bug.
- `devmem write` is not policy-gated, although the Part 102 memory-write
  specials are.
- The sensor poll sequence gives QUERY INPUT VALUE no retry, although
  restarting the latch is safe.
- A `backup import` blob carries no checksum, so a mistyped hex digit can
  decode as a plausible snapshot.
- `trace on` on the native CLI writes to UART0 from the DALI task. Once the
  1 KB TX buffer fills, it blocks reply handling.

### Status claims corrected

- `main` is `v2.0.0` plus `7f26fef`, not the tag itself.
- The ESPHome version the schema and compile were checked against is 2026.9.0.
  One "Not yet verified" line still said 2026.8.1.
- "The Home Assistant text surface blocks raw commissioning primitives": it
  blocks the `special` and `config` spellings, but `raw` and `raw2` are open.
- The restore's "convergent on re-run" depends on each move landing, which the
  executor does not check.

Checked and still accurate: 31/31 host suites pass; light write arbitration; the
scan gate for every component-owned producer; `discover`'s silent enumeration
failure; no addressing fault reaching Home Assistant; console `OK` meaning
queued.

## `restore` treated a contested address as free — found 2026-09-04

Found while adding `address <aN> clear`, in the module that had the strongest
claim not to have this bug. `dali_restore.h` opens by saying the plan exists so
that nothing is ever moved onto an address another unit still holds, "the
contested-address fault that nothing on the bus can undo remotely". The planner
could create exactly that.

`restore_collect_bus()` skipped every inventory entry that was not `present`.
A contested address is deliberately not `present` — the scan records
`has_undecodable_activity` and leaves the entry absent, because something
answered but nothing could be read from it. So a contested address entered the
planner's occupancy model as free, on all three routes to an address:

- as a **placement target**, if a backup entry recorded a unit there;
- as a **staging address**, borrowed to break a cycle;
- as a **displacement address**, to park a unit the backup has never seen.

The third is the worst, because it is the one the planner picks on its own
initiative. A restore run to tidy a scrambled bus could take a working,
identifiable unit and park it on top of two units already fighting over one
address — turning a two-unit collision into a three-unit one, and doing it to a
unit that had nothing to do with the fault.

`dali_commissioning.c` and `dali_device_commissioning.c` both already got this
right; their free-address masks reserve on `has_undecodable_activity` with a
comment saying why. The restore planner was written against `present` alone and
never revisited when the contested classification was added.

**Fixed** by collecting contested addresses as `reserved` rather than skipping
them, and folding `reserved` into both `occupied` (which every free-address
decision reads) and `immovable` (so a blocked move is reported rather than
stalling until the loop limit calls the plan incomplete). Each space reads its
own flag, so a contested `d7` still leaves gear `a7` movable.

Blocked moves report a new `DALI_RESTORE_CONFLICT_TARGET_CONTESTED` rather than
`TARGET_OCCUPIED`. The distinction is not cosmetic: `target occupied` sends an
operator to look up what is sitting on the address, and the answer here is that
nothing addressable is — the remedy is `address <aN> clear`, then
`commission unaddressed`, then plan again, which is the sequence the `clear`
work exists to make possible.

Seven host vectors in `test_restore_plan.c`, and `replay_and_assert_safe()` —
the helper that replays a plan and asserts no move ever lands on an occupied
address — now seeds contested addresses as occupied, which it did not before. It
would have waved this bug through. Six of the seven fail with the occupancy fold
disabled; the seventh is the over-reservation guard and passes either way, which
is the point of it.

Still unproven on a bus, for the reason everything contested is: no genuine
two-unit collision has been driven through a scan, so the input to all of this —
`has_undecodable_activity` being set by real overlapping replies — is asserted
from the standard rather than observed.

## Ignored events arrive in our gaps, not on our frames — found 2026-09-04

Retires the decisive test proposed at the end of the entry below, and re-reads
the counter that test leaned on. Neither entry below is edited.

**`TX retries` cannot see reply loss.** The test offered below assumed the
counter mostly reflects frames that had to be re-sent because something went
wrong. It does not. `sched_retry_active_step()` increments it on every reply
timeout, and a 64-address walk times out on every address where nobody lives.
`DALI_DISCOVERY_QUERY_RETRIES_LEFT` is 1, so each failing query is two attempts,
two timeouts and one retry — and the measured deltas are exactly that: 132
retries against 264 timeouts, on a **fully quiesced** walk. The predicted floor
is 59 absent addresses x 2 probes (QUERY STATUS, then QUERY NUMBER OF INSTANCES)
x 1 retry = 118; the remaining ~14 are enrichment and instance probes on the
five present devices. There is no headroom in the counter for a collision
signal, on either bus.

Two side facts worth keeping. Every one of those 132 retries pays
`DALI_REPLY_TIMEOUT_BACKOFF_US`, so ~1.5 s of each walk is backoff alone,
against 6.6 s of reply waiting (264 x 25 ms). And a reply killed by the
intervened path below increments nothing at all, so it is indistinguishable
from silence in `stats`.

**`rx_event_unroutable` does not mean the event hit us.** `SCHED_TX` is not just
the frame: the state returns early while `sched_tx_guard_active()` holds, so the
scheduler sits in TX through the whole inter-frame guard — 9.17 ms normally,
11.16 ms after a retry backoff — and then through the blocking PHY call.
`sched_can_route_unsolicited_event()` admits only `SCHED_IDLE` and
`SCHED_WAIT_REPLY`, so an event arriving during that guard is counted unroutable
even though the bus was idle and nothing collided. A sizeable share of a walk is
spent in a state that classifies events this way, most of it genuinely quiet
wire — the exact fraction is not known, since a 29 s walk is far longer than its
264 attempts account for and the surplus may be `SCHED_IDLE` between
transactions, which routes. The counter name is accurate; the scan note's
"arrived mid-walk" framing is what invites the wrong reading.

**An event that really did land in a reply window would abort the scan.** When a
foreign forward frame arrives inside the window, `s_reply_intervened` is set,
the transaction finishes `DALI_ERR_INTERVENED`, and `scan_error_is_absent()`
does not list that error — so the scan returns it and prints `Scan ERR
intervened` rather than quietly losing one address. That has never been seen, on
any walk, on either bus. The control devices are staying out of the
backward-frame slot, which is what Part 101 requires of them, and emitting into
our gaps instead. The 36 events of the 2026-09-04 walk interfered with nothing.

**The emitter is a metronome, and the walk's 36 is unexplained by it.** An idle
`capture` on 2026-09-04 (111 s, 40 events) settles what a0 actually sends:
device 0 / instance 0 emits `0x008001` every **3.000 s**, with 37 intervals all
between 2998 and 3004 ms and `event_information` fixed at 1; device 0 /
instance 1 emits `0x00840C` every 30.01 s, also with a constant payload. Both
are timers, not value reports — the lux entity reads 0.64 and never moves while
the frames keep coming. So occupancy has nothing to do with the count, which is
why "nobody was moving" and "36 events" were never in conflict. Home Assistant
sees none of this: its recorder stores only value changes.

That capture also puts 40 events over 111 s = **0.36 events/s**, matching to two
figures the 0.36/s derived independently from lifetime counters (3553 routed
plus 242 unroutable over ~2 h 56 m of uptime). The baseline is solid.

**A walk is 29 s, so the excess is real and it is about 3x.** Timed on
2026-09-04, two consecutive walks, 33 events noted on each. In 29 s the two
timers can emit at most ~10.6 frames — 9.7 lux at 3.000 s plus one occupancy at
30.01 s. The note counts only `rx_event_unroutable`, which is a *subset* of the
frames received, so 33 is a floor on how many event frames reached the wire:
**at least 3.2x the idle emission rate**, and the true multiple is higher by
whatever fraction arrived while the scheduler was idle and got routed instead.
No assumption about where the walk spends its time is needed for that bound,
which is why it is worth stating this way.

The reading that fits: a multi-master control device that keeps losing its slot
to a dense walk detects the collision and retransmits, so each timer tick
reaches the wire as three or more frames. It predicts the count scales with walk
density rather than with occupancy — and the two 29 s walks noting 33 apiece,
against 36 for a walk on the same build the same evening, is the stability that
prediction wants. It is also consistent with `Malformed frames` staying at 2
throughout: retried frames arrive intact, whereas frames aborted mid-collision
would not.

**The same session lost a query to a present device, and the code says which
one.** On the first of the two walks, a0 was detected as
`input-device(4 instances)` — so QUERY NUMBER OF INSTANCES was answered during
the walk — but the follow-up `dali_discovery_query_input_device()` failed and no
`00: 4 input instance(s) enumerated` line printed. The second walk, run
immediately after, enumerated it.

Enumeration is not one frame. It is QUERY NUMBER OF INSTANCES, then five queries
per instance — instance type, enabled, resolution, instance status, instance
error — so a0 costs **21 device-addressed queries** and a1 costs six. Of those,
**only the first can fail the call**: `query_instance_u8()` results are recorded
per instance in `instance_type_errors[]` and skipped, and the function still
returns `DALI_OK`. So the missing line means QUERY NUMBER OF INSTANCES was lost,
both attempts of its one-retry budget — the same command, to the same device,
that had answered seconds earlier in the same walk.

This is the missing-lamp failure in miniature and on the *current* build. That
it struck a0 — the emitter itself, the one device whose own event frames contend
with its own reply window — is the strongest circumstantial support the
collision reading has.

It also exposes a shell defect, independent of any of this: that failure is
silent. The enumeration loop only prints on `DALI_OK` and has no else branch, so
the operator sees a device line claiming four instances, no enumeration line,
and no error.

**A cheaper test than the pre-backoff build.** Run `discover` repeatedly on 2k
and count how often a0's enumeration line is missing; then repeat under
`quiescent on all`. If event traffic is losing queries to present devices, the
failure rate should go to zero under quiescence. That measures event-induced
query loss directly, on the build that is already flashed.

**What this does not settle.** The backoff's mechanism is untouched. The
intervened path cannot be it, since it aborts scans rather than dropping
addresses and has never fired. The collision reading in the entry below stands
as the leading candidate, with one addition: the backoff also hands the bus
11 ms of guaranteed idle per retry, which is the slot a deferring control device
needs to drain, so it may reduce that device's own retransmissions as well as
spacing ours.

**Two smaller notes on method.** `sched_trace()` fires before classification, so
unroutable events do reach `capture` as raw `rx` records — but the ring holds
128 records and a walk emits several hundred traces, so capture idle rather than
across a walk to see what the emitter sends. And bracket each walk with `stats`
on *both* sides: the 2026-09-04 session bracketed only the quiesced walk, which
is why its unquiesced deltas are unrecoverable.

**Worth adding to the firmware.** A counter on the `s_reply_intervened` path. It
is the one reply-loss mode that increments nothing today.

## The 2k "late gear" reading was event traffic — settled 2026-09-03, 22:20

Supersedes the *mechanism* conclusion of the 2026-08-13 2k diagnosis, and closes
the doubt left open at the end of *The scan's unattributed-RX note is a motion
detector* below. Neither entry is edited; both stand as they were written.

The 2026-08-13 diagnosis read ~25 `rx_ignored_outside_reply` per scan on 2k
against exactly 0 on 1k as gear answering past `DALI_REPLY_TIMEOUT_MS`, and
re-armed the retry guard with `DALI_REPLY_TIMEOUT_BACKOFF_US` on the strength of
it. The counter conflated four facts, so the reading was never separable from
three others. It is separable now.

Measured on `b64f81f` (see the verification entry of the same timestamp): three
unquiesced 64-address walks on 2k, **`rx_reply_late` = 0 on all three**. Not
"near zero" — the note that prints whenever early or late is nonzero never
printed, on any run. No control gear on the 2k bus answers past the 25 ms
window. The 2k-versus-1k split that carried the whole argument was the Steinel
at a0 emitting on four instances, which is what the 14-17 events per walk are,
and which 1k has nothing to produce.

**The fix is not in question; the story about it is.** The backoff's effect was
measured directly and independently on 2026-08-13: 8/8 scans on 2k found all
five lamps afterwards, against 2/8 before. That number stands. What cannot
stand is the explanation attached to it in the comment on
`DALI_REPLY_TIMEOUT_BACKOFF_US` in `dali_frame.h` and on
`sched_retry_active_step()` in `dali_scheduler.c`, both of which describe gear
"that answers a shade later than the timeout" still driving the bus when the
retry goes out. Nothing on this bus answers late, so the retransmission was
landing on something else.

Leading candidate, offered as inference and not as measurement: the collision
is with a Steinel event frame, and it happens twice. A 24-bit event frame is
~20.8 ms on the wire (25 bit periods at 833 µs), the walk is back-to-back TX for
minutes, and the sensor emits asynchronously on four instances. An event that begins after the TX idle guard
has been checked corrupts the forward frame mid-flight; the gear never decodes a
query, so it never replies, and the 25 ms window expires as a timeout rather
than as a late answer. Pre-fix, the retry then went out the instant the window
shut — still inside the same event burst — and died the same way, and
`scan_error_is_absent()` folded the second timeout into "absent". The backoff's
11 Te plus settle happens to clear the burst as well as it clears a straggler,
which would make it the right fix for the wrong reason.

The objection to that story, kept here so it is not re-derived: the scheduler
does not transmit onto an already-active frame, since it waits for bus idle
first. The collision therefore has to be an event *starting* during our forward
frame or reply window, not one already in progress. That is possible on an
asynchronous emitter and is consistent with the frequency, but it is narrower
than "the bus is busy".

**Decisive test, if this is worth closing properly.** Build the pre-backoff ref,
and run repeated scans on 2k under `quiescent on all`. If the missing-lamp rate
collapses from ~25% to zero with the backoff absent, event collision is
confirmed as the mechanism and the comments can be rewritten with evidence
behind them. If lamps still go missing under quiescence, the cause is something
neither entry has named yet. Until one of those runs, the honest statement is
that the backoff works and nobody knows why.

## The scan's unattributed-RX note is a motion detector — found 2026-09-03

Every `discover` on the 2k bus carried `note: N RX observation(s) fell outside
active reply attribution`, with N running 11 to 39 across the session. The note
advises inspecting timing if a known device is missing, which sends the reader
after a PHY fault. There is no PHY fault. N is a readout of whether a person is
standing in the corridor.

The mechanism is `sched_route_unsolicited_or_ignore()`. An unsolicited event
frame is routed only in `SCHED_IDLE` or `SCHED_WAIT_REPLY`
(`sched_can_route_unsolicited_event()`); anything arriving in `SCHED_TX` falls
through to `g_dali_stats.rx_ignored_outside_reply++`. A 64-address walk is
back-to-back TX for minutes, and the Steinel at a0 emits on four instances
throughout, so a large fraction of its events land in the window that counts
them as ignored.

Two independent confirmations, which is why this is recorded as settled rather
than as a theory:

- The one scan of the session run under `quiescent on all` printed **no note at
  all**. It returned on the next scan after `quiescent off all`. Silencing
  control-device events silences the counter.
- Home Assistant's recorder for the same wall-clock window:
  `sensor.2k_koridor_dali_2k_zone_2_occupancy` changed state 31 times between
  19:55 and 20:13 local, cycling `Present` / `Present + Moving` / `Unoccupied`
  every 10 to 30 seconds, with lux updating 18 times alongside. The operator
  confirms being in the room; the sensor was tracking them. The scans with the
  lowest counts (14, 16, 11) fall in the part of the session where the recorder
  shows `Unoccupied` stretches at 20:04:23, 20:10:03 and 20:11:51. The shell log
  carries no timestamps, so this is a consistent shape rather than a proof of
  alignment.

The operational consequence is worse than misleading copy. Commissioning
*requires* a human in the room — `identify` is useless unless someone is
watching the lamp blink, and it was run five times in this session. So the note
is loudest exactly when the operator is doing the work it interrupts, and
quietest when nobody is present to read it. It is anti-correlated with the fault
it claims to report.

**This counter has now been used to diagnose three different faults on two
buses, and it cannot tell them apart.** `sched_is_unsolicited_event_frame()`
accepts only 16- and 24-bit frames, so an 8-bit backward frame — a gear reply —
is never routable and always lands in `rx_ignored_outside_reply`. Four distinct
facts share the number:

1. **Early** replies from gear settling faster than the 5.5 ms window open. The
   2026-08-25 1k investigation in this file reads the deltas 58/77/113 as
   exactly this, correctly.
2. **Late** replies from gear answering past `DALI_REPLY_TIMEOUT_MS`. The
   2026-08-13 2k diagnosis reads ~25 per scan as this, and fixed the retry
   backoff on the strength of it.
3. Event frames arriving outside `SCHED_IDLE`/`SCHED_WAIT_REPLY` — this entry.
4. Undecodable observations outside a reply window, the only case the note's
   own wording describes.

The 2026-08-13 reading is the one now in question. Its discriminator was that
2k showed ~25 per scan while 1k showed exactly 0 — but 2k has a Steinel
emitting on four instances and 1k has none, which explains the same split
without any slow gear. `quiescent on all` silences control-device events and
does nothing whatsoever to gear reply timing, so a count that falls to zero
under quiescent cannot have been late gear replies. On 2026-09-03 it fell to
zero. That is a single scan and wants repeating before the 2026-08-13
conclusion is called wrong, but it is the right experiment and it points the
other way.

Note this does **not** touch the backoff fix itself, whose effect was measured
directly and independently: 8/8 scans on 2k found all five lamps afterwards
against 2/8 before. The fix works. What is in doubt is the mechanism story told
about why, and the doubt exists only because the counter conflates the
candidates.

Two fixes, both worth taking:

- Split the counter, four ways or at least three: early in-window-miss, late
  post-timeout, unroutable event, undecodable. Only the first two are timing
  signals and they point in opposite directions. Until they are separate
  numbers, every future investigation that reaches for this counter inherits
  the same ambiguity — and three already have. **Done 2026-09-03**, seven ways
  rather than three; see the unreleased-changes entry below for the buckets and
  what the scan prints now. Host-covered, mutation-checked, and not yet on a
  bus.
- Assert quiescent for the duration of a scan and release it after. The scan
  already knows which addresses are input devices — it enumerates their
  instances in the same function that prints the note. Two constraints on any
  implementation: the release must cover every exit path including the cancelled
  and errored scans, or the bus is left deaf to events; and `find switches` must
  never do it, since listening for events is that verb's entire purpose.
  **Done 2026-09-03**, both constraints kept and both covered by vectors; see
  the unreleased-changes entry below. Note the order this leaves behind: the
  bracket removes exactly the traffic the split counters were built to measure,
  so `b64f81f` — split counters, no bracket — is the only build that can still
  observe the "before" arm on the 2k bus.

## `restore` cannot stage a unit the backup has never seen — found 2026-09-03

The 2026-09-03 session ended with the operator hand-executing, in three
commands, the exact cycle-staging algorithm `dali_restore.c` implements:

    address a4 set a6      (stage the blocker out of the way)
    address a5 set a4      (recorded unit to its recorded address)
    address a6 set a5

`restore apply` would not have done this. The blocker at a4 was a driver that
had been unpowered during every previous `backup save`, so it appears in no
snapshot. `restore_match_unit()` files it as `DALI_RESTORE_CONFLICT_UNKNOWN_UNIT`
and returns NULL, at which point the caller marks its address **immovable**. The
move the operator wanted is then dropped by the blocked-move loop as
`DALI_RESTORE_CONFLICT_TARGET_OCCUPIED`. The plan comes back with zero moves and
two conflicts, and the operator is sent back to doing it by hand — which is what
happened.

The header comment justifies the immovable treatment as "a restore reverts
addressing, it does not retire units nobody recorded." That reasoning is sound
and should be kept. But staging an unrecorded unit through a *free* address is
not retiring it: it ends up somewhere reachable, addressed, and discoverable,
and the operator can then decide what it is. a6 was free for the whole session.

Proposed change: when a recorded unit's target is held by an `UNKNOWN_UNIT` and
the space has a free address, plan a staging hop for the unknown unit rather
than dropping the move. Keep the current immovable behaviour when the space is
full — there, refusing is genuinely the only safe answer — and keep it for
`UNIDENTIFIED` units, which cannot be moved safely because nothing can confirm
where they went. The distinction that matters is readable identity, not
membership in the snapshot.

This is the difference between a restore that converges after a mixed
commissioning accident and one that hands the problem back.

**Done 2026-09-04**, both constraints kept and both covered by vectors; see the
unreleased-changes entry below. One thing the proposal did not anticipate: the
displacement and the cycle hop compete for the same free address and do not
spend it the same way, so the planner has to break cycles first. Detecting that
required distinguishing a real cycle from a chain ending at a displaceable unit,
which the old "pick the first stalled move" victim search could not do.

## `address` cannot un-assign — found 2026-09-03

Faced with two units contesting a4, the operator reached for
`address a4 set none`, then `address a4 set`, and got usage text both times. The
CLI spec fixes the verb at three required tokens, and every arm requires a
parseable target argument.

The fallback used was `config-dtr0 a4 set-short-address-dtr0 255`, which is
correct DALI and did the right thing — both contesting units took the broadcast
and dropped their address, leaving them to be re-commissioned cleanly. But it
printed a bare `OK`. `config` cannot read back, and against a contested address
nothing can: that is the definition of the state. The shell had `a4: contested`
in its inventory from two commands earlier and said nothing about it.

Wanted: an `address <aN> clear` arm that owns this properly — refuses or warns
when the subject is contested in the current inventory, sends the DTR0 255
sequence atomically as `address set` already does, and verifies afterwards that
the address went silent. Clearing an address is a normal step in resolving a
collision, and it is the one address operation with no typed verb.

## No addressing fault reaches Home Assistant — found 2026-09-03

Cross-checking the 2026-09-03 session against the HA recorder turned up a gap
that the shell log alone could not show.
`sensor.2k_koridor_dali_2k_bus_status` read `OK` at 19:00 and never changed —
through the contested a4, a broadcast address wipe, INITIALISE, randomise, a
full commissioning walk, and four re-addressings. One state record for the
entire window.

That sensor is not lying. `CONF_BUS_FAULT` is documented in the component schema
as `"OK"` at boot and `"Bus stuck: N"` when the bus idle timeout fires. It is a
PHY-level liveness signal and it reported correctly.

The gap is that there is **no HA-visible signal for an addressing fault at
all**. Two fixtures sharing a short address is the one failure nothing on the
bus can undo remotely; the shell detects it, names the address, and prints the
remedy — and none of that can reach Home Assistant. Had the collision happened
while nobody was attached to the shell, nothing would have reported it. The
scan already computes `inventory->undecodable_count` and the per-address
`has_undecodable_activity` flags, so the data exists on the device at the right
moment and simply has nowhere to go.

A related, smaller instance of the same shape: `light.dali_2k_dali_group_0` has
a single state record for the whole session and never went `unavailable`. As a
robustness result that is good — the integration rode out a hostile
commissioning session without flapping. But between the commissioning walk and
the operator's `address a5 remove g0`, the freshly repowered driver came up with
`groups=[0]`, so for that stretch the g0 light entity was commanding an extra
fixture. `inventory_changed` fired on every scan; group membership is not part
of what it surfaces.

Wanted: an addressing-health text sensor fed from the scan result — a second
one, or a widened `bus_fault` reporting `"Contested: a4"` — so the fault class
that actually matters is visible from Home Assistant. Group membership changes
reaching the integration is the same fix one layer along, and is what would let
a group light entity know its own membership changed underneath it.

## Hybrid units get no pairing model — decided 2026-09-03

One physical unit can be both control gear and a control device, holding two
independent short addresses in two independent address spaces. The Steinel
sensors on the 2k bus are exactly this. The question was whether the software
should record that the two halves belong to one box, warn when a walk moves one
without the other, or try to keep the two numbers equal.

**Decision: none of that. No pairing model, and nothing is owed here.** The
entry it replaces in `current_status.md` framed this as a gap; it is not one.

Nothing in the stack needs the relationship:

- **Backup and restore** already match each space against its own Bank 0.
  Reading the two spaces on the 2k bus produced *different* GTINs and different
  identification numbers from the same physical unit — recorded in the
  `DaliDiscoveryDeviceInfo::device_identity` comment — so there is no reliable
  key to pair on even if something wanted to. A pairing table would have to be
  built from numeric coincidence, which is precisely what is not evidence.
- **Free-address accounting** keeps the two spaces separate, which is correct
  and must stay that way. Gear at numeric address 7 and a control device at
  numeric address 7 are different units, and reserving one because of the other
  would refuse addresses that are free.
- **Groups, scenes, and every control operation** are gear-space only.
- **Commissioning** is per-space by construction: the Part 102 walk touches gear
  addresses, the Part 103 walk touches device addresses, and neither can move
  the other's.

The real cross-part coupling is a *protocol* one, not an addressing one: a
control device sitting in its own addressing state can answer a Part 102
COMPARE, and control gear in an open initialise window can act on Part 103
specials, because 0xC1 is both the Part 102 ENABLE DEVICE TYPE opcode and the
first byte of every Part 103 special frame. That is handled by the TERMINATE
brackets in both walks and by quiescent-mode bracketing in the gear walk. A
pairing model would not have helped with any of it.

What the decision does cost, and is accepted: removing a unit from a site frees
its gear address and leaves the device address orphaned, with nothing connecting
the two for whoever cleans up. `discover` shows the orphan; an operator joins
them by hand. That is an operational annoyance, not a correctness defect.

One thing did come out of the review as a genuine defect, and was fixed rather
than filed: a contested *device* address was invisible at any number where
control gear answered. `dali_discovery_scan()` probes the device space only when
the gear query reports absent, so the enrichment path — the one a hybrid unit
takes — dropped `DALI_ERR_RX_ACTIVITY` from the instance-count query on the
floor. Two control devices sharing a device short address were therefore
unreportable in exactly the installation where hybrids are common.
`discovery_enrich_device()` now records it, guarded so that it never overwrites a
good device-space reading already in hand.


## 1k bus: gear that replies just before the attribution window opens

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

## Equal random address and mixed-device commissioning — the 2026-08-26 audit

The ordered plan as it stood when the work was scoped. Steps marked *Done* were
closed between 2026-08-26 and 2026-09-03; the design reasoning is kept because
it is what the guards in `dali_commissioning.*` and `dali_device_commissioning.*`
encode, and it is expensive to re-derive. Open items from it live in
`current_status.md` under P0, not here.

Audited 2026-08-26. What follows was the ordered plan, with the first step
already done at the time of writing.

*Equal random address.* Two gear sharing a 24-bit random value are selected,
programmed, verified, and withdrawn as one. The run ends one of two ways
depending on how the two backward frames happen to overlap: they decode, and
the run reports success with one assignment missing and two gear on one short
address; or they collide, and `dali_commissioning_verify_from_sequence()`
propagates `DALI_ERR_RX_ACTIVITY` so the run aborts with an undiagnosed bus
error *after* the duplicate has been programmed. `test_commissioning.c`'s
`test_rx_activity_means_yes_only_for_compare` pins that propagation
deliberately.

1. **Post-scan verification. Done 2026-08-26** — see the verification entry
   above. Turns the silent-success failure into a visible one with no protocol
   change. Still open within it: the failed-run path returns before any
   post-scan, which is where a collision is most likely.
2. **Detection at VERIFY. Done 2026-08-26** — see the verification entry
   above. `RX_ACTIVITY` on the VERIFY step is now `VERIFY_MULTIPLE` rather than
   an error.
3. **Recovery. Done 2026-08-26**, and simpler than the nested per-address
   INITIALISE window sketched for it. PROGRAM SHORT ADDRESS `0xFF` plus
   WITHDRAW leaves the pair unaddressed and out of the search, the address
   unconsumed, and the run continuing — so a re-run picks them up with fresh
   random addresses. The nested window remains available if in-run placement
   is ever wanted instead of asking for a second run; it is an optimisation
   now, not a requirement.
4. **Reporting. Done 2026-08-26** — `duplicate_count`,
   `duplicate_random_addresses`, `duplicate_recovery_failed`, and a progress
   event, surfaced live and in the summary on both the success and failure
   paths.
5. **Host vectors. Done 2026-08-26** — three added, three updated, mutation-
   checked.

What remains on this half: **hardware**. Nothing in it has met a bus, and the
underlying `RX_ACTIVITY` classification still has no physical collision
capture behind it — the same HIL debt the COMPARE work carries. Until then the
single-unaddressed-device operating envelope stands, and the failed-run path
still returns before the post-scan.

*Mixed device — Part 102 gear and Part 103 control devices on one bus.* Six
distinct gaps, only one of which quiescence touches.

1. **Cross-part TERMINATE bracketing. Done 2026-08-26** — see the
   verification entry above. `DALI_CMD_FRAME_24BIT_SPECIAL`,
   `DALI_CMD_DEVICE_TERMINATE`, `dali_build_device_special()`, and a
   three-send bracket around the gear walk. Host vectors only.
2. **Quiescence does not close that path. Documented 2026-08-26**, and item 1
   is what closes it. Quiescent mode suppresses a device's own bus activity —
   event frames. It does not stop the device entering addressing state on an
   INITIALISE it observed, nor answering a COMPARE it was addressed with.
3. **There is no control-device commissioning surface at all.** No device
   INITIALISE / RANDOMISE / COMPARE / SEARCHADDRH-M-L / PROGRAM SHORT ADDRESS /
   VERIFY / WITHDRAW in `DaliCommandId`, no device-space counterpart to
   `dali_commissioning`, no verb. A bus of new sensors cannot be addressed by
   this tool — that is a Cockpit job today. Project-sized, and the largest of
   the six. The encoding half is no longer part of the cost: item 1 added
   `DALI_CMD_FRAME_24BIT_SPECIAL` and `dali_build_device_special()`, which is
   generic over the command table rather than specific to TERMINATE, so each
   further device special command is a table row. What is owed is the opcode
   set, the walk, a device-space `used_mask`, and the verb.
4. **The interference is symmetric.** `0xC1` is Part 102 ENABLE DEVICE TYPE and
   the Part 103 special-command address byte, so gear in an open Part 102
   initialise window can act on the Part 103 special frames a device run emits.
   Whenever item 3 is built it must bracket with a Part 102 TERMINATE for the
   same reason — design the guard once, in both directions.
5. **Discovery's device-space blind spot. Done 2026-08-26** — the Part 103
   instance probe records `DALI_ERR_RX_ACTIVITY` as
   `has_undecodable_device_activity` and counts it in
   `undecodable_device_count` instead of dropping it as "absent". Reported as
   `dN: contested` by both `discover` surfaces. Deliberately reserves nothing:
   `dali_commissioning_used_mask_from_inventory()` stays gear-only, because the
   spaces are independent. Item 3 still needs a device-space `used_mask`, but
   there is now something to build one from.
6. **Hybrid units have no pairing model.** One physical device can be both
   control gear and control device, with two independent short addresses. The
   gear walk can move its gear address without touching its device address,
   nothing records that they belong together, and nothing warns. Relevant to
   the Steinel units specifically; see "Observed Steinel instance layout".


Since this audit: item 3 was built (`dali_device_commissioning.c/.h`, the
`commission devices` verb, `dali_device_commissioning_used_mask_from_inventory()`,
and `test_device_commissioning.c`, commit `2ac99da`) and item 4 with it — the
options struct carries `terminate_control_gear` as the mirror of
`terminate_control_devices`. Host vectors only; none of it has met a bus.

---

# Unreleased API and operator-visible changes

Moved to `CHANGELOG.md`, under *Unreleased: v3.0.0*, on 2026-10-02.
Everything listed here since `v2.0.0` is there, grouped by kind rather than by
date, with a verification label per item.

New changes go into that section as they land. At the next tag it is checked
against the tree and renamed, rather than emptied out of this file afterwards.
The lesson that prompted the move, and the rest of the release checklist, are
in `todo.md`, under *Release process*.
