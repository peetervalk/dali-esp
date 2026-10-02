# DALI-ESP To-Do

Everything still open, in two parts:

- **Verify**: what `current_status.md` describes but no real bus has confirmed
  yet. Each item gives the procedure that confirms it.
- **Develop**: known defects to fix, features to build, decisions to make, and
  the release process.

`current_status.md` describes what the code is built to do, without
verification labels. This file is where "not yet confirmed" lives.
`dali_capability_matrix.md` keeps the per-capability record, and
`project_log.md` keeps the dated evidence.

**When an item is done**, record the session in `project_log.md`: date, ref,
ESPHome version, bus, what ran, the results, and what was not covered. Then
update the matrix row and delete the item here, or cut it down to what
remains. Do not mark an item done in place. If the item backs a claim in
`CHANGELOG.md`'s Unreleased section, update that claim's label too.

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
  Only 0 and 1 emit events. d1 is the Casambi CBU-DCS, with 1 instance. A spare
  LED driver, the one that contested a4 on 2026-09-03, is normally
  disconnected.

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

**HTTP refusal**

1. From another machine:
   `curl --http0.9 -m 5 --data-binary $'x\r\nhelp\r\n' http://<host>:2323/`
2. Expected on the terminal: the banner, a `> ` prompt, then `HTTP request
   refused: this port is the DALI shell, not a web server.`, and no `help`
   output.
3. Expected in the device log, at WARN: `Closed a connection from <your
   address> that opened with an HTTP request; nothing it sent was run`, then
   `Shell session closed`.
4. **Browser.** Open the devtools console on a page served over plain http on
   the LAN, such as Home Assistant's own UI, and run:
   `fetch("http://<host>:2323/", {method: "POST", mode: "no-cors", body: "help\n"})`.
   The fetch reports a network error; that is expected. The device log should
   show the same WARN line, naming the browser's machine.

**Send timeout.** The device has to block on a send, so the client must not
read and must not offer a large receive buffer. A desktop client's default
buffer swallows the whole reply and never makes the device wait. Run this from
Linux, for example the Home Assistant SSH add-on or WSL:

```python
import socket, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)   # before connect()
s.connect(("<host>", 2323))
s.sendall(b"schema\r\n" * 5)    # far more output than both buffers hold
time.sleep(60)                  # never read
```

- **Expected:** `Shell session opened`, then `Shell session closed` about 10–12 s
  after the script starts, not minutes later. A client that connects right
  after that gets the banner at once.

**Keepalive**

1. Connect from a laptop on Wi-Fi with `python3 dali-shell` and leave it at the
   prompt.
2. Turn the laptop's Wi-Fi off without closing the client.
3. **Expected:** `Shell session closed` in the device log about 45 s after the
   last traffic: 30 s idle, then 3 probes 5 s apart. With the starter's
   `idle_timeout: 10min`, keepalive is what closed the session.
4. Connect from another machine. The banner should come at once.

**Second client**

1. With one session open, run `nc <host> 2323` from a second machine.
2. **Expected:** it connects and prints nothing. The banner arrives once the
   first session ends.

`python3 dali-shell --list` run from a third machine leaves this node out while
the first session is open. That is the known probe defect in *Fix*.

### ESPHome version range

- **Compile `dali_test.yaml` on 2026.9.1.** Only `esphome config` has run on it.
  - Run `pip install esphome==2026.9.1` in a venv.
  - Then run `python -m esphome compile dali_test.yaml`.
- **Establish the `manifest.json` floor of `>=2026.6.0`.** Do the same compile
  on 2026.6.0. If it fails, raise the floor to the oldest version that
  compiles, and record which one.

### `export config` validates

1. On a node that has run `discover`:
   `python3 dali-shell export config > block.yaml`.
2. Paste the block into a copy of that node's YAML, replacing its `dali:`,
   `light:` and `sensor:` blocks.
3. Run `python -m esphome config <copy>.yaml`.
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

---

## 2k sessions (control devices)

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
3. Run `capture start`, then `address d0 clear`, then `capture export`. Today
   `address d0 clear` answers `d0 does not answer; nothing to clear`.
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
to 8/8. But `rx_reply_late` reads 0, so the late-reply story in its comment
is not what happens, and the emitter-spacing candidate does not account for
it either. Background is in `project_log.md`, *Nothing spaces a forward frame
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

### P0 — Reserve the scheduler for a scan

The TCP shell's single-frame verbs do not observe the scan gate, so they can
interleave with a button scan. The quiescence check plus the component-wide
gate is not an atomic check-and-reserve. `dali_sched_is_quiescent()` no longer
reports quiescent between dequeue and `SCHED_TX`, and a claim now waits for
queued work to drain, but a true admission reservation is still missing.

### P0 — Confirm transmission for every scheduler client

Light entities act on completion. Three clients still report, or act on,
admission rather than transmission:

- the console's `OK`
- the refresh pump
- headless dispatch

### P0 — Space forward frames from received frames

Nothing spaces our forward frame from a received one. Ours follows a reply by
about 2.9 ms, and an event by as little as 1.7 ms. TI's DALI-2 device firmware
waits 13.5–19.5 ms after any frame, by priority, and DALI-1 wanted 22 Te after
a reply.

TI's SDK is a compact reference for:

- the settle table
- collision detection, avoidance and recovery

Its receiver accepts a send-twice pair whose stop conditions are up to 94 ms
apart, which is looser than the scheduler's own bracket.

Also open:

- DALI-2 priority and backoff
- a deadline-aware PHY call for when a repeat would cross the 100 ms
  send-twice limit

A 1k `restore apply` move lost a frame right after another unit's reply. That
fits, but one miss does not establish it. Measure first: *Bus timing with a
logic analyzer*, step 3, and the second-load counts.

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

Reproduce it first: *The GPIO ISR-service conflict*.

### P1 — `load_address_backup()` calls ESPHome preferences from the shell task

`ESP32PreferenceBackend::load()` walks the unlocked pending-save vector that
the loop task's `save()` and `sync()` mutate. That is a data race. Fix: load
once in `setup()`, and serve from `s_address_backup`.

### P1 — Entity limits fail silently

The 33rd light, 17th sensor and 33rd dispatch rule are dropped with only a
boot-time log line. An unregistered light never gets a profile and can never
transmit. Fix: reject over-limit configs in the schema, and size the light
registry for 64 + 16 + 1.

### P1 — Core affinity: the comments are wrong, and the PHY busy-waits on the loop's core

ESPHome 2026.9 pins `loopTask` to core 1, the core `dali_worker_core()` also
gives the DALI, scan and shell tasks. Synchronization is unaffected, but some
text is now wrong:

- every "Core 0" comment
- the rationale in `dali_core_affinity.h`
- the "preferences API is Core 0 only" comment in `dali_component.cpp`

Worse, the PHY's pre-TX busy-wait runs on the loop's core: 1.67 ms per frame,
and 33 ms per attempt on a stuck bus. Fix the comments, and let the idle check
use an edge time the ISR records.

### P1 — One intervening frame aborts a whole scan

`discovery_scan_walk()` returns on any per-address error other than TIMEOUT,
MALFORMED or RX_ACTIVITY. So a coupler button press during a reply window ends
a 45 s walk. Fix: retry the address once, then record it as unreadable and
reserved.

### P1 — `restore_find_spare()` can stage onto a missing unit's recorded address

If that unit is only unpowered, it comes back onto a contested address. Fix:
prefer spares outside `dali_snapshot_used_mask()`, and add a host vector.

### P1 — Report a failed input-device enumeration

The post-scan loop in `cmd_discover` prints only on `DALI_OK` from
`dali_discovery_query_input_device()`, and has no else branch. A lost query
leaves a device line claiming N instances, no enumeration line, and no error.
This has been seen on hardware. Fix: report the failure, and keep the count
the device line already gave.

### P1 — Clearing a shared device address

When two control devices share an address, `address d<N> clear` can read the
collision as silence and refuse with `does not answer`. Settle the mechanism
first with *`address d<N> clear` at a shared device address: capture the
collision*. There are two candidates:

- The merged reply falls outside the window and is dropped as noise.
- A MALFORMED first attempt is retried, and the retry's silence replaces it.

Every query shares that path, so do not change the scheduler before the
capture.

Two shell messages are stale meanwhile. `backup save` and the restore planner
print `nothing here de-addresses a control device, so a contested d<N> needs a
hardware pass` (`dali_shell.c`), although `address d<N> clear` exists. They
should name the by-hand clear and its caveat:

- device DTR0 `0xFF`
- then `raw2 <addr>FE14 len=24`

### P1 — `tools/dali-shell` misses a node whose shell is in use

`probe_shell()` expects a busy notice that the TCP binding never sends. A
second connection waits in the listen backlog instead. So `--list`, and
discovery without `--host`, leave out a node with a session open. Fix: treat
a connection that is accepted but silent as "in use", and list the node with
that note.

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

### Before tagging v3.0.0

- **Pin the tag in the tree being tagged.** At `v2.0.0`, `dali-starter.yaml`
  and the README still pinned `v1.3.0`; `7f26fef` fixed that after tagging.
  - Bump the pins in the release-prep commit.
  - Make `release-packaging.yml` fail when a tag's starter config names a
    different ref.
- **Update the comments that describe `dev`-only behaviour.** The
  `idle_timeout` comments in `dali-starter.yaml` ("closed without quitting",
  "a lost session locking out the shell until reboot") and in the README
  example ("a terminal that dropped") describe `v2.0.0`. They go stale the
  moment the pins move.
- **Check `CHANGELOG.md`'s Unreleased section against the tag.** Check each
  claim, not the heading: `git grep <symbol> v2.0.0` settles it in seconds.
  Re-check every verification label against `project_log.md`, then rename the
  heading to the version.
  - The list for v2.0.0 had not been emptied at v1.3.0, so it claimed two
    releases' worth of breaks as new.
- **Re-measure the README's memory table.** It dates from 2026-09-04.
  - Compile `dali_test.yaml` with and without its `shell:` block.
  - Then update the RAM line in the hardware table.
- **Bump `manifest.json`** if the version range item above moved the floor.

### Standing

- **Enforce, or document, the real ESP-IDF and ESPHome version requirements.**
  `idf-build.yml` pins IDF 6.0.1, so the native requirement fails as soon as it
  stops holding. The ESPHome side is still advisory: CI installs whatever `pip
  install esphome` resolves to. The two builds compile the same
  `components/dali` C with different toolchains.
- **Keep local filenames hyphenated in all documentation.** Re-check
  `dali_commands.md`, which tracks both verb surfaces, whenever either table
  changes.
- **Complete release provenance:** project SPDX identifiers, and the full
  vendored Unity MIT license and third-party notice.
