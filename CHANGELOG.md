# Changelog

Notable changes per release. Verification status is stated per item, using the
same three labels as the rest of this repository: **hardware-verified** (a real
bus produced the result), **host-tested** (host vectors only, no bus), and
**unverified**.

Dated evidence for every claim here is in [project_log.md](project_log.md).

## Unreleased: v3.0.0

Everything on `dev` since `v2.0.0`. New changes are added here as they land.

At tagging:

- check each claim against the tree (`git grep <symbol> v2.0.0` settles most
  of them in seconds)
- re-check each label against `project_log.md` and `todo.md`
- rename this heading to the version

**This is a breaking release.**

| Who | What breaks |
|---|---|
| Anyone holding a backup | A backup taken by `v2.0.0` does not load: the format is now version 2 |
| C API consumers | Several changes fail to compile, and several change what a value means |
| Out-of-tree build systems | One more translation unit |
| Automation that scrapes shell or console text | Many verbs print different text. Re-check it |

Home Assistant YAML needs no change. The first ESPHome build after upgrading
is a full rebuild.

**Upgrading from `v2.0.0`:**

1. If the bus needs a restore from a backup you hold, run it **before**
   upgrading. This release cannot read that backup, whether it is in flash or
   exported to a file.
2. After flashing, run `backup save`.

**Staying on `v2.0.0`:**

- Keep `allow_commissioning` off wherever a browser is used on the same
  network. See *Security*.
- Do not use `address d<N> set`, a device-space `restore apply`, or `commission
  devices`. See *Fixed*.

### Security

- **The TCP shell no longer runs what a browser sends it.** A web page open in
  any browser on the network could POST to the shell port. 2323 is not on the
  browsers' blocked-port list, and a `text/plain` form or no-cors fetch needs no
  preflight. Every line of the request body then ran as a shell command,
  commissioning included where `allow_commissioning: true`, which
  `dali-starter.yaml` sets.
  - A connection that opens with an HTTP request line is now closed before
    anything in it runs.
  - The device logs a WARN that names the sender's address.

  Every earlier release with the TCP shell has the hole. **Host-tested**: the
  classifier `dali_cli_peer_sniff()` has 7 vectors, the attack's full shape
  among them, and 6 mutants killed. The binding is unverified on a device.

### Breaking — operator-visible

- **Backups taken before this release are lost.**
  - A backup stored in flash by an older build no longer loads, because the
    stored record changed size. It reads as no backup at all.
  - An exported version-1 file is refused by `backup import`, which says what
    it is.
  - Run `backup save` after flashing. **Host-tested.**
- **The device-space `address` arms need the device to answer QUERY CONTENT
  DTR0 (`0x36`).** A device that does not makes `address d<N> set|clear` and
  any device-space `restore apply` move refuse with `DTR0 could not be
  checked`. No device here has yet been seen to answer it.

### Breaking — C API

- **`DaliError` gains `DALI_ERR_WAIT_EXPIRED` (13), appended.** It means a
  blocking caller stopped waiting, and nothing is known about the bus. The
  shell's and the scan task's blocking transports return it where they
  returned `DALI_ERR_TIMEOUT`. Treat it as unknown, never as absent.
- **`dali_phy_tx()`:**
  - It returns `DALI_ERR_TIMING`, not `DALI_ERR_TIMEOUT`, when a frame does not
    complete in time.
  - It reports success only once the ISR has reached `DALI_PHY_TX_DONE`.
- **The TX-end reference moved 1.664 ms earlier.**
  `dali_phy_get_last_tx_end_us()` and the scheduler's `get_last_tx_end_us`
  hook now report the end of the forward frame's last data bit. The constants
  follow:

  | Constant | Before | Now |
  |---|---|---|
  | `DALI_REPLY_WINDOW_OPEN_DECODED_US` | 2000 | 3664 |
  | `DALI_REPLY_WINDOW_CLOSE_US` | 27000 | 28664 |
  | `DALI_REPLY_WINDOW_OPEN_US` | 5500 | 5500, which now opens 1.664 ms earlier |

  The first two are the same instants measured from the new reference.
  `DALI_TX_STOP_BITS_US` is new.
- **`DALI_DEVICE_INITIALISE_UNADDRESSED_PARAM` is `0x7F`**, not `0x00`. `0x00`
  selects the device at d0.
- **`DaliRestoreMove` changes layout.** `kind` moves ahead of `from` and `to`;
  `has_identification` and `identification[8]` are appended. Positional
  initializers break. There are none in-tree.
- **`DaliRestoreMoveCheck` gains `DALI_RESTORE_MOVE_UNIT_MISSING`, appended.**
  `DALI_RESTORE_MOVE_TARGET_SILENT` now also means the source still answers.
  `dali_restore_confirm_move()` sends one more query, the probe of `from`, when
  `to` is silent. A caller switching on the result must handle the new value.
- **`DaliRestoreConflictKind` gains `DALI_RESTORE_CONFLICT_CONTESTED`,
  appended.** A plan on a bus with a contested address is no longer clean, even
  when nothing wants that address.
- **The snapshot format is version 2.**
  - `DALI_SNAPSHOT_FORMAT_VERSION` is 2.
  - `DALI_SNAPSHOT_BLOB_MAX` grows by 768 bytes, to 3,208.
  - `DaliSnapshot` gains `instance_count` and `instances[]`.
  - `dali_snapshot_from_inventory()` and
    `dali_commissioning_occupancy_from_inventory()` treat an identity-collided
    gear address as contested.
- **Removed: `dali_cmd_instance_group()`.** It put the group in the address
  byte, which made it a device-group command and not an instance-group one. It
  also built that byte with the 16-group gear layout, so device group 16
  became group 0. Nothing called it. `dali_build_instance_command()` with an
  instance-group selector replaces it.
- **`DaliDispatchKey` gains `group_kind`, `match_instance_type` and
  `instance_type`, appended.** Zero values keep the old matching. A positional
  initializer that stops at `instance` now fails
  `-Wmissing-field-initializers`.
- **`DaliInputInstanceInfo`:**
  - It gains `has_event_scheme`/`event_scheme`,
    `has_event_priority`/`event_priority` and
    `has_instance_groups`/`instance_groups[3]`.
  - Its `role`, `role_source` and `usable` fields are now `uint8_t` holding the
    enum values. C callers are unaffected. C++ callers that pass them to a
    function taking the enum need a cast.
- **Struct growth.** `DaliDiscoveryDeviceInfo` gains `has_identity_collision`,
  and `DaliDiscoveryInventory` gains `identity_collision_count`. Anything that
  compiled a struct size in must be rebuilt as a unit.
- **Shell hooks:**
  - `restore apply` no longer calls `DaliShellHooks.config_applied` or
    `inventory_changed`.
  - It calls `short_address_moved` once per confirmed gear move, in plan
    order.
  - `DaliShellHooks` gains `instance_config_applied`, appended.
- **The scheduler routes unsolicited event frames to subscribers in every
  state.** `rx_event_unroutable` stays in `dali_stats_t`, but is no longer
  incremented. `dali_sched_is_quiescent()` no longer reports quiescent between
  dequeuing a transaction and transmitting it.
- **ESPHome C++:**
  - `DaliComponent::add_dispatch_entry()` takes `group_kind` and
    `instance_type`, both defaulted.
  - `on_instance_config_applied()` is new.

### Breaking — build

- **One new translation unit:** `components/dali/dali_event_source.c`. An
  out-of-tree build that lists sources by hand must add it, or fail to link.
  In-repo builds are covered: `components/dali/CMakeLists.txt` lists it,
  `esphome/components/dali/proto_dali_event_source.c` is its shim, and CI
  fails if the lists disagree.
- **The ESPHome component sets `CONFIG_GPTIMER_ISR_CACHE_SAFE` and
  `CONFIG_GPIO_CTRL_FUNC_IN_IRAM`,** so the first build after upgrading is a
  full rebuild.
- **A config with a `shell:` block needs two more lwIP sockets.**
  `CONFIG_LWIP_MAX_SOCKETS` rises by 2, unless the YAML sets it. In that case
  ESPHome warns if it is short.

### Added

- **Instance settings in the backup, and `restore instances [apply]`.**
  - `backup save` records each control device's instance settings: type,
    enabled, event scheme, priority, filter and three instance groups, up to 64
    instances.
  - It reports how many it recorded, left out over the limit, or could not
    read. That costs about eleven queries per instance.
  - `restore instances` lists each differing field as `now -> recorded`.
    `apply` writes the fields, ENABLE or DISABLE last, and reads each instance
    back.
  - `apply` needs the same policy as `restore groups apply`. Type-specific
    settings, such as Part 303 timers, are not recorded.

  **Host-tested.**
- **Sensors poll on events of every scheme.**
  - A sensor with `poll_on_event` is polled on any event that could be its own,
    not only a Device/Instance one. An inferred match logs `event poll
    requested (inferred): ...`.
  - The integration reads each sensor's source profile, five instance queries,
    after boot, after a scan, after any shell workflow, and after an `iconfig`
    to that sensor's device. It reads one at a time.
  - New log lines:
    - an instance off scheme 2: WARN when the sensor polls on events or a
      dispatch rule keys on its device, INFO otherwise
    - `event scheme changed from X to Y`
    - DEBUG when a profile read comes back incomplete

  **Host-tested** matcher; the ESPHome wiring is unverified.
- **Instance-group, instance-type and all-instances selectors.**
  - `iquery` and `iconfig` take `g<N>`, `t<N>` and `all` as the instance. A
    query through one prints `note: ... can reach more than one instance`.
  - The console usage line reads `inst 0-31|gN|tN|all`.
  - `instances` and `export inventory` show each instance's event scheme,
    event priority and instance groups. `discover` spends five more queries per
    input instance reading them.

  **Host-tested**, against TI's decoder and recall of the standard.
- **`headless_dispatch` keys on Part 103 groups and instance types.**
  - YAML accepts `address_kind: instance_group` and `device_group`, and
    `instance_type:`. `group` still matches either group space.
  - A typed rule keyed on a short address and an instance is refused, since no
    event carries all three. `export config` writes the new keys back.

  **Host-tested.**
- **Shared addresses that answer alike are detected.**
  - A gear address whose Bank 0 identity read collides twice running is marked
    contested, and stays listed.
  - `discover` and `scan` append `, contested` to it, and after `Scan complete`
    print `note: N listed address(es) hold more than one unit.`, the addresses,
    and the remedy.
  - `inventory` marks it `contested (identity collides)` and counts it.
  - The planner reserves it, `backup save` leaves it out, and the commissioning
    post-scan counts it contested rather than confirmed.
  - The ESPHome scan logs `N address(es) answer as one unit but hold more than
    one; their identification numbers collide`, then one `aN: contested,
    identity collides` line per address.

  **Host-tested.**
- **DTR0 is read back before every short-address write.**
  - Every `address` arm and every `restore apply` move loads DTR0, reads it
    back with QUERY CONTENT DTR0 (gear `0x98`, device `0x36`), and only then
    sends SET SHORT ADDRESS.
  - A wrong, silent or unreadable read-back gets one more load. If that fails
    too, nothing further is sent:
    - `DTR0 did not load: a7 read back 0x0B, not 0x05; nothing further sent`
    - `DTR0 could not be checked: ...`
  - A write that needed the second load prints `DTR0 needed a second load: a7
    first read back 0x0B`.
  - Clearing a contested address sends the pair unchecked when the read-back
    collides, and says so.

  **Host-tested**; one bus reading, a `clear` on 2k that loaded first time.
- **`restore apply` confirms every move.**
  - Each move is checked before the next is sent: the destination answers, the
    source is silent, and the identification number there is the unit's.
  - Lines read `OK` or `sent, not confirmed: <reason>`.
  - The run stops at the first move it cannot confirm. A clean run ends
    `restore apply: N move(s) applied, each confirmed on the bus`.

  **Hardware-verified**: gear and device moves on 2k, and on 1k a stop after a
  move that did not land.
- **`backup save` warns about unaddressed gear.** It sends one broadcast QUERY
  MISSING SHORT ADDRESS. When anything answers, it prints `backup: gear on the
  bus reports no short address and is NOT recorded here`, then `backup: run
  'commission unaddressed', then 'backup save' again, to include it`.
  **Hardware-verified** on 1k.
- **The TCP shell drops dead peers.**
  - A client that stops reading without closing is dropped once a write makes
    no progress for 10 s, and its workflow ends at the next step.
  - A peer that vanished while idle is reclaimed in about 45 s by TCP
    keepalive.
  - The shell's listener and client are registered with ESPHome's socket
    count.

  **Unverified** on a device.
- **New C API:**

  | Header | New |
  |---|---|
  | `dali_cli.h` | `DaliCliPeerVerdict`, `DaliCliPeerSniffer`, `DALI_CLI_HTTP_METHOD_MAX`, `dali_cli_peer_sniffer_init()`, `dali_cli_peer_sniff()`; `dali_cli_parse_instance_selector()`, `dali_cli_instance_selector_is_multi()`; `dali_cli_format_gear_version()`, `DALI_CLI_VERSION_TEXT_MAX`; `dali_cli_raw_frame_is_commissioning()` |
  | `dali_event_source.h` (new module) | `dali_event_source_match()`, `dali_event_source_scheme_differs()`, the profile read builder and parser |
  | Instance selectors | `DALI_INSTANCE_SELECTOR_MASK`, `DALI_INSTANCE_GROUP_SELECTOR`, `DALI_INSTANCE_TYPE_SELECTOR` |
  | `dali_snapshot.h` | `DALI_SNAPSHOT_MAX_INSTANCES`, `DALI_SNAPSHOT_INSTANCE_WIRE_SIZE`, `DaliSnapshotInstance`, `dali_snapshot_add_instance()` |
  | `dali_input_device.h` | `DaliInstanceSettings` |
  | `dali_restore.h`, instance settings | `dali_restore_read_instance_settings()`, `dali_restore_instance_write_mask()`, `dali_restore_locate_device()`, `dali_restore_write_instance_settings()`, `dali_restore_instance_field_name()`, the `DALI_RESTORE_INSTANCE_*` mask bits |
  | `dali_restore.h`, moves | `dali_restore_build_move_sequence()`, `dali_restore_write_short_address()`, `DaliRestoreWriteOutcome`, `DaliRestoreWriteResult`, `dali_restore_write_was_sent()`, `dali_restore_write_outcome_name()`, `dali_restore_confirm_move()`, `DaliRestoreMoveCheck`, `dali_restore_move_check_name()` |
  | Discovery | `dali_discovery_gear_address_contested()`, `dali_discovery_inventory_store_identity()` |
  | Transport | `dali_transport_transaction_timeout_ms()` |

### Changed — what verbs print

None of these renames or removes a verb.

**`address` and `restore apply`:**

- `address aN set aM` and `dN set dM` probe the source when the destination is
  silent: `... and aN still does -- the gear did not move`, or `neither aM nor
  aN answers after the write -- run 'scan' to find the gear`. An undecodable
  destination reads `whether aM answers after the write is unreadable (...)`.
- A transport error during an `address` write says whether SET SHORT ADDRESS
  had gone out, replacing `ERR <error> at sequence step N`:
  - `<error> before SET SHORT ADDRESS; nothing moved`
  - `<error> while SET SHORT ADDRESS went out; run 'scan' before sending
    anything else`

  `restore apply` makes the same distinction on the move line.
- `restore apply`'s `nothing answers at the target` now reads `nothing answers
  at the target, and the source still does`. A new reason, `nothing answers at
  the target or the source`, reports a unit that answers at neither end.

**`identify`:**

- It reads QUERY ACTUAL LEVEL before it blinks, and puts that level back
  afterwards: OFF for a lamp that was off, DAPC for any other level.
- It ends `identify: done, level N restored` or `identify: done, switched off
  again`, where it printed `identify: done` and left the lamp at min.
- A level it cannot read prints `identify: level before unreadable (<reason>);
  aN will be left at min` before the blink.
- The help text reads `blink one short-addressed lamp, then restore its
  level`.

**`discover`, `inventory` and `stats`:**

- `discover` and `inventory` print the gear version as major.minor (`v2.0`),
  where they printed the byte halved (`v4`). A byte below 2.0 prints raw, as
  `version=0x01`. `export inventory` and `smoke` still give the raw byte.
- `discover`'s event note counts every event that arrived during the walk, so
  it reads higher on the same bus. `stats`' `event unroutable` stays at 0.

**Backup and restore:**

- `backup save`'s contested block reads `N address(es) are contested and NOT
  recorded here`, where it read `answered undecodably`, and lists both kinds.
  A collided address is no longer recorded as an unanchored entry.
- `backup status` and `backup import` report the instance count, and `status`
  lists each instance under its device.
- An export is longer. A full blob is 3,208 bytes, which is 107 import lines.
- `restore plan` and `restore groups` report every contested address as a
  `contested` conflict, even with no move aimed at it.
  - The remedy line reads `free a contested address with ...`, where it read
    `free a contested target`.
  - A contested `d<N>` gets its own remedy line, `free a contested d<N> with
    'address <dN> clear', then 'commission devices', then run this again`,
    where it read `nothing here de-addresses a control device, so a contested
    d<N> target needs a hardware pass`.
- `backup save` names the same fix for a contested `d<N>`: `'address <dN>
  clear' frees the device ones for 'commission devices'`.

**Elsewhere:**

- `raw` and `raw2` of a commissioning frame need `allow_commissioning: true`
  on the TCP shell. Without it they print `raw (commissioning frame): refused
  by session policy`.
- `capture` and `trace` measure `since_tx_us` from the frame end. Add 1664 to
  compare a capture taken before this release.
- `wait expired` can appear where `timeout` did, when the shell stopped waiting
  on a busy bus.

### Changed — ESPHome

**Address changes:**

- After `restore apply`, the group map follows each confirmed gear move,
  rather than being rebuilt from the scan taken before the moves.
  - Both ends of each move drop their cached level profile, and a refresh is
    requested.
  - Each move logs `group membership followed the move` and an `aX -> aY`
    warning about YAML entities that still name the old address.
  - The per-move `short address changed ... stale until the next scan` line is
    gone. **Hardware-verified** on 1k.
- After `address aN clear`, the device log reads `aN cleared: no longer polled
  for group state (gX gY); the gear keeps its groups and still follows group
  and broadcast commands`, where it read `retired from every group it was
  known in`.
  - The warning after it reads `nothing answers at aN now. Any entity
    configured with address: N targets nothing until 'commission unaddressed'
    re-addresses the unit, at an address only a scan can find`.

**Other behaviour:**

- The Identify button restores the level the same way the shell does. It logs
  `back to level N`, `switched off again`, or `left at min` when the level was
  unreadable. It blinks the address it started on, even if Target Address
  changes mid-blink. **Unverified.**
- A shell workflow's bus claim waits up to 5 s for queued work to drain, and is
  refused if it does not, logging `queued traffic did not drain; claim
  refused`.
- The **DALI Command** text entity refuses a `raw` or `raw2` commissioning
  frame with `commissioning frame; use the native CLI`.
- `idle_timeout` now only reclaims a terminal left open and unused, and
  `idle_timeout: 0` no longer costs the shell to a dropped connection.

### Fixed

- **Both Part 103 addressing encodings.** **Hardware-verified** on 2k, all three
  writers.
  - `address d<N> set d<M>` loads device DTR0 with `M`, not `(M << 1) | 1`,
    and prints it so (`device DTR0=4` for d4). On `v2.0.0` it sent the device
    to 2M+1, or nowhere for M ≥ 32.
  - A device move in `restore apply` loads DTR0 raw too.
  - `commission devices` sends INITIALISE `0x7F`, not `0x00`. `0x00` selects
    the device at d0. On 2k it found neither of two unaddressed devices, and on
    a bus whose d0 is occupied it would have re-addressed that device.
- **The reply window was measured from after the stop bits.** That put the
  undecodable-activity edge 1.664 ms past the standard's minimum. It is now
  measured from the frame end. **Hardware-verified** for a decoded reply, in a
  1k capture. The undecodable edge is host-tested only.
- **`restore apply` did not confirm a move before the next one depended on it.**
  **Hardware-verified.**
- **A unit that missed the DTR0 load took whatever DTR0 last held.** It could
  land on top of another unit. Now prevented by the read-back under *Added*.
  **Host-tested.**
- **`restore apply` handed the integration the scan it planned from,** so group
  membership went stale after the moves. **Hardware-verified** on 1k.
- **Events that arrived while the scheduler was in `SCHED_TX` or `WAIT_SETTLE`
  were dropped,** including the inter-frame guard on an idle bus. They now
  reach their subscribers. **Host-tested.**
- **`raw` and `raw2` bypassed the commissioning policy.** **Host-tested.**
- **The TX bit clock was not cache-safe in ESPHome builds.** NVS commits could
  stall it mid-frame. Fixed by the sdkconfig options under *Breaking — build*.
  **Unverified** beyond the generated sdkconfig.
- **The TCP shell could read an occupied address as free.** Refresh and poll
  work queued ahead of a probe outlasted its fixed 200 ms wait. The expired
  wait reported `DALI_ERR_TIMEOUT`, which means silence. So `address` could
  write onto an occupied address, and a `commission` pre-scan could offer it as
  free. Three things changed:
  - A claim now drains the queue first.
  - An expired wait reports `DALI_ERR_WAIT_EXPIRED`.
  - Single-frame waits are sized from the retry budget.

  **Unverified** on a device.
- **`identify` left the lamp at min.** **Hardware-verified** from a mid level on
  1k. From max, from off, and on the ESPHome button it is unverified.
- **The gear version printed as the byte halved.** **Host-tested.**
- **The ESPHome log called a cleared lamp "retired from every group".** The
  gear keeps its groups; only the integration's polling changed.
- **A sensor whose instance was not on event scheme 2 polled only on its
  interval.** Its events matched nothing. This is what slowed the 2k Steinel.
  **Host-tested.**
- **Two units that answered alike shared an address invisibly.** No scan
  flagged it. **Host-tested.**
- **The TCP shell had no send timeout and no keepalive.** A client that stopped
  reading could hold a workflow, and the bus, for as long as TCP took to give
  up. With `idle_timeout: 0`, a peer that vanished while idle held the shell
  until reboot. **Unverified** on a device.
- **The TCP shell's sockets were not counted** in ESPHome's lwIP socket pool.
- **`backup save` and the restore planners sent a contested `d<N>` to a hardware
  pass.** They said nothing here de-addresses a control device, which has not
  been true since `address d<N> clear` arrived in `v2.0.0`. Its contested arm
  opens on the same undecodable reply that marks the address contested, so
  they now name it. **Unverified:** that arm has not yet met a bus.
- **`tools/dali-shell` left out a node whose shell was in use.**
  - The cause: its probe waited for a busy notice the device never sends.
    While a session is open, a second connection waits in the device's listen
    backlog and gets nothing.
  - What it did: `--list`, and discovery without `--host`, dropped the node,
    and connecting to it ended in `no prompt after 10s; the device may still
    be working`.
  - Now: a connection the device accepts but does not greet reads as in use.
    The node is listed and offered as in use, and connecting to it says another
    session is probably open, and how long the device takes to reclaim one.

  Tested against local stand-ins for each case; **unverified** against a
  device.

### Verification

- **Host and CI.** 34 host suites, all passing. `dali_test.yaml` compiles on
  ESPHome 2026.9.0, and the native firmware builds on IDF 6.0.1.
- **`v2.0.0` features that have run on a bus since that tag:**
  - `commission devices`
  - `backup export` and `backup import`, in the version-1 format
  - `address <aN> clear`
  - `address <dN> set`, on the fixed encoding
  - the move-aside path in `restore plan`
- **Not yet on any bus:**
  - equal-random-address recovery
  - `restore groups`
  - the version-2 backup and `restore instances`
  - event-source matching
  - the instance selectors
  - the shared-address detection
  - DT6, DT8, memory writes, and every input-device configuration write but
    SET EVENT SCHEME
  - the TCP shell hardening

  `todo.md` has the procedure for each.

## v2.0.0

**This is a breaking release for C API consumers and for out-of-tree build
systems.** The operator-facing command surface is backward compatible — no verb
was renamed or removed — but several commands print more, or different, output
than they did in v1.3.0, so automation that scrapes shell or console text needs
re-checking. Home Assistant users upgrading from v1.3.0 have nothing to change
in YAML.

Upgrading from v1.2.0 or earlier: the console verb renames, the decoded reply
format, error names replacing error numbers, and `special randomize` →
`special randomise` all landed in **v1.3.0**, not here. Read that release's
notes as well.

### Breaking — C API

- **`dali_commissioning_verify_from_sequence()` and
  `dali_commissioning_verify_short_address()` changed signature.** Both took
  `bool *verified_out` and now take `DaliCommissioningVerifyOutcome
  *outcome_out`. VERIFY has three outcomes, not two: a unit that confirms, a
  unit that stays silent, and bus activity meaning two units answered at once.
  The bool could only report the first two, and equal-random-address detection
  depends on telling the third apart. This breaks at compile time, which is the
  intent.
- **`DaliCliCommandId` gained `DALI_CLI_CMD_ADDRESS` in the middle of the
  enum**, between `DALI_CLI_CMD_CONFIG_DTR0` and `DALI_CLI_CMD_MEMREAD`, so
  every enumerator after it takes a new numeric value and `DALI_CLI_CMD_COUNT`
  moves again. `DALI_CLI_CMD_BACKUP`, `_RESTORE` and `_DEVINFO` are appended
  before `COUNT`. Anything that persisted or transmitted a numeric CLI command
  id across this boundary will resolve it to the wrong verb; re-derive them.
- **Struct layouts grew.** `DaliCommissioningOptions` and
  `DaliDeviceCommissioningOptions` gain `quiesce_control_devices`; both result
  structs gain `quiescence_requested`, `quiescence_started`,
  `quiescence_release_attempted`, `quiescent_state_unknown` and
  `quiescence_error`. `DaliDiscoveryDeviceInfo` gains
  `has_undecodable_device_activity`, `has_device_identity` and
  `device_identity`; `DaliDiscoveryInventory` gains `undecodable_device_count`.
  `dali_stats_t` gains seven `rx_*` buckets. All are appended and inert when
  zero-initialized, so behaviour is unchanged for a caller that does not set
  them — but anything that compiled a struct size in, or that copies these
  across a boundary, must be rebuilt as a unit.
- **`DaliShellHooks` gains four optional members** — `short_address_moved`,
  `short_address_cleared`, `snapshot_save` and `snapshot_load` — appended, so
  designated initializers are unaffected and a hook left NULL is skipped.

### Breaking — build

Four new translation units. An out-of-tree build that lists sources by hand must
add all four or fail to link:

```
components/dali/dali_snapshot.c
components/dali/dali_restore.c
components/dali/dali_device_commissioning.c
components/dali/dali_commissioning_audit.c
```

In-repo builds are covered: `components/dali/CMakeLists.txt` carries them for
the native ESP-IDF route, each has its `esphome/components/dali/proto_dali_*.c`
shim for the ESPHome route, and CI fails if the two lists and the sources ever
disagree.

### Breaking — operator-visible output

No verb was renamed or removed. These change what existing verbs print:

- **Every operator-driven scan now silences control devices for the duration of
  the walk.** `scan`/`discover`, both commissioning pre-scans, the commissioning
  post-scan, `backup save`, and the scan behind `restore plan`/`apply` bracket
  themselves with broadcast `START`/`STOP QUIESCENT MODE`. On a bus with
  occupancy or lux sensors, those go quiet for the length of the walk — tens of
  seconds to minutes. The release is attempted on every exit path including
  cancellation and error; a release that fails prints a line naming
  `quiescent off all` as the fix. Two walks deliberately do **not** take the
  bracket: `find switches`, whose entire purpose is listening for events, and
  the integration's own unattended periodic scan, because silencing occupancy
  for minutes is a trade nobody is present to accept.
- **Commissioning runs the post-scan on failure.** A failed
  `commission unaddressed` now post-scans instead of returning at the error
  line, and `commission devices` gained a post-scan on both exits where it
  previously had none. Both print more than they used to. Two new report lines
  exist: `occupied, unrecorded` for an address a run wrote to and ended before
  recording, and a note that the post-scan ran read-only while the bus may still
  be in initialisation state.
- **The contested wording changed** from `contested - two gear answered as one`
  to `two units answered as one`, because the same text now serves the
  control-device space, where addresses print as `d<N>` rather than `a<N>`.
- **`special initialise`, `program-short` and `verify-short` print one or two
  extra lines** before the frame, decoding what the raw parameter resolves to
  (`special: 27 is the encoded form of a13`). The parameters themselves are
  unchanged and still raw bytes — `special` exists to put a literal frame on the
  bus — and the echo is not a gate: the frame goes out either way.
- **The scan's unattributed-RX note is now classified.** It read
  `N RX observation(s) fell outside active reply attribution` and advised
  inspecting timing; it now names each class separately, and control-device
  events are identified as expected on a bus with sensors rather than reported
  as a timing fault. `status` prints the seven-way breakdown beneath the
  aggregate.

### Added — backup and restore

New: an address backup that survives re-addressing, keyed to the physical unit
rather than to the address it currently answers on.

- `backup save` / `status` / `export` / `import` — records which physical unit,
  by its Bank 0 identification number, holds which short address. On ESPHome the
  backup persists to NVS (up to 2448 B) through the preferences API and survives
  reboots and OTA. **Hardware-verified** (`save`, `status`).
- `restore plan` / `restore apply` — turns a snapshot plus a live bus into an
  ordered move list, executed with plain addressed commands and no `INITIALISE`
  window. Breaks cycles with staging hops, and moves aside gear the backup has
  never seen rather than letting it block the plan. **Hardware-verified.**
- `restore groups` / `restore groups apply` — puts group membership back.
  Deliberately a separate verb that `restore apply` never reaches, because a
  backup predating a deliberate regrouping would silently undo it. Existing
  backups already carry the data, so no format change and no re-export.
  **Host-tested.**
- `backup import begin | <hex>… | end | abort` — reads an exported backup back
  in, which is how a backup lives on the native serial CLI, where there is no
  persistent store. **Host-tested.**
- `dali_snapshot_decode()` is now transactional: entries are validated in full
  before the first byte is written, so a corrupt entry no longer replaces a good
  snapshot with a truncated one.

`backup export`'s output format changed when `backup import` arrived: it printed
one long hex line, and now prints the `backup import` script that reproduces the
snapshot. Anything that captured the old single-line form and parsed it will not
match. No tagged release ever carried the old form — `backup` and `restore` are
both new here — so this affects only installations tracking `dev`.

Control gear only. Part 103 control devices have their own group scheme the scan
does not read, and scenes are not captured at all.

### Added — the `address` verb

`address` is a checked tier over re-addressing and group membership: every
argument is written the way a target is, and every result is read back off the
bus. It stands to `config`/`config-dtr0` as `commission` stands to the
addressing specials.

- `address <aN> set <aM>` probes the destination (refusing unless demonstrably
  empty) and the source, writes atomically, then confirms the destination
  answers and the source is silent. **Hardware-verified.**
- `address <aN> add <gN>` / `remove <gN>` send the group command and read both
  membership bytes back, because a group command is unacknowledged and a driver
  that ignored it is otherwise indistinguishable from one that took it.
  **Hardware-verified.**
- `address <aN> clear` de-addresses gear, backed by a broadcast QUERY MISSING
  SHORT ADDRESS. **Host-tested.**
- `address <dN> set <dM>` / `clear` are the Part 103 counterparts. The `d`
  prefix is mandatory — `5`, `a5` and `g5` are all refused — because the two
  address spaces are independent, and a bare number would leave `address 5 clear`
  and `address d5 clear` meaning different things with nothing on the line to
  say which. `address d5 add g3` is refused rather than sent: Part 103 device
  groups exist, but nothing in this stack reads them back, and this verb was
  built not to report success on an unacknowledged frame. `address d<N> clear`
  proves less than its gear counterpart and says so — Part 103 has no
  missing-address query here, so silence is the whole of the evidence.
  **Host-tested.**

`set` requires `allow_commissioning: true`, the same gate as the `config`
spelling; `add`/`remove` are ungated. A verified move calls the new
`short_address_moved` hook, so the integration moves its group-membership
bookkeeping with the gear and a re-address costs no rescan. An entity configured
in YAML against the old address still cannot follow, and is logged rather than
guessed at.

### Added — control-device commissioning

- `commission devices` — the Part 103 walk. It brackets its run with broadcast
  START/STOP QUIESCENT MODE. The bracket was refused when the module was
  written, on the argument that quiescence would silence the devices the walk is
  searching for. **The bus settled it**: `discover` under `quiescent on`
  enumerates control devices and their instances normally, so quiescence stops a
  device transmitting on its own initiative and does not gate replies to a query
  it was addressed with. With replies unaffected the trade runs the other way —
  a device walk asks roughly 25 COMPARE questions per device found, and one
  event landing in one COMPARE window sends the 24-bit binary search down a
  branch no device is on. **Host-tested**, no bus result.
- New module `dali_commissioning_audit.{c,h}` — the post-scan diff, lifted out
  of `dali_shell.c` so it is host-testable at all.
- Equal random addresses are detected at VERIFY, de-addressed, and left for a
  second run to place. **Host-tested**; the path has never run on a bus, because
  the units commissioned so far drew distinct randoms.

### Added — diagnostics

- **`rx_ignored_outside_reply` split into seven named buckets** —
  `rx_reply_early`, `rx_reply_late`, `rx_reply_superseded`,
  `rx_event_unroutable`, `rx_event_no_subscriber`, `rx_undecodable_ignored` and
  `rx_ignored_unclassified`. One number had been answering four unrelated
  questions, and three separate investigations read it as three different
  faults. Only the first two say anything about bus timing, and they point in
  opposite directions. The aggregate is kept, so nothing that already reads it
  breaks. Nothing about frame attribution or acceptance changed.
- `devinfo` — Part 103 device identification.
- `dali_discovery_scan_ex()` takes options and fills a result;
  `dali_discovery_scan()` is unchanged and calls it with none, so every existing
  call site keeps its behaviour exactly.

### Changed — ESPHome

- **The GPIO16/17 ban is lifted.** `tx_pin: 16` was rejected outright; the pins
  are ordinary on WROOM and the S3 puts PSRAM elsewhere, so the schema no longer
  refuses them. They remain unusable on **WROVER-E**, where they are wired to
  the PSRAM die — that is now a documented wiring caveat and your
  responsibility, not a build error.
- The pin schema validates against the target's real capabilities, so an
  input-only pin (GPIO34-39 on the classic ESP32) named as `tx_pin` is rejected
  where it previously was not. Full pin schemas (with `mode:` or `inverted:`)
  are still refused — these options take a bare pin number.
- Builds against newer ESPHome, which no longer compiles every IDF driver
  component by default: the component now requests `esp_driver_gptimer`
  explicitly, falling back cleanly on older ESPHome.

### Fixed

- `restore` treated a contested address as free, and could plan a move into an
  address two units were already answering on.
- `DALI_RESTORE_MAX_MOVES` was 132 against a true worst case of 188. It failed
  closed — a truncated plan sets `incomplete` and is refused — but the
  documented invariant that a plan is never truncated did not hold.
- A query meeting `MALFORMED` or `OVERFLOW` inside its reply window re-sends
  once if it holds a retry budget, instead of failing on the first blip.
  Retry-safe commands only, so nothing repeats that could not already repeat on
  a timeout — but a noisy bus will show more `tx_retries` and fewer aborted
  sequences than before.
- `since_tx_us` underflow.
- The ESPHome component resolves its vendored C sources from a single known path
  rather than guessing between candidates, so a packaging layout change fails
  loudly instead of silently shipping a partial protocol stack.

### Verification

31 host suites, all passing. CI builds the ESPHome component, the native ESP-IDF
firmware, and — on every `v*` tag — a clean-room consumer project that fetches
the tag over HTTPS with no checkout around it.

**Implemented does not mean verified on hardware.** A real bus cleared gear
commissioning, the backup/restore core, the `address` gear arms, and the
contested classification, including a genuine physical two-unit collision. Not
yet on any bus: `commission devices`, `backup import`/`export`, `restore
groups`, equal-random-address recovery, DT6/DT8, memory writes, and input-device
configuration. [dali_capability_matrix.md](dali_capability_matrix.md) carries
the per-capability breakdown. This is not a conformance claim, and the project
is not DALI Alliance certified.
