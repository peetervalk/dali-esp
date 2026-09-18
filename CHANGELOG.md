# Changelog

Notable changes per release. Verification status is stated per item, using the
same three labels as the rest of this repository: **hardware-verified** (a real
bus produced the result), **host-tested** (host vectors only, no bus), and
**unverified**.

Dated evidence for every claim here is in [project_log.md](project_log.md).

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
