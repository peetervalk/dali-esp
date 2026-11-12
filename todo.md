# DALI-ESP To-Do

Everything still open, in three parts:

- **Verify**: what `current_status.md` describes but no real bus has confirmed
  yet. Each item gives the procedure that confirms it.
- **Develop**: known defects to fix, features to build, decisions to make, and
  the release process.
- **Design**: proposals not yet approved. Each records what was weighed and
  the intended implementation.

`current_status.md` describes what the code is built to do, without
verification labels. This file is where "not yet confirmed" lives.
`dali_capability_matrix.md` keeps the per-capability record, and
`project_log.md` keeps the dated evidence.

**When an item is done**, record the session in `project_log.md`: date, ref,
ESPHome version, bus, what ran, the results, and what was not covered. Then
update the matrix row and delete the item here, or cut it down to what
remains. Do not mark an item done in place. If the item backs a claim in
`CHANGELOG.md`'s Unreleased section, update that claim's label too. A claim in
a released section stays as it shipped: record the result under *Unreleased*,
*Verification*, instead.

---

# Part 1 — Verify

## Before any bus session

**The buses, as last recorded in `project_log.md`:**

- **1k**: 16 LED gear at a0–a15, in groups 0 and 2–7; a2 is in g6, a5 in g0.
  No DALI-2 control devices. Four units reply early, 5.8–7.5 ms after the frame
  ends: the LED-strip drivers at a1, a13 and a15, and the corridor fixture at
  a0. That is conformant, but close to the edge.
- **2k**: 5 LED gear at a0–a4, one product, all in g0. d0 is the Steinel
  HF 360 II, with 4 instances: 0 lux, 1 occupancy, 2 temperature, 3 humidity.
  Only 0 and 1 emit events. The Casambi CBU-DCS that sat at d1 has been
  removed, and nothing answered there on 2026-11-11. A spare LED driver, the
  one that contested a4 on 2026-09-03, is normally disconnected.

**Every session:**

1. Flash a `dev` build and write down `git rev-parse --short HEAD` and the
   ESPHome version. Several log entries could not say which ref ran.
2. Set the logger to DEBUG. Keep the device log beside the shell transcript.
3. Before anything that writes an address, a group or an instance setting, run
   `backup save`, then `backup export > <file>`. A version-1 backup does not
   load on `dev`, so take a new one after flashing.
4. Address writes need `allow_commissioning: true` on the node.
   `dali-starter.yaml` sets it.
5. Drive the session from `tools/dali-shell`. On ESPHome, the device log is the
   only record of the integration's side.

---

## No bus needed, or read-only

### TCP shell hardening

These can be checked on any node with a `shell:` block, and need nothing on
the DALI wire.

**HTTP refusal, from a browser.** Open the devtools console on a page served
over plain http on the LAN, such as Home Assistant's own UI, and run:
`fetch("http://<host>:2323/", {method: "POST", mode: "no-cors", body: "help\n"})`.
The fetch reports a network error; that is expected. The device log should
show, at WARN, `Closed a connection from <address> that opened with an HTTP
request; nothing it sent was run`, naming the browser's machine.

**Keepalive**

1. Connect from a laptop on Wi-Fi with `python3 dali-shell` and leave it at the
   prompt.
2. Turn the laptop's Wi-Fi off without closing the client.
3. **Expected:** `Shell session closed` in the device log about 45 s after the
   last traffic: 30 s idle, then 3 probes 5 s apart. With the starter's
   `idle_timeout: 10min`, keepalive is what closed the session.
4. Connect from another machine. The banner should come at once.

**Opening an interactive session, soon after boot**

At priority 4, three interactive connections in four reset 2k through the task
watchdog, most often soon after boot. With the task at priority 1, `schema` in
chunks and output gathered, repeated connections on a node that had been up a
while no longer dip the heap and get the prompt at once. Connections in the
first 30 s after boot were not recorded. See `project_log.md`, 2026-11-09 and
2026-11-10.

1. Flash the node. Wait for `Boot seems successful` in the log, about a minute
   after boot, so that a crash cannot roll the image back.
2. Reboot it. Within 30 s of boot, open an interactive `dali-shell` session.
   That runs `schema`. Close it, and repeat five times.
3. **Expected:** the node never restarts. ESPHome prints `CRASH DETECTED ON
   PREVIOUS BOOT` on the boot after a crash, and the `Reset reason` sensor
   names it.
4. Run `discover` and `trace on` in the same session. Each `discover` line
   should appear as it completes, as before the binding gathered its output.
   The walk's trace lines should arrive during it, one bus wait behind, as
   they did on 2026-11-11; see `project_log.md`.

**The TCP binding after the `select()` rewrite**

The TCP binding no longer blocks in `accept()` and `recv()`; it waits in one
`select()` with the web listener. Nothing a client sees should differ. The
two checks above were written for the old loop, so run them on a build with
`web:`: *Keepalive* and *Opening an interactive session, soon after boot*.
The send timeout and a second client passed on 1k on 2026-11-11; see
`project_log.md`.

### The web shell

Needs a node with a `web:` block under `shell:`; both `_local` configs have
one. What is left needs a browser. Below, `<host>` is the node's name.

**Help under a refusing policy.** On a node with `allow_commissioning: false`,
`commission` is marked refused, nine `special` names are struck through, and a
note at the top says why.

**Leaving, in other browsers.** Close the tab while `discover` runs on 1k,
and note when the device log shows `Shell session closed`. Chrome drops the
connection, so the walk ends at its next write; see `project_log.md`,
2026-11-11. A browser that sends a close frame and waits lets the walk run
to its end. Record Firefox and a phone's browser.

**On the access point.** With the node's Wi-Fi unreachable so that its AP comes
up, join it from a phone and open `http://192.168.4.1:8080/`. **Expected:** the
page works, at phone width, with nothing fetched from the internet.

**`export config` with `allowed_hosts`.** On a node that lists names under
`web: allowed_hosts:`, `export config` prints them there.

**The 503 below the heap floor.** Normal use does not reach it: both nodes keep
70 kB or more free. On a test build with `WEB_HEAP_FLOOR_BYTES` in
`dali_shell_web.cpp` raised above the node's free heap, both the page and an
upgrade should get `503`, with a WARN that gives the free heap and the floor.

### ESPHome version range

- **Establish the `manifest.json` floor of `>=2026.6.0`.** Compile
  `dali_test.yaml` on 2026.6.0 with `uvx --from esphome==2026.6.0 esphome
  compile dali_test.yaml`, which leaves the pinned `.venv` alone. If it fails,
  raise the floor to the oldest version that compiles, and record which one.

### `export config` validates

1. On a node that has run `discover`:
   `python3 dali-shell export config > block.yaml`.
2. Paste the block into a copy of that node's YAML, replacing its `dali:`,
   `light:` and `sensor:` blocks.
3. Run `uv run esphome config <copy>.yaml`.
4. **Expected:** valid. Any error is a bug in the export. If this passes
   repeatedly, it is the vector *Host vectors for the unasserted shell
   outputs* wants.

### The GPIO ISR-service conflict

This is a defect read from ESPHome 2026.9.0 source and not reproduced.
Reproduce it before fixing it.

1. Flash a test node with the `dali:` block and, declared after it, a
   `binary_sensor: - platform: gpio` on a spare pin with `mode: INPUT_PULLUP`.
2. Short the pin to GND and release it a few times.
3. **Expected if the defect is real:** an error from the pin's interrupt
   attach in the boot log, and a binary sensor that never changes.
4. Add `use_interrupt: false` to the sensor. It should now follow the pin.

### A Home Assistant scan holds the bus against the shell

Read-only: the scan only queries. Either node.

1. Open a shell session. Press the scan button in Home Assistant.
2. While the scan log runs, type `query a0 status`, then `identify a0`.
3. **Expected:** both refused with `busy`, and no frame of theirs in the scan
   log. Once `DALI Scan Complete` is logged, the same lines run.
4. Run `discover` in the shell, and press the scan button during it.
   **Expected:** the button scan is refused (`Scan already in progress`), and
   `discover` completes: the shell's own claim does not refuse its frames.

### `restore` after a reboot reads the stored backup

`restore` now reads the backup from a copy `setup()` loaded, never from the
preferences API on the shell task. Read-only.

1. On a node with a stored backup, reboot, open a session, and run
   `restore plan`.
2. **Expected:** a plan from the stored backup, not `no backup`. Then
   `backup save` and an immediate `restore plan` plan from the new one.

---

## 2k sessions (control devices)

### Spacing from received frames

Our next frame now waits 9.6 ms after a reply to our query, from its last edge,
and 15.1 ms after any other frame. The PHY declines while a frame is still
arriving; see *Bus Timing* in `dali_protocol.md`. On 2k, the Steinel's events
are the frames that used to be followed after 1.7 ms. Host-tested only.

1. `stats` and note `TX retries`, `Reply timeouts` and `TX deferred (RX)`.
2. With the Steinel emitting (walk under it), `capture start`, then
   `query a0 status` ten times, then `capture export`.
3. **Expected:** from each `rx` to the next `tx`, `timestamp_us` differs by at
   least 23,752 after a reply and 29,244 after an event, for a 16-bit `tx`.
   That is the gap plus 17 bit periods; see *Reading a `capture export`* in
   `dali_commands.md`. Before the change these read about 17,000 and 15,900.
4. Run `discover` three times, without `quiescent on all`, and `stats` after
   each. **Expected:** every lamp found each time, `TX deferred (RX)` above
   zero, and fewer retries per walk than *Why the retry backoff works*
   recorded. Note the walk's duration for `current_status.md`, which gives the
   pre-spacing figure.
5. On 1k, repeat the `restore apply` that lost a frame after another unit's
   reply, if a swap is due anyway, and count `DTR0 needed a second load`
   lines; see *Bus timing with a logic analyzer*, step 3.

### Headless dispatch acts on transmission

A transmitting rule now moves its toggle and its lights only once the frame
has gone out. Needs a node with a rule other than `observe`, such as a toggle
on a Steinel event or a push button. Host-tested only.

1. Trigger the rule. **Expected:** the log shows `dispatch queued ... -> gN`,
   then `dispatch sent -> gN ...`, and the light entity follows the second.
2. Trigger it twice in quick succession. **Expected:** on, then off, as before.
3. The failure path, a frame that is not sent, cannot be provoked on a working
   bus without disturbing it. It stays host-tested.

### A failed instance enumeration is reported

Seen on hardware before: a device line claiming N instances, then nothing.
Now `dN: instance enumeration failed (<error>)` follows instead, naming
`instances N` as the retry. It cannot be provoked on demand. Watch for the line
in any `discover` with the Steinel emitting, and check that `instances 0` then
enumerates.

### DTR0 read-back before every short-address write

The device-space `address` arms and device moves in `restore apply` depend on
control devices answering Part 103 QUERY CONTENT DTR0 (`0x36`). No device here
has been seen to do so. Do this item before any other device-space write.

1. Run `dtrcheck 0 0 5` and `dtrcheck 1 0 5`. Each should read `5 (0x05)`.
   A silent device makes every device arm refuse with `DTR0 could not be
   checked`. If that happens, stop the device-space steps and record it.
2. Run `address d1 set d2`. Expected: `d1 -> d2 (device DTR0=2)` and `d2
   confirmed, d1 silent`. Then run `address d2 set d1`.
3. Swap two gear through a spare address:
   - `address a0 set a5`
   - `address a4 set a0`
   - `address a5 set a4`
   - `restore plan`: expect 3 moves, the first staged through a5.
   - `restore apply`: expect every move `OK`, in one apply.
4. Count every `DTR0 needed a second load` line in steps 2–3. Each one is a
   load the unit missed, which measures *Space forward frames from received
   frames* directly. Record the count.

### Event-source matching off scheme 2

1. Run `iconfig 0 1 set-event-scheme 0`, then `iquery 0 1 event-scheme`.
   Expect 0.
2. Within one occupancy heartbeat (30 s), the device log should show `event
   scheme changed from device-instance to instance`, and a WARN that the
   sensor's instance is not on scheme 2.
3. Each `type=3 inst=1` event should then be followed by `event poll requested
   (inferred): ...`.
4. Walk in front of the sensor. The occupancy entity should change as fast as
   it does on scheme 2, within a second or two.
5. Run `iconfig 0 1 set-event-scheme 2`, then `iquery 0 1 event-scheme`.
   Expect 2. The log should show the change back, and polls should read
   `event poll requested: addr=0 inst=1 ...` again, without `(inferred)`.

Not observable: the poll's result. The component logs none, and the occupancy
entity's state line does not appear in the log.

### What set the Steinel to event scheme 0

*The Steinel's events lost their device address* in `project_log.md` has the
background. Run this after the DTR0 item has passed.

1. Run `iquery 0 0 event-scheme` and `iquery 0 1 event-scheme`. Expect 2 and 2.
2. Run `address d0 set d2`, then `iquery 2 1 event-scheme`. Run `address d2 set
   d0`, then `iquery 0 1 event-scheme`.
3. Run `address d0 clear`, then `commission devices`. The Steinel returns at
   the lowest free device address. Read its scheme there, then put it back
   with `restore plan` and `restore apply`.
4. Switch the DALI supply off for 10 s and on again. Read the scheme once more.

**Reading the result:**

- If the clear or the re-commission sets the scheme to 0, then every device
  clear and re-commission changes that unit's event addressing. Say so in
  `commissioning_readme.md` next to `restore instances`.
- If nothing changes it, the cause stays open.

### Instance settings in the backup (format 2)

1. Run `backup save`. Expect 7 entries and 5 instance settings: four on d0 and
   one on d1.
2. Run `backup status`. It should list the instances under their devices.
   Instances 2 and 3 should record as disabled. That tests the rule that
   records "disabled" only when QUERY INSTANCE STATUS agrees with a silent
   QUERY INSTANCE ENABLED.
3. Run `iconfig 0 1 set-event-scheme 0`.
4. Run `restore instances`. It should list exactly one field, d0 instance 1's
   event scheme, as `0 -> 2`.
5. Run `restore instances apply`. It should end `OK`. Then `iquery 0 1
   event-scheme` should read 2.
6. Wait more than 60 s for `flash_write_interval`, then reboot the node. `backup
   status` should read `loaded from storage`, with the same instance count.
7. Run `backup export`. Paste the script back through `backup import`. It
   should report the same entry and instance counts.

### Instance selectors, and the instance fields discovery reads

The selector encodings come from TI's device decoder and from recall of the
standard. This run is their first independent check.

1. Run `instances 0`. Each instance line should show its event scheme,
   priority and instance groups. Each value should match `iquery 0 <i>
   event-scheme`, `event-priority`, `primary-group`, `group1` and `group2`.
2. Type selector:
   - `iquery 0 t3 type`: expect 3. Only the occupancy instance has type 3.
   - `iquery 0 t4 type`: expect 4.
   - `iquery 0 t0 type`: two instances should answer. Expect a collided or
     unreadable reply and the note `... can reach more than one instance`.
3. Group selector:
   - `iquery 0 1 primary-group`: note the value.
   - `iconfig 0 1 set-primary-group 5`.
   - `iquery 0 g5 type`: expect 3.
   - Restore the noted value. 255 clears the group.
4. `iquery 0 all type`: four instances answer, so expect a collision.

A reply from the wrong instance means the selector byte is wrong.

### Input-device configuration writes, per parameter

SET EVENT SCHEME is confirmed. The rest have never been written on a bus. Use
the same five steps for every parameter:

1. `iquery` the original value.
2. `iconfig` the test value.
3. `iquery` it back. **Expected:** it reads the test value.
4. `iconfig` the original value.
5. `iquery` it back. **Expected:** it reads the original value.

| Device / instance | Parameter (`iquery` name → `iconfig` name) | Test value |
|---|---|---|
| d0 / 2 | `enabled` → `enable` / `disable` | the opposite of what it reads |
| d0 / 1 | `event-priority` → `set-event-priority` | 3, where it reads 4 |
| d0 / 1 | `primary-group`, `group1`, `group2` → `set-primary-group`, `set-group1`, `set-group2` | 7 |
| d0 / 1 | `event-filter0` → `set-event-filter <v0> 0 0` | 3, where it reads 7 |
| d0 / 1 | `occ-hold-timer` → `occ-set-hold-timer` | 2 |
| d0 / 1 | `occ-report-timer` → `occ-set-report-timer` | 20, where it reads 30 |
| d0 / 1 | `occ-deadtime` → `occ-set-deadtime` | 12, where it reads 10 |
| d0 / 1 | `occ-detection-range`, `occ-sensitivity` | 50. `occ-capabilities` reads 0, so expect MASK to stay |
| d0 / 0 | `light-report-timer`, `light-hysteresis`, `light-deadtime`, `light-hysteresis-min` → their `light-set-*` | one step from the original |
| d1 / 0 | `pb-*` timers, only if `iquery 1 0 type` reads 1 | at or above `pb-short-timer-min` / `pb-double-timer-min` |

- `occ-catch-movement` has no setting to read back. Run it, then `iquery 0 1
  occ-catching` should read yes.
- Do not run `set-instance-type` or `set-instance-config` on the Steinel.
  Nothing here documents a writable type or configuration index for it.
- The occupancy timers change how the corridor behaves. Run this when that
  does not matter, and put every original value back before leaving.

### Part 103 memory write, read back (Steinel Bank 2)

1. Run `devmem read 0 2 0`. Expect 13 (`0x0D`), the last accessible offset on
   type 107. Any other value means the DTRs are not landing; stop and see
   `steinel_bank2_reference.md`.
2. Run `devmem read 0 2 5` and note the global detection range.
3. Run `devmem write 0 2 5 <original - 1>`, then `devmem read 0 2 5`. It should
   read the new value.
4. Run `devmem write 0 2 5 <original>`, then `devmem read 0 2 5`. It should
   read the original.
5. Run `smoke 0`. Its read/write/read-back should pass.

Wait about 1 s after each write before cutting bus power; the NVM commit is
asynchronous.

Part 102 has a read only: compare `memread a0 0 3 8`, the GTIN and the start
of the identification number, with what `meminfo a0` prints.

### The quiescence bracket releases on every exit

QUERY QUIESCENT MODE is not implemented, so the Steinel's lux heartbeat is the
only read-back. The device log shows `rx a0 inst=0 evt=1` every 3 s.

1. **`discover`.** The heartbeat should stop for the length of the walk, about
   29 s, and resume within about 3 s of `Scan complete`. There should be no
   `quiescent off all` line in the shell output.
2. **Cancelled walk.** Start `discover`, then press Ctrl-C after about 5 s,
   which drops the connection. The heartbeat should resume within a few
   seconds of the walk stopping.
3. **Commissioning.** Run `commission unaddressed` and `commission devices`
   with nothing unaddressed on the bus. The heartbeat should stop, then resume
   after each run.
4. **By hand.** `quiescent on all` should stop the heartbeat, and `quiescent
   off all` should bring it back.

**If the heartbeat does not resume:** run `quiescent off all` and record which
exit path failed to release.

### `address d<N> clear` at a shared device address: capture the collision

On 2k, two devices colliding at d0 read as silence, so the contested arm never
opened. This item settles why before anything changes the scheduler; see
*Clearing a shared device address* under *Fix*.

1. Make sure `backup save` and `backup export` hold the current layout.
2. Put d1 onto d0 by hand:
   - `raw C13000 len=24`: device DTR0 = 0.
   - `raw2 03FE14 len=24`: SET SHORT ADDRESS at d1.
3. Run `capture start`, then `address d0 clear`, then `capture export`.
   - **If the collision reads as silence,** as it did on 2k before: `address d0
     clear` answers `d0 does not answer; nothing to clear`. Carry on with the
     steps below.
   - **If it reads as undecodable activity,** this is the first bus run of the
     contested arm, which `backup save` and `restore plan` now name as the fix.
     The verb should print `d0 answers undecodably (...) -- control devices
     sharing one short address is the expected cause`, clear both devices, and
     end `run 'commission devices' to give them distinct addresses`. Then skip
     to `commission devices` in step 6.
4. Run `raw 01FE35 len=24 wait` three times, with a capture around them.
5. Read the export:
   - Does the merged reply to QUERY NUMBER OF INSTANCES start outside
     5500–28664 µs of `since_tx_us`?
   - Or does it decode as malformed, so that a retry's silence replaces it?
6. Clear both by hand:
   - `raw C130FF len=24`
   - `raw2 01FE14 len=24`
   - `commission devices`: it should find 2.
   - `restore plan` and `restore apply` put both back.

### Dispatch on instance group, device group and instance type

1. Add two rules to a test build of the 2k config. Use an output you can see,
   such as g0, and put it back afterwards with `off g0`:

   ```yaml
   - { frame_kind: input_24bit, address_kind: instance_group, address: 3,
       instance_type: 3, action: recall_max, output_type: group, output_address: 0 }
   - { frame_kind: input_24bit, address_kind: device_group, address: 1,
       instance_type: 3, action: recall_max, output_type: group, output_address: 0 }
   ```

2. **Instance group.**
   - `iconfig 0 1 set-primary-group 3`
   - `iconfig 0 1 set-event-scheme 4`
   - Walk past. g0 should go to max.
3. **Device group.** This uses raw frames; ADD TO DEVICE GROUPS (`0x19`) comes
   from TI alone.
   - `off g0`
   - `raw C13102 len=24`: DTR1 = `0x02`, which is group 1.
   - `raw C13200 len=24`: DTR2 = 0.
   - `raw2 01FE19 len=24`
   - `raw 01FE41 len=24 wait`: QUERY DEVICE GROUPS 0-7. Expect `0x02`.
   - `iconfig 0 1 set-event-scheme 3`
   - Walk past. g0 should go to max.
4. **Restore.**
   - Load DTR1 and DTR2 as in step 3.
   - `raw2 01FE1B len=24`: REMOVE FROM DEVICE GROUPS.
   - `raw 01FE41 len=24 wait`: expect `0x00`.
   - Put the original primary group back.
   - `iconfig 0 1 set-event-scheme 2`
   - `off g0`

Under schemes 3 and 4 the occupancy sensor should also keep polling, by
inference.

---

## 1k sessions (16 gear, 7 groups)

### `identify` from max and from off, and the Identify button

1. Run `max a0`, then `identify 0`. It should end `identify: done, level 254
   restored`, and the lamp should be back at max.
2. Run `off a0`, then `identify 0`. It should end `identify: done, switched off
   again`, and the lamp should be off.
3. **ESPHome button.** Set Target Address to 0 and press Identify, once from a
   mid level and once from off.
   - The device log should read `back to level N` or `switched off again`.
   - The lamp should end where it started.
   - Change Target Address during the blink. It should still blink and restore
     the address it started on.

### A swap restores in one `restore apply`

The seventh session needed two applies, because a lost DTR0 load stopped the
first. With the read-back in place, one should do it.

1. Swap the lamps at a2 and a5 by hand:
   - `address a5 set a16`
   - `address a2 set a5`
   - `address a16 set a2`
2. Run `restore plan`. Expect 3 moves, staging through a16.
3. Run `restore apply`. Expect all three `OK` in one apply.
4. Count the `DTR0 needed a second load` lines, as in the 2k DTR0 item.
5. Each confirmed move should log `group membership followed the move`.

### A staging hop avoids a missing unit's recorded address

A spare is now chosen outside the addresses the backup recorded where it can
be, so a unit that was only unpowered does not come back to find another
sitting on its address. Host-tested only.

1. With a backup of all sixteen, disconnect a15's mains, and swap a2 and a5 as
   in the item above.
2. Run `restore plan`. **Expected:** a15 reported missing, and the staging hop
   to a16. The old planner took the lowest free address nothing wanted, a15.
3. Reconnect a15 before `restore apply`, and check `discover` finds all
   sixteen where the backup says.

### A coupler press during a walk

A status probe that meets another master's frame is now asked once more, so a
coupler press no longer ends a `discover`. Host-tested only.

1. Run `discover`, and press a coupler button every few seconds until it ends.
2. **Expected:** the walk completes with all sixteen, and the pressed lamps
   follow the coupler. A press inside a reply window costs one more status
   query at that address. The walk's own second query is not a scheduler retry,
   so `TX retries` does not count it; `capture` shows it.

### `restore groups`

1k's seven groups make this the bus for it. This is the one restore step that
can destroy something, so take `backup save` immediately before.

1. Run `query a2 groups-0-7` and note the value. a2 is in g6.
2. Run `config a2 remove-group 6` and `config a2 add-group 1`.
3. Run `restore groups`. It should show `a2 now g1 -> g6`, with every other
   unit already correct.
4. Run `restore groups apply`. It should read back with no `MISMATCH`. Then
   `query a2 groups-0-7` should match step 1.
5. Run `discover`. Membership should match the backup everywhere.

### DT6 verbs

Start where a wrong answer is visible rather than destructive.

1. Run `query a0 device-type`. Expect 6.
2. Run the commands below on a5, a ceiling fixture on a DT6 driver, and on a13,
   an LED-strip driver that replies early.
   - `dt6 <a> dimming-curve`: expect 0 or 1, matching the profile the
     integration uses for that light.
   - `dt6 <a> failure-status`: expect `0x00` on healthy gear. The shell
     prints each flag.
   - `dt6 <a> gear-type`, `features`, `operating-modes`, `version`,
     `fast-fade`, `min-fast-fade`: expect plausible replies, not timeouts.
3. Select a curve and read it back:
   - Note the curve from step 2.
   - `dt6 a0 select-curve 1`, then `dt6 a0 dimming-curve`: expect 1.
   - The device log should show a0's level profile being re-read.
   - Put the original curve back and read it again.
4. Run the same commands from the HA DALI Command entity on one address. Expect
   the same answers; `failure-status` comes back as the raw byte there.

### Gear commands no bus has run

Run these on a1 and compare with `query a1 actual`:

| Command | Expected |
|---|---|
| `level a1 100`, then `level a1 mask` | stays at 100 |
| `cont-up a1`, then `cont-down a1` after a second | fades up, then down, at the fade rate |
| `level a1 120`, `off a1`, `last a1` | back at 120 |
| `dapc-seq a1`, then `level a1 80` and `level a1 160` | both levels land |
| `config-dtr0 a1 set-scene 50 3`, then `scene a1 3` | level 50, and `query a1 scene-level 3` reads 50 |
| `raw 0369 len=16`: ADD TO GROUP 9, sent once | nothing: `query a1 groups-8-15` reads `0x00`. Configuration commands need the pair |
| `raw2 0369 len=16`: the same, sent twice | a1 joins g9: `query a1 groups-8-15` reads `0x02` |

Run `raw2 0379 len=16` (REMOVE FROM GROUP 9) and `config a1 remove-scene 3`
afterwards.

### The command console, verb by verb

The shared `dali_cli` migration and the console's dispatch have never run on a
bus, and the console's handlers have no host vectors. From the HA DALI Command
entity, send one line per row and read DALI Command Result:

| Line | Expected result |
|---|---|
| `query a0 actual` | decoded level |
| `status a0` | decoded status flags |
| `level a0 128`, then `off a0` | `OK` (queued), and the lamp follows |
| `dtr 0 5` | `OK` |
| `config a0 add-group 9`, then `config a0 remove-group 9` | `OK`, and `query a0 groups-8-15` shows the bit set, then cleared |
| `special program-short 5` | refused: `commissioning special; use the native CLI` |
| `raw 01FE35 len=24 wait` | decoded reply, or timeout on 1k, which has no control devices |
| `raw2 ...` carrying a commissioning frame | refused: `commissioning frame; use the native CLI` |
| `queue` | `d=0/16 hw=... ok=... full=0 busy=0` |
| `iquery 0 1 type`, on 2k | 3 |

A result that differs from the shell's answer to the same line is a console
bug.

---

## Shared-address bench

These need a unit put deliberately on an occupied address. Take a backup and
export it first. Each bench ends with `restore plan` and `restore apply`
putting every unit back.

### Bench A: two units in different states

One unit lit and one off, so their QUERY STATUS replies differ in the arc-on
bit and garble when they overlap. This covers:

- the undecodable-activity window edge at 5.5 ms from the frame end
- the contested arm of `address <aN> clear`
- the `contested` and `target contested` conflicts in `restore plan`
- a two-unit `commission unaddressed`

Run it on 2k. Then run it on 1k with two of the early LED-strip drivers, such
as a13 and a15. On 1k it also shows whether their collisions are now
attributed rather than discarded.

1. Note which lamp the backup places on a3; call it X. Move it aside with
   `address a3 set a10`, which should confirm.
2. Run `level a4 128` and `off a2`. The lamps at a4 and a2 are Y and Z.
3. Put Y and Z both on a3 with the raw spelling, since `address` refuses an
   occupied destination:
   - `config-dtr0 a4 set-short-address-dtr0 7`
   - `config-dtr0 a2 set-short-address-dtr0 7`
4. Run `capture start`, `status a3`, `capture export`.
   - Expected: an `rx-activity` error, not a timeout and not a decoded byte.
   - The capture should show the activity inside the window: `since_tx_us`
     after 5500 µs.
   - `stats` should show no rise in undecodable-ignored.
5. Run `discover`. Expected: four units listed, and `note: 1 address(es)
   answered undecodably ... a3: contested`.
6. Run `restore plan`. Expected:
   - the conflicts `gear a3: contested` and `gear a10: target contested (3)`
   - Y and Z reported `not on bus`
   - the remedy line `free a contested address with 'address <aN> clear', then
     'commission unaddressed', then run this again`
7. Run `address a3 clear`. Expected:
   - `a3 answers undecodably (rx-activity) -- gear sharing one short address
     is the expected cause`
   - the warning that no backup holds them
   - `a3 -> unaddressed (DTR0=255)`

   The missing-address result will probably read as one YES, because both
   units answer `0xFF` in step. If `more than one unit` appears, record it.
8. Run `commission unaddressed`. It should assign two addresses and confirm 2
   of 2 in its post-scan.
9. Run `restore plan` and `restore apply`. Every move should be `OK`, and
   `discover` should show the recorded layout.

### Bench B: two units alike

Two units of one product, both off, answer every status probe in step, so only
the identity read collides. This tests the shared-address detection, which
depends on that collided read arriving as RX activity.

1. Run `off a3` and `off a4`, then `config-dtr0 a4
   set-short-address-dtr0 7`.
2. Run `discover`. Expected:
   - a3 listed with `, contested`
   - after `Scan complete`: `note: 1 listed address(es) hold more than one
     unit.`, `a3: contested`, and the remedy

   If a3 lists as one plain unit, the collided identity read arrived as
   silence or as a malformed reply. Record which, using `capture` around
   `meminfo a3`.
3. Run `restore plan`. Expected: `gear a3: contested`.
4. **Last, because it overwrites the good backup:** run `backup export`, then
   `backup save`. Expected: `1 address(es) are contested and NOT recorded
   here` and `gear a3: contested`. Paste the export back through `backup
   import`.
5. Run `address a3 clear`, `commission unaddressed`, `restore plan` and
   `restore apply`.

### The post-scan audit: an address contested during the walk

The audit's own contested path has never met a bus. Its `newly contested,
never assigned` class can be staged with the spare driver.

1. The spare must hold an address that is in use. It held a4 on 2026-09-03.
   If it no longer does:
   - Connect it and find it with `discover`.
   - Put it on a4 with `config-dtr0 a<N> set-short-address-dtr0 9`.
   - Disconnect its supply.
2. Clear one lamp with `address a2 clear`, so the walk has something to
   assign.
3. Run `commission unaddressed`. While it walks, power the spare driver.
4. Expected in the post-scan: a4 listed among addresses that became contested
   without being assigned, separately from the run's own assignments.
5. Clean up with `address a4 clear`, `commission unaddressed`, `restore plan`
   and `restore apply`. Disconnect the spare.

---

## Needs hardware or tooling not on site

### Equal random addresses

This is the largest untested slice of commissioning. Two units drawing the
same 24-bit random address cannot be staged with real gear: no Part 102
command writes a random address, only RANDOMISE draws one. It needs emulated
gear.

1. Build the control-gear firmware from TI's MSPM0 SDK 2.11
   (`source/ti/dali/dali_102`) for two LaunchPads with DALI transceivers. The
   maintainer keeps a copy in the gitignored `_local/`. Patch RANDOMISE so that
   its first call returns a fixed value, and later calls return random ones.
2. Put both on a bench bus with the controller and run `commission
   unaddressed`. Expected:
   - `commission: random=0x... answered from two gear; short N taken back,
     both left unaddressed`
   - the closing summary `1 random address(es) held by two gear`
3. Run it again. Both should now be assigned, and the post-scan should confirm
   2 of 2.
4. Repeat with `dali_103` and `commission devices` for the device walk.

TI's code has known edge-case bugs; see `project_log.md`, *A device-side
source sides with Beckhoff on both Part 103 encodings*. Trust it for the
selection logic only, and read any surprise against the standard.

### DT8 gear

No DT8 gear is on either site. With a Tc or RGBWAF unit:

1. Run `query <a> device-type`, which should read 8, then `dt8 <a>
   colour-type-features` and `dt8 <a> features`.
2. Run `dt8 <a> set-tc <lo> <hi>`, `dt8 <a> activate`, then `dt8 <a> colour
   tc`. The Kelvin read back should match what was set.
3. Run the `x`/`y` pair the same way on an xy-capable unit, and `set-rgb` plus
   `activate` on RGBWAF.
4. Run the store commands only on a unit whose original values have been read
   first.

Once these pass, DT8 can go on the console, and the HA colour mapping under
*Build* can start.

### Lunatone sensor queries

This needs a Lunatone DALI-2 sensor. Run `vendor lunatone <addr> <instance>
multiplicator`, then `divisor`, `offset-msb`, `offset-lsb`, `offset-mult`,
`offset-div` and `unit`. The scaling they describe should turn
`sensor poll` values into the unit the device's datasheet gives.

### Bus timing with a logic analyzer

The reply window and frame spacing have only been checked from the
controller's own timestamps. Tap the bus, or the Click's RX line, at 100 kHz
or more. Then:

1. **Reply window.** For a single query, measure from the end of our frame's
   last data bit to the reply. Compare with `since_tx_us` from `capture`. They
   should agree to within an edge.
2. **The three edges.**
   - 5.5 ms for undecodable activity, which needs two units colliding: use
     *Bench A*.
   - 3.664 ms for a decoded frame.
   - 28.664 ms for the close.

   Each needs a unit, or an injected frame, that answers near the edge.
3. **Spacing after received frames.** On 2k with events flowing, count our
   transmissions that start within 2.4 ms of the end of a received frame.
   Today the code allows about 1.7 ms after an event and about 2.9 ms after a
   reply, so expect some. This is the measurement *Space forward frames from
   received frames* asks for before any change.
4. **Send-twice.** Measure the gap between the two frames of a pair. It must
   stay under 100 ms.
5. **External master.** Have a Lunatone DALI USB send during our query.
   Expect `rx-activity` or `intervened`, never a wrong decoded answer.

---

## Bus data for open investigations

### Why the retry backoff works

`DALI_REPLY_TIMEOUT_BACKOFF_US` took the 2k missing-lamp rate from 2/8 scans
to 8/8. But `rx_reply_late` reads 0, so the late reply it was added for is not
what happens, as its comment now says. The emitter-spacing candidate does not
account for it either. Background is in `project_log.md`, *Nothing spaces a forward frame
from a received one*.

1. **Decisive test.** Build `66e85d0^`, the commit before the backoff. On 2k,
   run 8 scans under `quiescent on all`; that build predates the bracket, so
   quiesce by hand.
   - If all 8 find every lamp, event traffic was what the backoff was working
     around.
   - A build that old may not compile on current ESPHome. If so, flash the
     native firmware instead and drive it over serial.
2. **Cheap test.** On current `dev`, run `discover` ten times on 2k and count
   the walks with no instance-enumeration line for a0. `discover` now
   silences control devices, so a loss here is not event traffic.

### How often a query to present gear is lost

The same counting as the cheap test above. It also bounds *Report a failed
input-device enumeration* under *Fix*.

---

# Part 2 — Develop

## Fix

### P0 — The RX edge interrupt is not IRAM-safe

The TX bit clock is cache-safe in both builds. The RX handler is not: it goes
through the GPIO ISR service, which was installed without
`ESP_INTR_FLAG_IRAM`.

- Every NVS commit masks it: light restore state, the group map, the backup.
  So a backward frame or event that arrives during a commit loses edges, and
  decodes as silence or garbage.
- The service is shared with ESPHome, which installs it on its own terms. This
  and *The DALI PHY can break every other GPIO interrupt* want one fix.

None of the fixes `project_log.md` weighed on 2026-09-25 fixes both. A fourth
is designed in Part 3, *RX capture off the shared GPIO ISR service*. It is a
software-stack change, so it waits for a go-ahead.

### P1 — Priority, collisions and the send-twice pair

Our frames are now spaced from received ones; see Part 1, *Spacing from
received frames*. After another master's frame, ours always takes DALI-2
priority 1's 13.5 ms. Still missing:

- **Priority and randomisation.** TI's devices wait 13.5, 14.9, 16.3, 17.9 or
  19.5 ms by priority, each a range. Ours always takes the shortest and its
  start, so it goes first against any DALI-2 device and collides with another
  master doing the same. Which priority an application controller's frames
  carry is a reading of IEC 62386-101 not yet made.
- **Collision detection and recovery.** TI compares each RX half-bit with what
  it sent, stops on a mismatch, breaks for 1.3 ms, and retries after 4.3 ms.
  The PHY suppresses RX through its own TX, so it cannot see a collision at
  all.
- **A send-twice pair split by another master's frame.** The halves must
  arrive as consecutive frames, as recalled from the standard; this is not
  checked against the text or TI's source. The scheduler sends the second half
  anyway and reports success. Fix: when a forward frame is routed between the
  halves, end the step with `DALI_ERR_INTERVENED` and let the caller resend
  the pair.
- **A deadline-aware PHY call** for when a repeat would cross the 100 ms
  send-twice limit.

TI's SDK is a compact reference for the settle table and for collision
detection, avoidance and recovery. Its receiver accepts a send-twice pair whose
stop conditions are up to 94 ms apart, which is looser than the scheduler's own
bracket.

### P1 — `tools/dali-shell` waits up to 400 s for a device that restarted

Every command, `schema` included, waits with the 400 s command timeout. A
device that restarted sends nothing, and neither does the waiting client, so
no reset arrives to end the wait. Seen on 2026-11-09, when connecting reset 2k;
see `project_log.md`.

Fix. This is a software-stack change, so it waits for a go-ahead:
- **Give `schema` a short deadline,** about 15 s. On 2k it now returns at once;
  see `project_log.md`, 2026-11-10.
- **Set TCP keepalive on the client socket,** so any command notices a device
  that restarted within seconds. On Windows that is `SIO_KEEPALIVE_VALS`, and
  elsewhere `TCP_KEEPIDLE`, `TCP_KEEPINTVL` and `TCP_KEEPCNT`.

### P1 — The DALI PHY can break every other GPIO interrupt

`dali_phy_init()` installs the shared GPIO ISR service with flags `0`, and
accepts `ESP_ERR_INVALID_STATE` if someone got there first. ESPHome's
`ESP32InternalGPIOPin::attach_interrupt()` installs it with
`ESP_INTR_FLAG_LEVEL3`. It treats `ESP_ERR_INVALID_STATE` as failure: it logs
an error and returns without registering its handler.

So when the DALI component sets up first, anything that attaches a pin
interrupt later is dead. Both components sit at `setup_priority::HARDWARE`,
and registration order breaks the tie. The most visible victim is a `gpio`
binary sensor, which on ESP32 defaults to interrupt mode and stops polling.
Neither tracked config has one, and `use_interrupt: false` is the workaround.

Fixes, cheapest first:

- Move the install out of `setup()`, so ESPHome always installs first.
- Attach RX through ESPHome's own pin API.
- Fix ESPHome upstream to accept an already installed service.

Part 3, *RX capture off the shared GPIO ISR service*, takes the first as its
step 1 and fixes the RX interrupt with it. Reproduce the defect first: *The
GPIO ISR-service conflict*.

### P1 — Entity limits fail silently

The 33rd light, 17th sensor and 33rd dispatch rule are dropped with only a
boot-time log line. An unregistered light never gets a profile and can never
transmit. Fix: reject over-limit configs in the schema, and size the light
registry for 64 + 16 + 1.

### P1 — The PHY busy-waits on the loop's core

ESPHome 2026.9 pins `loopTask` to core 1, the core `dali_worker_core()` also
gives the DALI, scan and shell tasks. Synchronization is unaffected, and the
comments in the component say which task owns what rather than which core.
But the PHY's pre-TX busy-wait runs on that shared core: 1.67 ms per frame, and
33 ms per attempt on a stuck bus.

A frame already on the wire no longer holds it: the PHY declines with
`DALI_ERR_BUS_ACTIVE` instead. The RX ISR now records its last accepted edge,
`s_rx_last_isr_edge_us`, which is the edge time this fix wants: the idle check
can read how long the bus has been quiet instead of polling it for 1.67 ms.

### P1 — Record an address a walk cannot read

A status or instance probe that fails for a reason that says nothing about the
address, `INTERVENED` or `OVERFLOW`, is asked once more. A second such failure
at the same address still ends the walk. Nothing records an address as
unreadable, and one the walk could not read must not be reported free.

Wanted: a flag, say `has_unreadable_status`, that reserves the address
wherever a contested gear address is reserved today, without calling it
contested. Contested means more than one unit; this means nothing is known. It
reaches:

- the commissioning used mask, which reads `has_undecodable_activity` directly
- the commissioning audit
- the restore planner, which needs a conflict kind of its own, because the
  remedy is a rescan, not `address aN clear`
- `backup save`, `discover`, the scan log and `export inventory`

### P1 — Clearing a shared device address

When two control devices share an address, `address d<N> clear` can read the
collision as silence and refuse with `does not answer`. Settle the mechanism
first with *`address d<N> clear` at a shared device address: capture the
collision*. There are two candidates:

- The merged reply falls outside the window and is dropped as noise.
- A MALFORMED first attempt is retried, and the retry's silence replaces it.

Every query shares that path, so do not change the scheduler before the
capture.

### P2 — Reserve the scheduler for a scan

The scan gate is a check before enqueue, not an admission reservation. The
TCP and web shell now check it before every frame, as every component producer
already did, so nothing new is queued once a Home Assistant scan raises it,
and the scan waits for the queue to drain. A frame that passed the check in
the moment before the gate went up still goes out, ahead of the scan's first
query. A reservation at `dali_sched_enqueue()` itself would close that: an
owner token, and while it is held only that owner's work is admitted.

### P2 — The console and the refresh cursor act on admission

Headless dispatch now acts on transmission. Two clients still act on
admission, and neither publishes a state it has not read:

- **The console's `OK` means queued,** as documented. A verb that waited for
  its frame would publish `pending`, then `OK` or the error, as the
  asynchronous verbs already do.
- **The refresh pump moves its cursor once a light's level query is queued.**
  A read that then fails is retried only on the next pass. Advancing from the
  reply instead would retry it at once.

### P2 — Closed probes fill a busy shell's queue

While a session is open, a TCP client waits in the listen backlog, which
holds two. A connection that closes politely keeps its place until the
session ends. `dali-shell --list`, and `dali-shell` without `--host`, probe
every node and close politely. So after two of them, the next client's
connect is never answered: `dali-shell --host` reports `cannot reach
<ip>:2323: timed out`, not that the shell is busy. Seen on 1k on 2026-11-11;
see `project_log.md`.

A probe that closes with a reset frees its place at once. Measured the same
day: after two reset probes, a third connect was accepted in 0.05 s.

Fix, in `tools/dali-shell`. A software-stack change, so it waits for a
go-ahead:
- **Close a silent probe with a reset** (`SO_LINGER` 0) in `probe_shell()`.
  Keep the orderly close for a probe that got a greeting, which ends its
  session cleanly.
- **Name both causes when a connect times out:** the device is unreachable,
  or its shell is busy with two clients already queued.

Then repeat *Second client* from `project_log.md`, 2026-11-11: `--list`, then
`dali-shell` with no `--host`, then `--host`, all while one session is held.
The last should report the shell in use after 10 s.

### P2 — Small hardening items from the stack review

- `dali_rb_clear()` should drain from the consumer side (`tail = head`). In
  the native build, the ISRs and the DALI task run on different cores.
- Delete the stale `DALI_SHELL_*_MAX` macros in `dali_shell.h`; `dali_shell.c`
  does not use them.
- Policy-gate `devmem write`, as the Part 102 memory writes are.
- Give the retry-safe QUERY INPUT VALUE step in sensor polls one retry.
- Give `backup import` blobs a checksum.
- Stop native `trace on` writing to the UART from the DALI task. It blocks
  once the TX buffer fills.
- Log why a network session closed when the peer stopped reading or went
  away, as the idle timeout already does. Today the log reads only `Shell
  session closed`.

## Build

### P1 — Addressing faults and membership changes reach Home Assistant

`bus_fault` is a PHY liveness signal. It read `OK` throughout a real
collision, an address wipe and a commissioning walk.

The scan already computes `undecodable_count`, `identity_collision_count` and
the per-address flags, and logs them. Wanted: either an addressing-health
sensor, or a widened `bus_fault` that reports `Contested: a4`.

The same gap exists one layer along. A group light entity is not told when its
group's membership changes.

### P1 — Part 103 device groups: a read and write path

Device groups are decoded as an event source and nowhere else. A replaced
control device gets its address back, but not its group membership.

Wanted, in order:

1. a query path in discovery
2. a `groups` field on the device snapshot entry
3. the planner and the `address <dN> add|remove` arms

The opcodes are in `dali_protocol.md`. QUERY DEVICE GROUPS (`0x41`–`0x44`)
comes from TI and `esp_dali`; ADD and REMOVE (`0x19`–`0x1C`) from TI alone.

The same sources give two more device queries:

- QUERY MISSING SHORT ADDRESS (`0x33`), which would back `address <dN> clear`
  as the broadcast query backs the gear arm
- QUERY QUIESCENT MODE (`0x40`), which would let a bracket's release be read
  back per device

In TI's firmware, a device RESET also resets every instance's event scheme,
filter and groups. `restore instances` puts back the first two, and the
instance groups; device groups would need this item.

### P1 — Host vectors for the unasserted shell outputs

No host vector covers these output formats:

- `identify`, `smoke` and `capture`
- the inventory JSON export
- `export config`

`export config` matters most. Its output is useful only if it validates, which
a vector could assert against the ESPHome schema. The console's dispatch in
`dali_component.cpp` is ESPHome- and FreeRTOS-bound and has no vectors either.
Its parsing and decoding are shared code that does have them.

### P1 — Find Couplers, paged or exportable

Today it gives one summary, truncated with a count. The log already holds
every frame.

### P2 — DT8 in Home Assistant

Map DT8 to the colour-temperature, XY and RGB/RGBWAF traits, and put the `dt8`
verbs on the console. This starts after *DT8 gear* passes.

### P2 — Typed Part 303 and Part 304 profiles

Occupancy and illuminance profiles, beside the generic sensor platform.

### P2 — A Home Assistant commissioning workflow

Only after the shared path is proven on hardware. The TCP shell already
exposes the guarded verb behind `allow_commissioning: true`.

### P2 — Scene and fade UX

A level-changing command arms a deferred re-read. Two problems remain:

- The refresh it triggers is a full pass over every light, not a query of the
  one that moved.
- Nothing reads a final value after a transition longer than the arming
  window.

Scenes are not captured by `backup save` either.

### P2 — Test infrastructure

Capture replay, parser fuzzing, and hardware-in-loop tests for:

- timing
- collisions
- queue pressure
- bus faults
- power restoration

### P2 — More device types

Add DT1 and others only when an installation needs them, following the DT6/DT8
module pattern.

## Decide

### What an input sensor shows when its device stops answering

Today a sensor holds its last value indefinitely. `DaliInputSensor` publishes
only after a complete read, in `on_input_value_done()`. A failed poll
publishes nothing, and nothing marks the entity stale.

On 2k, "Zone 2 Lux" sat at 54 lx for as long as the Steinel shared d0 with
the Casambi and then had no address, which reads as a working sensor. It is
unconfirmed whether 54 was the last good reading or a collided read that
decoded; HA's history would say. Occupancy is the costly case: a dead sensor
stuck on "present" keeps the zone occupied.

To decide:

- Whether to go stale at all.
- After what: N consecutive failed polls, or a time since the last good read
  scaled by `poll_interval`.
- What to publish. `NAN` shows as "unknown", but also fires `on_value` with
  NaN, which site lambdas must then handle.
- Whether it is per-sensor YAML, with a default.
- Whether text and binary entities derived in YAML follow automatically.

### The brightness mapping

HA brightness maps linearly onto light output and bypasses ESPHome's
`gamma_correct` (default 2.8). A DALI light is therefore much brighter at
mid-slider than other ESPHome lights, and the dim half of the arc range sits
in the bottom few percent of the slider. Decide this on purpose.

### Recovery after a bus-only power cycle

Define it, beyond the split between current and cumulative faults that
`bus_fault` already makes.

### Bus topology

Direct-control couplers also transmit. Their coexistence is field-tested, but
that is not collision-safe single-master arbitration. Say so in the README and
`commissioning_readme.md`, or design for it.

## Release process

### Before tagging the next release

Nothing yet. A `dev` change that makes a pinned comment wrong goes here.

### Every release

- **Pin the tag in the tree being tagged.** At `v2.0.0`, `dali-starter.yaml`
  and the README still pinned `v1.3.0`; `7f26fef` fixed that after tagging.
  - Bump the pins in the release-prep commit, with the comments that described
    the previous tag.
  - A pin to a tag that does not exist yet fails `esphome config` on the
    starter. Merge and tag locally, then push `dev`, `main` and the tag in one
    `git push --atomic`.
  - Make `release-packaging.yml` fail when a tag's starter config names a
    different ref. Not done: at `v3.0.0` the pins were still bumped by hand.
- **Check `CHANGELOG.md`'s Unreleased section against the tag.** Check each
  claim, not the heading: `git grep <symbol> <previous tag>` settles it in
  seconds. Re-check every verification label against `project_log.md`, then
  rename the heading to the version.
  - The list for v2.0.0 had not been emptied at v1.3.0, so it claimed two
    releases' worth of breaks as new.
  - At v3.0.0, one label covered a fix made after the bus run it cited.
- **Move the ESPHome pin to the current release** in `pyproject.toml`, run `uv
  lock`, and compile. The memory table below and CI then describe the version
  users build with.
- **Re-measure the README's memory table.**
  - Compile `dali_test.yaml` with and without its `shell:` block.
  - Then update the RAM line in the hardware table.
- **Bump `manifest.json`** if the version range item in Part 1 moved the floor.

### Standing

- **Enforce, or document, the ESPHome version range.** `idf-build.yml` pins
  IDF 6.0.1 and `esphome-build.yml` builds the `uv.lock` pin, so each fails as
  soon as its own version stops building. The range `manifest.json` claims is
  still advisory between those points: `release-packaging.yml` builds the
  latest ESPHome only at a tag, and nothing builds the floor. The two builds
  compile the same `components/dali` C with different toolchains.
- **Keep local filenames hyphenated in all documentation.** Re-check
  `dali_commands.md`, which tracks both verb surfaces, whenever either table
  changes.
- **Complete release provenance:** project SPDX identifiers, and the full
  vendored Unity MIT license and third-party notice.

# Part 3 — Design

## RX capture off the shared GPIO ISR service

Not approved. It would fix *P0 — The RX edge interrupt is not IRAM-safe* and
*P1 — The DALI PHY can break every other GPIO interrupt* together.

**The problem.** The RX edge interrupt goes through the GPIO ISR service, which
every GPIO interrupt in the firmware shares, ESPHome's included. Two things
follow:

- It is masked during every flash write, because one handler on a shared
  interrupt that is not IRAM-safe makes all of them non-IRAM. An edge during an
  NVS commit is lost.
- Whoever installs the service first sets its flags. ESPHome refuses a service
  someone else installed, so a pin interrupt that attaches after DALI's
  `setup()` never gets its handler.

**What was weighed.** The first three are from `project_log.md`, 2026-09-25.

| Option | Conflict | IRAM | Cost |
|---|---|---|---|
| Install after ESPHome's `setup()` | Fixed | Not fixed | Small |
| Attach RX through ESPHome's pin API | Fixed | Not fixed | Small; ties the PHY to ESPHome, which the layering forbids |
| Fix ESPHome upstream, then install with `ESP_INTR_FLAG_IRAM` | Fixed | Fixed, if every other handler is IRAM-safe, which is not ours to promise | Waits on upstream |
| Capture RX edges with a peripheral that has its own interrupt | Fixed: DALI leaves the service | Fixed: that driver's ISR can be made cache-safe on its own | A new RX front end in the PHY |

**The fourth, in more detail.** Two ESP32 peripherals could do it:

- **MCPWM capture.** It timestamps each edge in hardware and raises an
  interrupt per edge, with its direction. That maps onto the buffer-first
  model unchanged: the capture ISR pushes timestamp and level into the same
  ring, and the decoder does not change. The edge time comes from the capture
  register rather than from when the ISR ran, which is better than today. The
  ESP32, S3, C6 and H2 have it; the S2, C3 and C2 do not.
- **RMT receive.** Every variant has it. It hands over whole frames as
  level-and-duration pairs once the line has been idle for a set time, so the
  decoder's input changes from edges to durations. Absolute times must be
  inferred from when the frame completed. ESPHome's `remote_receiver` and
  `remote_transmitter` share the channels.

**Intended implementation, if approved.** Capture with MCPWM where the variant
has it, and keep the GPIO path for the rest.

1. The GPIO path installs its service only after ESPHome's own `setup()` has
   run, so it never takes the service first. That alone fixes the conflict on
   every variant.
2. On MCPWM variants, a capture channel on the RX pin, both edges, pushes into
   the existing ring from an IRAM-safe callback. Set the driver's cache-safe
   option, the MCPWM counterpart of `CONFIG_GPTIMER_ISR_CACHE_SAFE`, in
   `__init__.py` and `sdkconfig.defaults`; check its name on IDF 5.5.6 and
   6.0.1.
3. The self-echo and settle suppression move into the capture callback, which
   keeps the ISR rules: no logging, no allocation, counters only.
4. `rx_activity_unreported()` and the reply windows keep working, provided
   capture timestamps are converted to the `esp_timer` microseconds the
   scheduler compares them with.

**Verification.** *Bus timing with a logic analyzer*, plus a bench check:
stream frames while forcing NVS commits, as the 2026-09-25 stack review
proposed for TX, and compare decoded frames against a capture.

## A browser GUI for the shell

A small web server on the device whose page is a GUI for the shell. It would
have a terminal, plus views built from what the verbs already report:
- a command reference with real descriptions,
- a bus map,
- per-device panels, such as a Steinel's sensitivity as a slider rather than
  `devmem write d0 2 4 128`,
- a live bus log.

Commissioning would move there, and the Home Assistant console and buttons
would go. Step 3a of *Order of work*, the binding and a terminal page, was
approved on 2026-11-10 and is built. Nothing after it is approved: each is a
software-stack change, so it waits for an explicit go-ahead.

If adopted, this replaces *P2 — A Home Assistant commissioning workflow*.

### Budget

**Heap, as observed on a device:** about 80 kB free from boot to the first
shell use, and about 60 kB with a shell session open.

**Measured on 2k, 2026-11-09,** with the caps below:
- **Opening a TCP shell session** moved the low-water mark from 99,828 B, the
  low since boot, to 76,612 B.
- **For the session,** the largest block fell from above about 106 kB to
  77,824 B.
- **`discover` and `trace on`** set no new low.
- **Closing the session** gave the block back.
- **Free heap** fell from about 108 kB to about 80 kB at open. About 7 s later
  it was back to within about 300 B, and those 300 B returned at close.

So a session holds about 300 B. Opening an interactive `tools/dali-shell`
session cost about 28 kB for seconds. The client runs `schema` at once, about
6,500 one-character sends.
- ESP-IDF's `LWIP_NETIF_TX_SINGLE_PBUF` gives each segment a full 1,440 B
  MSS buffer, however short.
- Under `TCP_NODELAY`, 16 one-byte segments can wait for an ACK at once.

So the spike was that burst, not the connection. `schema` now writes in
159-byte chunks, and the TCP binding gathers writes into sends of a line or
more. **Confirmed on 2k, 2026-11-10:** opening a session no longer dips free
heap, and a session holds about 300 B; see `project_log.md`. The web binding's
heap floor is therefore a reserve for other connections, not a budget for its
own: 16 kB. **Measured on 2026-11-11,** a page load costs about 13.5 kB for
the 0.1 s it streams, on both nodes, so the floor covers one. `help` cost
about 20 kB until the bindings stopped sending at every line end, and about
14 kB on 2k since; see `project_log.md`, 2026-11-11.

**Read the right sensor.** ESPHome's debug `free`, `block` and `min_free` use
`MALLOC_CAP_INTERNAL`. That counts about 52 kB of IRAM that takes 32-bit access
only, so 1k's `free` reads about 147 kB while about 94 kB is usable. On a PSRAM
build `MALLOC_CAP_8BIT` alone counts the PSRAM. Measure with
`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, as `_local/dali-2k.yaml` now does. See
`project_log.md`, 2026-11-09.

- **The shell is not the 20 kB.**
  - Its caches, about 62 KiB, are static `.bss` inside the 141,792 B of static
    DRAM, so they leave the heap before boot.
  - Nothing in `components/dali` calls malloc, `dali_shell_attach()`
    included.
  - The session task's 12,288 B stack is created in `setup()`, so it is
    already inside the 80 kB.
- **The drop is the network stack.**
  - Each connection can hold `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` (5,760 B) of
    unacknowledged output and `CONFIG_LWIP_TCP_WND_DEFAULT` (5,760 B) of
    unread input.
  - The Wi-Fi driver allocates up to 32 dynamic RX buffers and 32 dynamic TX
    buffers, about 1.6 kB each, as traffic flows.

  An idle connection measured about 300 B. The 20 kB was the spike at open,
  caught by a sensor, and the `schema` the client runs then is the likely
  cause.
- **Enrichment costs flash, not heap.** The browser renders. The device serves
  a constant blob from flash and streams verb output through `shell_printf()`,
  as it already does.

| Item | Heap | Flash |
|---|---|---|
| The page with `dali_commands.md` built in, gzipped, as of 3b | 0 | 46,964 B. The reference is most of it |
| `commissioning_readme.md`, `steinel_bank2_reference.md` as further built-in help | 0 | 29,996 B with `gzip -9` (25,110 + 4,886 apart), not built in |
| Serving the page: one short connection | the send buffer while it streams, then 0 | — |
| A WebSocket session | about what a TCP session costs now; one replaces the other | about 8 KB of code |
| Flash headroom on the `dali_test.yaml` build | | 798,624 B: a 1,036,384 B image in a 1,835,008 B app slot |

### Transport: HTTP and WebSocket on the shell task

**Chosen.** A minimal HTTP/1.1 listener on the `dali_shell` task, beside port
2323, with one `select()` over both listeners and the client.

- `GET /` serves the gzipped page from flash.
- `GET /ws` upgrades to a WebSocket, and that connection *is* the shell
  session:
  - `write_cb` gathers writes as the TCP binding does, and sends one text
    frame per flush: before a bus wait, after the prompt, or when full. A
    frame per write would make even a line printed in pieces several frames,
    and a frame per line made `help` 116 of them.
  - Each client text frame is one command line, fed to
    `dali_shell_feed_byte()`.
  - The prompt goes in a frame of its own, so the page knows a command has
    finished. That is framing, and it stays within what a binding may do.
- **No new task and no new stack.**
- **SHA-1 and base64 are `dali_web`'s own, about 1 KB of flash.** mbedTLS has
  both in the image (`CONFIG_MBEDTLS_SHA1_C=y`), but using them would give
  `components/dali` an IDF dependency and leave the handshake untestable on the
  host. The local copies are checked against the FIPS 180 and RFC 6455 vectors.
- **Frames.**
  - Client frames must be masked, unfragmented and no longer than 125 bytes.
    A command line is at most `DALI_SHELL_LINE_MAX`, 80 characters.
  - Ping gets pong, and close gets close. Anything else ends the session.
- **Parsing is portable.** The request-line and header parser, the handshake
  and frame encode/decode are pure functions in `components/dali`, host-tested
  like `dali_cli_peer_sniff()`. The binding in `esphome/components/dali` moves
  bytes and owns the session's lifetime, as `dali_shell_tcp.cpp` does.
- **What a second browser sees.** While a session runs, `select()` still
  accepts. A page request is served. A second upgrade gets a close frame that
  says the shell is busy, so it does not wait unanswered in the backlog, as a
  second TCP client does today.
  - **Only between verbs.** `dali_shell_dispatch()` holds the task for the
    whole verb, and a `discover` takes tens of seconds. While it runs, nothing
    is accepted, and a page load waits for the verb to end. Either accept that,
    or serve the page from somewhere that does not wait on the shell.
- **There is no cancel.** The session's `aborted` hook fires only when a send
  fails. A page will want a Stop button. The binding's `aborted` could peek
  the socket without blocking and treat a close frame, or an agreed cancel
  line, as the stop. The TCP shell has the same gap, so fix it in both
  bindings.

**Rejected:**
- **ESPHome `web_server:`.** It is an entity UI, and it loads its JS from a
  CDN unless `local: true` is set. None of the views above fit it.
- **`web_server_base` / `esp_http_server`.** It is already linked, through
  `captive_portal`, but it starts only in AP mode.
  - Starting it costs a permanent 4,352 B task stack: IDF's 4,096 plus
    ESPHome's 256.
  - It runs handlers one at a time on that task, so a streaming `discover`
    would block every other request. Bridging to the shell task means queues,
    and coupling to `web_server_idf` internals that change between ESPHome
    releases.
  - `web_server_base` adds `Access-Control-Allow-Origin: *` to every response.
  - It does not enable `CONFIG_HTTPD_WS_SUPPORT`.

**Port.** Not 80, and the schema refuses a port another component holds.
- The captive portal binds 80 when the AP comes up, and `web_server:` binds it
  permanently.
- AP mode is where the GUI is worth most: a phone on `dali-starter-ap`
  commissioning a site that has no network yet. So the page loads nothing from
  a CDN.

**YAML,** a sub-block of the shell it binds:

```yaml
dali:
  shell:
    port: 2323
    allow_commissioning: true   # one policy for both bindings
    web:
      port: 8080
      allowed_hosts: [dali.lan] # beyond the IP literal, <name>.local and <name>
```

`dali_test.yaml` gains the block in the same change.

### Security

A WebSocket is a wider hole than the POST that `dali_cli_peer_sniff()` closed.
Browsers apply no CORS to it, and the attacking page can read every reply. The
upgrade is refused unless both checks pass, and both have host vectors:

1. **`Host`** is the device's IP literal, `<name>.local`, `<name>`, or a name
   in `allowed_hosts`. This blocks DNS rebinding, where `Host` and `Origin`
   both name the attacker's domain and agree with each other.
2. **`Origin`** is exactly `http://` plus that `Host`. This blocks cross-site
   WebSocket hijacking. A missing `Origin` is refused too, because every
   browser sends one on an upgrade.

`GET /` checks `Host` as well. The page holds nothing secret, but the check
keeps the two paths identical.

A `Host` that names a port must name the listener's. One that names none
means port 80, because a browser omits only the default.

**Framing is forbidden** (`X-Frame-Options: DENY`, `frame-ancestors 'none'`).
A page elsewhere could otherwise show the device's own page in an invisible
frame, where its socket passes both checks, and steer the operator's clicks
into it: clickjacking onto `commission`. The rest of the page's
`Content-Security-Policy` waits for the page. Whether `connect-src 'self'`
covers `ws:` needs checking in each browser first.

The session takes the same `DALI_SHELL_ALLOW_*` policy as the TCP binding. The
page greys out what the policy refuses. It never decides that itself.

There is no HTTPS and no real authentication: a TLS handshake needs on the
order of 30–40 kB of heap. Trust stays where the TCP shell and OTA already
are: anyone who can reach the port on the LAN.

### Content

**The rule: DALI knowledge stays in C tables, and reaches the page through
`schema`.** The page is a client, as `tools/dali-shell` is. It composes verbs
and renders their output, and holds no opcode, offset or range table of its
own. A page that grew one would be a second decoder that drifts, which is how
the HA console became a second implementation.

**On verbs that exist today:**

- **Command reference.** `schema` already emits every verb with its args,
  summary, argument counts and subcommands, and every named command table.
  - Forms and completion come from that.
  - The long descriptions are compiled in from `dali_commands.md` at codegen
    time, so the help always matches the firmware. `type: git` clones the
    whole repository, so the file is there; the `proto_dali_*.c` shims rely on
    the same fact.
- **Bus map**, from `export inventory`. Per address it gives status, device
  type, level, groups, GTIN, identification number, firmware, level window,
  curve and scenes, plus cached input devices and switch mappings. That is
  enough for:
  - a 64-cell gear grid and a 64-cell control-device grid,
  - a 64×16 group matrix,
  - contested and undecodable flags,
  - click to `identify` a unit, or to open its panel.
- **Files instead of copy-paste.**
  - `export config` becomes a YAML download.
  - `backup export` becomes a file.
  - `backup import` becomes an upload, which the page sends as the existing
    `begin`, `HEX` and `end` lines.
- **A commissioning stepper.** The flow in `commissioning_readme.md` (discover,
  commission gear, commission devices, find switches, export, back up) as
  steps, with the live output beside each one. `commission`, `address`,
  `backup`, `restore` and `dt8`, all refused by the HA console, are available
  because this is the shell.

**Small C additions that both shells gain:**

- **Parameter metadata in `schema`.**
  - Per instance type: each `iquery`/`iconfig` name, its range and unit. For
    example, the Part 303 hold timer in 10 s steps.
  - Vendor register maps from a table in `dali_steinel.c`: Bank 2 offsets with
    labels and ranges, `0x04` global sensitivity among them.
  - The panels render from this metadata.
- **One structured read per panel.** For example, `export device <aN|dN>`
  prints one JSON object. Its instance half already exists as the backup v2
  instance-settings reader in `dali_restore`.
- **Write, then read back, inside the verb.** This is the Known Limitation *No
  memory or configuration write reads itself back*. Fixed in `iconfig`,
  `devmem write` and `config`, every surface gains it. Fixed in the page alone,
  only the page would.

### Live log

**Sources.**
- The trace subscriber already sees every frame on the wire:
  - our own forward frames,
  - backward frames,
  - other masters' forward frames,
  - events.

  Each has a µs timestamp and, for a reply, the time since our TX.
- Lines start with `[BUS]`, so the page can route them out of the terminal
  pane.
- **The prompt can share a line with trace.** A trace line pumped between
  commands follows the prompt, so a TCP client reads `> [BUS] TX ...`. The
  page is unaffected, because the prompt has a frame of its own.
- With `defer_foreign_task_output` they already pass through the deferred ring.
  They never block the DALI task, as the native UART trace can.

**Rate.**
- One 16-bit forward frame and its 22 Te gap take about 25 ms, so a loaded bus
  carries about 40–50 frames a second, replies included. Decoded, that is
  about 4 kB/s.
- The deferred ring holds 16 lines of 128 B. Between verbs it is pumped every
  250 ms (`SHELL_POLL_MS`), a ceiling of about 64 lines a second. During a
  verb it is pumped before every bus wait. A 2k walk raised about 34 lines a
  second and a 1k walk about 46, and none was dropped. While trace is on,
  pump it from the `select()` timeout at a shorter interval rather than
  growing the ring, because each slot is static RAM on every board.
- Dropped lines are already reported as `[trace] N line(s) dropped`.

**To build:**
- **A frame-to-text decoder in `components/dali`,** host-tested. It reverse-
  looks-up the command tables `dali_cli` already holds, and decodes events
  through `dali_event_parse_frame()`.
  The aim is `a3 QUERY ACTUAL LEVEL -> 254` and `d5 i1 occupied`, not
  `0x47A0 (16-bit)`. The serial `trace` gains it too.
  - **Call an RX a reply only inside the reply window.** Today every RX that
    follows any TX carries the time since it, so a Steinel event 9.9 s after
    a poll reads `RX 0x008001 (24-bit, 9923.9 ms after TX)`.
- **History on connect.** If `capture` is running, the page pulls
  `capture export` when it connects, to show the last 128 records.
- **The idle timeout.** It counts input only, and a page cannot send a
  WebSocket ping from script. A page that only watches `trace on` is
  therefore closed after `idle_timeout`, 10 minutes on both nodes. Either
  count output as activity while trace is on, or have the page send a line.

**What it cannot show today:**
- Anything from before the session opened, except the capture ring.
- Frames lost during an NVS commit; see *P0 — The RX edge interrupt is not
  IRAM-safe*.
- Which foreign master a backward frame answered. It can say only which frame
  it followed.

### Out of scope

- **More than one live session.** The shell is single-session, and each
  connection pays its own buffers.
- **JSON assembled in RAM.** Every structured verb streams through
  `shell_printf()`, as `export inventory` does.
- **Auto-refreshing dashboards.** The limit is the bus, not RAM. DALI manages
  about 40 transactions a second, and the refresh pump and sensor polls already
  use part of that.
- **A JS framework or a CDN.** Vanilla JS, no build toolchain. `__init__.py`
  gzips the page and the docs into a generated header at codegen time.
- **The native firmware.** The parser is portable, but only the ESPHome
  binding is planned.

### Removing the Home Assistant console and buttons

**What stays: the runtime controller.**
- light entities,
- input sensors,
- headless dispatch,
- `bus_fault`.

**What goes:**
- the eight `button:` types and the Target Address `number:`,
- the `text:` console and `command_result`,
- `scan_status`, `scan_result`, `yaml_result` and `couplers_result`,
- `bus_monitor`, which the live log supersedes.

Only these platforms call `start_scan()`, `start_identify()`,
`start_find_couplers()`, `execute_command()` and the `send_diag_*()` family,
so the cut is clean.

**What it deletes:**
- **The console handlers,** `execute_command()` and `console_*_()`: about
  1,140 lines of `dali_component.cpp`. They are a second implementation of
  shell verbs, with no host vectors, that no bus has run.
- **`dali_scan.cpp`,** 612 lines, and the 8,192 B task stack each button scan
  takes from the heap.
- **2,224 B of static buffers** (`s_scan_yaml_str`, `s_scan_result_str`,
  `s_bus_monitor_str`), plus the strings those text sensors hold in heap.
- **These items, from this file:**
  - *The command console, verb by verb* (Part 1).
  - The console half of *P2 — DT8 in Home Assistant*.
  - *P1 — Find Couplers, paged or exportable*.
  - The button-scan half of *P2 — Reserve the scheduler for a scan*. The
    claim's missing atomic reservation stays.
  - The console's `OK` in *P2 — The console and the refresh cursor act on
    admission*.

**What is lost:**
- **Commissioning through Home Assistant's remote access without a terminal.**
  The page is plain HTTP on the LAN, and an HTTPS dashboard cannot embed it
  (mixed content). The TCP shell through HA's terminal add-on still works
  remotely.
- **Acting while another client holds the shell's one session.**
- **The two-walk cross-check.** The button scan and `discover` are separate
  implementations, and a disagreement was a finding. Lunatone DALI Cockpit
  stays the external reference.
- **Results in Home Assistant's history.**
- **`dali-starter.yaml` and the README example as they stand.** Both use the
  platforms, so this is a breaking change for a major tag: a migration note in
  `CHANGELOG.md`, and new pinned configs at that tag.

**Answered 2026-11-09: no automation writes to the DALI Command text
entity.** Neither installation exposes the console, a DALI button or the
Target Address number. The two automations that touch DALI use a light and the
occupancy sensor. See `project_log.md`. A later need for scenes or raw frames
from an HA script is runtime, not commissioning. It wants a small ESPHome
action, not the console back.

### Order of work

1. **The binding and a terminal page (3a). Built 2026-11-10.** What a client
   other than a browser can check passed on 1k and 2k on 2026-11-11.
   `dali_shell_web.cpp`, the `select()` loop in `dali_shell_tcp.cpp`, the
   `web:` schema, `dali_test.yaml`, and `web/shell.html`. See Part 1, *The web
   shell* and *The TCP binding after the `select()` rewrite*, and
   `project_log.md`. What it settled beyond the sketch above:
   - **Web connections that are not the session wait in two slots,** read
     without blocking from the same `select()`. A head must arrive within 10 s,
     and a third connection evicts the oldest. So a browser's idle preconnect
     cannot hold the task.
   - **An answered connection is shut and drained,** up to 2 s, so that
     closing it does not reset the answer.
   - **An upgrade with bytes after its head is refused** as malformed: a
     client sends nothing until the handshake answers.
   - **The heap floor is 16 kB,** a reserve; see *Budget*.
   - **Output bytes above 0x7F go out as `?`,** so a flush that falls anywhere
     is still a valid UTF-8 text frame. Shell output is ASCII today.
   - **The TCP listener is not watched while any session is open,** so a
     second TCP client waits in the backlog as before.
2. **Views (3b). Built 2026-11-10.** On 2026-11-11 the operator found Help
   and Map working on both nodes, and the Map's data matched both buses.
   Help from
   `schema` plus `dali_commands.md`, and a map from `export inventory`. See
   Part 1, *The web shell*, and `project_log.md`. What it settled:
   - **Two small C additions, approved with it.** `schema` flags each special
     `commissioning`, and `export inventory` (version 3) lists
     `contested_gear` and `undecodable_device`, with `identity_collision` on
     an entry. Version 2 left every undecodable address out, so a map built on
     it would have shown a shared address as free.
   - **The page runs its own lines quietly** through the same one-at-a-time
     queue as typed ones. A quiet line's output is what follows its echo, so a
     trace line pumped between lines still reaches the terminal.
   - **The reference is built into the page,** as a JS string, not served
     from a second path. `dali_web` stays two routes, and the help is the
     firmware's own. The cost is a 47 kB page on every load.
   - **A verb's Reference button finds its section** by the reference's own
     convention: the first `##` section whose opening code block has a line
     starting with the verb. Every verb but `devinfo` resolved, and
     `devinfo` is now documented.
   - **Actions on the map run as typed lines.** Identify, Status and Instances
     send `identify aN`, `status aN` and `instances N`. Use and the table names
     only fill the command line.
3. **Stop, in both bindings.** See *There is no cancel* under *Transport*.
   Proposed as its own change; not yet approved.
   - **Settle the signal first.** Over TCP it can be 0x03, which
     `tools/dali-shell` would send on Ctrl-C. A WebSocket text frame must be
     printable ASCII and a page cannot send a ping, so the web binding needs
     a reserved frame.
   - **Fold in a forgotten tab.** A tab left open holds the one session
     until `idle_timeout`, and meanwhile a TCP client waits unanswered in the
     backlog. The page could give the session back once its tab has been
     hidden for a minute, and reconnect when it is shown again.
   - **Treat a close frame as a stop too.** The binding reads nothing while a
     verb runs. On 2026-11-11 a close frame sent 8 s into a 1k `discover`
     waited until the walk had run its full 44.7 s. The same peek that reads
     the stop signal should act on a close frame.
4. **The content.**
   - The frame decoder and the live log.
   - Parameter and vendor metadata in `schema`.
   - `export device`.
   - Device panels.
   - Read-back in the write verbs.
5. **Remove the Home Assistant scheme** at the next major tag, with the
   migration note. Then rewrite the fallback sections of `dali-starter.yaml`
   and `commissioning_readme.md`.

Each step that lands updates `CHANGELOG.md` with its verification label.
`dali_commands.md` and `current_status.md` follow for any new verb or
operator-visible behaviour, and Part 1 gains an item for each expectation no
bus has confirmed.
