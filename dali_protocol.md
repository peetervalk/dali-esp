# DALI Protocol Reference

> **Spelling.** DALI command names follow IEC 62386, which spells them
> **INITIALISE** and **RANDOMISE** — so those are the spellings used here, in
> the command tables, the CLI verbs (`special initialise`, `special randomise`),
> and the C identifiers that name them. Everything else in this project is
> American English: `tokenize`, `recognize`, `quantize`, and ordinary software
> initialization. The split is deliberate — a command name you cannot grep the
> standard for is worth less than internal consistency of dialect.

Frame layouts, opcode tables, and standard behaviour, mapped to what this
project implements. It is not a replacement for IEC 62386.

**Last reviewed:** 2026-08-25

For the verbs that send these frames, see `dali_commands.md`. For what has been
run against real gear, see `dali_capability_matrix.md`.

## Frame Encoding

### Standard 16-bit forward frame

```text
data = (address_byte << 8) | command_or_level_byte
bit_length = 16
```

| Target | Selector | Address byte |
|---|---:|---|
| Short DAPC | 0 | `(short_addr << 1) \| 0` |
| Short command | 1 | `(short_addr << 1) \| 1` |
| Group DAPC | 0 | `0x80 \| (group << 1) \| 0` |
| Group command | 1 | `0x80 \| (group << 1) \| 1` |
| Broadcast DAPC | 0 | `0xFE` |
| Broadcast command | 1 | `0xFF` |

Selector bit `0` selects DAPC; the second byte is then the requested arc power
level `0x00..0xFE`. `0xFF` is MASK — leave the level unchanged — and is built by
a separate path so that no level arithmetic can arrive at it accidentally.

### DALI-2 24-bit input-device frame

```text
data = (device_address_byte << 16) | (instance_byte << 8) | command_or_event
bit_length = 24
```

For short-addressed input devices, `device_address_byte = (short_addr << 1) | 1`.
Instance byte `0x00..0x1F` addresses one instance, `0xFE` addresses device-level
commands, and `0xFF` addresses all instances where supported.

A control gear and a control device may share a short address: they answer
different frame widths. The 16-bit form reaches the gear, the 24-bit form the
device.

### Send-twice and local frame gaps

Configuration commands must arrive twice inside 100 ms. The scheduler owns that
window: it keeps the local pair adjacent, waits the local forward-frame gap, and
brackets the pair from before the first blocking PHY call until after the second.
When both PHY calls succeed it reports `DALI_ERR_TIMING` if that interval
exceeded 100 ms; a PHY error takes precedence. The pre-call check avoids starting
a repeat with no remaining budget, but if the second call itself crosses the
deadline the late frame may already be on the wire before the error is reported.

Two separately typed commands cannot meet this window, which is why `raw2`
exists and why every multi-frame helper runs as one scheduler sequence rather
than as lines a user is expected to type quickly.

All locally generated forward frames share the same rounded 22 Te minimum gap.
This is not DALI-2 priority/backoff or multi-master arbitration: the scheduler
cannot prove that an external command did not intervene. The PHY now exports the
precise ISR timestamp at which its own transmission released the bus, but the
scheduler does not yet derive backward/external-frame gaps or full arbitration
state from externally received frames.

### Timestamped RX observations and reply outcomes

The receive ISR still follows the buffer-first rule: it records edges into the
fixed ring, and task context decodes them. The decoder now reports an observation
containing the decode result, edge count, and first/last edge timestamps. Those
timestamps are a wrapping 32-bit microsecond clock quantized to 2 us. A successful
local transmission separately records its precise end timestamp in the TX ISR.
Unsigned deltas make attribution wrap-safe; task scheduling latency does not move
an observation into or out of a reply window.

For a transaction that expects a backward reply, the whole observation must be
attributable to a window measured from that precise TX end: its first edge
cannot precede the opening and its last edge cannot exceed the 27,000 us
closing. The opening is 5,500 us for undecodable activity and 2,000 us for an
observation that decoded as a complete backward frame; see Bus Timing for why
the two differ. The outcomes are deliberately three-way rather than treating
every decode failure as either a reply or silence:

| Observation in the attributed window | Scheduler result | Meaning |
|---|---|---|
| Valid 8-bit backward frame | `DALI_OK` | Decoded reply |
| Qualified, response-like malformed waveform | `DALI_ERR_RX_ACTIVITY` | Something answered, but its byte could not be decoded |
| Malformed activity that does not meet the response-like qualification, or RX overflow | The corresponding malformed/overflow error | Ambiguous observation; abort rather than inventing an answer |
| No observation | `DALI_ERR_TIMEOUT` | Silence |
| Valid 16- or 24-bit forward frame | Intervention error | Another forward transaction occupied the reply window |

`DALI_ERR_RX_ACTIVITY` is not a general yes response. Only the Part 102 `COMPARE`
operation maps it to YES, because overlapping affirmative backward replies may be
undecodable; silence maps to NO. `VERIFY SHORT ADDRESS`, ordinary queries, and all
other callers preserve it as an error.

The short-address scan is the one caller that neither maps it to an answer nor
aborts. IEC 62386 defines no scan of short addresses 0-63 — the standard's own
device-finding procedure is the random-address binary search, which is
collision-tolerant by construction — so the walk is this project's convention and
its handling of an undecodable address is a local design decision. It follows the
rule the standard applies to answers elsewhere: an invalid answer is neither a
value nor a "no". Such an address is recorded as `has_undecodable_activity`,
counted in `undecodable_count`, and left `present = false`:

- It is **not** reported as a discovered device, because nothing is known about it.
- It is **not** reported free. `dali_commissioning_used_mask_from_inventory()`
  reserves it, so a commissioning run cannot assign a further device onto a
  contested address.
- It does **not** abort the walk. One contested address must not cost the other
  sixty-three, and aborting would take the ESPHome boot scan and the `commission`
  pre-scan down with it.

Gear sharing a short address is the expected cause. The classification itself is
still awaiting the physical collision captures noted below. The classification has host coverage, but
the response-like thresholds and overlapping-reply behaviour have not yet been
validated with physical collision captures or hardware-in-the-loop tests.

## Control Gear — IEC 62386-102

### Direct arc power

| Code | Meaning |
|---:|---|
| `0x00..0xFE` | DAPC level, selected by address selector bit `0` |

### Output-level instructions

| Opcode | Name |
|---:|---|
| `0x00` | OFF |
| `0x01` | UP |
| `0x02` | DOWN |
| `0x03` | STEP UP |
| `0x04` | STEP DOWN |
| `0x05` | RECALL MAX LEVEL |
| `0x06` | RECALL MIN LEVEL |
| `0x07` | STEP DOWN AND OFF |
| `0x08` | ON AND STEP UP |
| `0x09` | ENABLE DAPC SEQUENCE |
| `0x0A` | GO TO LAST ACTIVE LEVEL |
| `0x0B` | CONTINUOUS UP (62386-102:2022) |
| `0x0C` | CONTINUOUS DOWN (62386-102:2022) |
| `0x10..0x1F` | GO TO SCENE 0..15 |

`UP`/`DOWN` perform one fade step; `CONTINUOUS UP`/`DOWN` fade toward max or min
until a stop condition.

### Configuration instructions

All addressed configuration commands require send-twice scheduling. DTR0-consuming
commands either use whatever DTR0 already holds or a sequence that loads it first.

| Opcode/range | Name |
|---:|---|
| `0x20` | RESET |
| `0x21` | STORE ACTUAL LEVEL IN DTR0 |
| `0x22` | SAVE PERSISTENT VARIABLES |
| `0x23` | SET OPERATING MODE DTR0 |
| `0x24` | RESET MEMORY BANK DTR0 |
| `0x25` | IDENTIFY DEVICE |
| `0x2A..0x30` | SET MAX/MIN/FAILURE/POWER-ON/FADE/EXTENDED-FADE from DTR0 |
| `0x40..0x4F` | SET SCENE 0..15 DTR0 |
| `0x50..0x5F` | REMOVE FROM SCENE 0..15 |
| `0x60..0x6F` | ADD TO GROUP 0..15 |
| `0x70..0x7F` | REMOVE FROM GROUP 0..15 |
| `0x80` | SET SHORT ADDRESS DTR0 |
| `0x81` | ENABLE WRITE MEMORY |

### Status byte

`QUERY STATUS` returns one byte; the eight flags are decoded on both surfaces.

| Bit | Flag | Meaning |
|---:|---|---|
| 0 | `ballast-fail` | Ballast failure |
| 1 | `lamp-fail` | Lamp failure |
| 2 | `arc-on` | Lamp arc power is on |
| 3 | `limit-err` | Requested level was outside the min/max window |
| 4 | `fading` | A fade is running |
| 5 | `reset` | Gear is in its reset state |
| 6 | `no-addr` | No short address programmed |
| 7 | `power-fail` | Power failure since the last reset |

### Device-type discovery

`QUERY DEVICE TYPE` returns `0x00..0xFD` for a single type, `0xFE` when no device
type is implemented, or `0xFF` (MASK) when several are. Only the MASK result
starts `QUERY NEXT DEVICE TYPE` enumeration; each next query returns a strictly
increasing type value, and `0xFE` or no reply ends the list. The discovery
inventory retains up to four types and marks the result truncated if it sees a
fifth.

### Memory bank 0 identity

The Part 102 helper reads control-gear Bank 0 locations `0x03..0x14`: GTIN,
firmware version, the eight-byte identification number, and hardware version.
Location `0x01` is reserved and is not read; `0x02` reports the last accessible
bank and is not identity data. Bank 1 is reachable through generic byte reads but
has no common parser — its contents are optional OEM data.

`READ MEMORY LOCATION` auto-increments the device's offset cursor, so a block
read must stay atomic: an interrupted and retried read returns the bytes after
the ones asked for. Memory transactions are not yet protected by a
scheduler-level session, and this layout correction is not hardware-verified.

## Special Commands

Special commands are not addressed in the normal way; every device on the bus
sees them.

| Opcode | Name | Notes |
|---:|---|---|
| `0xA1` | TERMINATE | Closes an initialise window |
| `0xA3` | DTR0 DATA | |
| `0xA5` | INITIALISE | Send twice |
| `0xA7` | RANDOMISE | Send twice |
| `0xA9` | COMPARE | Commissioning walk |
| `0xAB` | WITHDRAW | Commissioning walk |
| `0xAD` | PING | |
| `0xB1`/`0xB3`/`0xB5` | SEARCH ADDRH/M/L | Commissioning walk |
| `0xB7` | PROGRAM SHORT ADDRESS | Commissioning walk |
| `0xB9` | VERIFY SHORT ADDRESS | |
| `0xBB` | QUERY SHORT ADDRESS | |
| `0xC1` | ENABLE DEVICE TYPE | Applies to the next frame only |
| `0xC3` | DTR1 DATA | |
| `0xC5` | DTR2 DATA | |
| `0xC7` | WRITE MEMORY LOCATION | |
| `0xC9` | WRITE MEMORY LOCATION NO REPLY | |

`ENABLE DEVICE TYPE` qualifies only the frame immediately after it. Any
device-type-specific command must therefore travel with its enable in one
sequence — two separately issued lines are not adjacent on the bus.

Opcode `0xC1` is reused across parts: here it is a Part 102 special opcode, and
as the first byte of a 24-bit frame it is the Part 103 special-command address.
See Cross-Part Interference.

## Control Devices — IEC 62386-103

Input devices use instance types, not control-gear DT numbers: Part 301 defines
push-button instance type 1, Part 303 occupancy type 3, and Part 304 light-sensor
type 4.

Commands marked "twice" require two identical addressed frames inside the
send-twice window. A DTR load is a separate 24-bit special command and is not
itself duplicated.

### Generic instance configuration

| Opcode | Command | DTR | Sends |
|---:|---|---|---:|
| `0x61` | SET EVENT PRIORITY | DTR0 | twice |
| `0x62` | ENABLE INSTANCE | none | twice |
| `0x63` | DISABLE INSTANCE | none | twice |
| `0x64` | SET PRIMARY INSTANCE GROUP | DTR0 | twice |
| `0x65` | SET INSTANCE GROUP 1 | DTR0 | twice |
| `0x66` | SET INSTANCE GROUP 2 | DTR0 | twice |
| `0x67` | SET EVENT SCHEME | DTR0 | twice |
| `0x68` | SET EVENT FILTER | DTR2:DTR1:DTR0 | twice |
| `0x69` | SET INSTANCE TYPE | DTR0 | twice |
| `0x6A` | SET INSTANCE CONFIGURATION | DTR0 and DTR2:DTR1 | twice |

Opcodes `0x6E`, `0x6F`, and `0x70` are reserved in Part 103:2022. They are not
generic report-timer, hysteresis, or deadtime setters.

### Generic instance queries

Single-send commands expecting an 8-bit backward frame. Queries addressed to the
device rather than to one of its instances are under Device-level commands below.

| Instance byte | Opcode | Query |
|---:|---:|---|
| instance | `0x80` | QUERY INSTANCE TYPE |
| instance | `0x81` | QUERY RESOLUTION |
| instance | `0x82` | QUERY INSTANCE ERROR |
| instance | `0x83` | QUERY INSTANCE STATUS |
| instance | `0x84` | QUERY EVENT PRIORITY |
| instance | `0x86` | QUERY INSTANCE ENABLED |
| instance | `0x88` | QUERY PRIMARY INSTANCE GROUP |
| instance | `0x89` | QUERY INSTANCE GROUP 1 |
| instance | `0x8A` | QUERY INSTANCE GROUP 2 |
| instance | `0x8B` | QUERY EVENT SCHEME |
| instance | `0x8C` | QUERY INPUT VALUE |
| instance | `0x8D` | QUERY INPUT VALUE LATCH |
| feature selector | `0x8E` | QUERY FEATURE TYPE — not implemented |
| feature selector | `0x8F` | QUERY NEXT FEATURE TYPE — not implemented |
| instance | `0x90` | QUERY EVENT FILTER 0-7 |
| instance | `0x91` | QUERY EVENT FILTER 8-15 |
| instance | `0x92` | QUERY EVENT FILTER 16-23 |
| instance | `0x93` | QUERY INSTANCE CONFIGURATION — DTR0 selects, DTR2:DTR1 hold the result |
| instance | `0x94` | QUERY AVAILABLE INSTANCE TYPES — DTR2:DTR1:DTR0 complete the 32-bit result |


Feature queries stay unexposed until the addressing layer can encode Part 103
feature selectors. The `0x93` and `0x94` builders construct only the addressed
query frame; a complete typed operation must keep the query and its dependent DTR
reads atomic.

### Device-level commands

Instance byte `0xFE` addresses the control device itself rather than one of its
instances. Configuration commands at this level are send-twice; queries are
single-send.

| Opcode | Command | Sends |
|---:|---|---|
| `0x10` | RESET — not implemented | twice |
| `0x14` | SET SHORT ADDRESS DTR0 — not implemented | twice |
| `0x15` | ENABLE WRITE MEMORY — not implemented | twice |
| `0x1D` | START QUIESCENT MODE | twice |
| `0x1E` | STOP QUIESCENT MODE | twice |
| `0x30` | QUERY DEVICE STATUS — not implemented | reply |
| `0x35` | QUERY NUMBER OF INSTANCES | reply |
| `0x36`/`0x37`/`0x38` | QUERY CONTENT DTR0/DTR1/DTR2 | reply |

Quiescent mode suppresses a control device's own bus activity — event frames
above all — until `STOP QUIESCENT MODE` releases it. It matters beyond
commissioning: any operation wanting a quiet bus, such as a scan, a memory block
read, or DT8 bring-up, competes with sensor traffic otherwise. Espressif's
`esp_dali` records that some Part 102 gear mis-decode 24-bit event frames as DAPC
brightness commands, which would make an unquiesced sensor a visible fault rather
than only noise. Unverified here.

Device address byte `0xFF` addresses all control devices.
`dali_build_device_broadcast_command()` builds that form;
`dali_build_device_command()` still takes a short address and still rejects
anything at or above 64, because 0xFF is an address *byte*, not an address.
Both accept any 24-bit device command, queries included — a broadcast query
collides by construction, which the operator surfaces warn about rather than the
builder forbidding it.

`quiescent on|off <addr|all>` is the verb on both front ends. Host-tested;
no bus has run it.

### Part 103 special commands

Part 103 has its own special-command space, distinct from the Part 102 specials
above. The frame is 24-bit with a fixed first byte, the opcode in the second, and
the parameter in the third:

```text
data = (0xC1 << 16) | (special_opcode << 8) | parameter
```

| Opcode | Name | Notes |
|---:|---|---|
| `0x00` | TERMINATE | Closes a Part 103 initialise window |
| `0x01` | INITIALISE | Send twice; parameter is a device address byte |
| `0x02` | RANDOMISE | Send twice |
| `0x03` | COMPARE | |
| `0x04` | WITHDRAW | |
| `0x05`/`0x06`/`0x07` | SEARCH ADDRH/M/L | |
| `0x08` | PROGRAM SHORT ADDRESS | Raw 6-bit address |
| `0x09` | VERIFY SHORT ADDRESS | |
| `0x0A` | QUERY SHORT ADDRESS | |
| `0x20` | WRITE MEMORY LOCATION | |
| `0x30`/`0x31`/`0x32` | DTR0/DTR1/DTR2 DATA | |

None of this is implemented. The command table has no 24-bit special frame kind,
so control-device commissioning has no path here at all.

Two encodings differ from their Part 102 namesakes and are worth stating plainly,
because using the Part 102 form silently does the wrong thing:

- `PROGRAM SHORT ADDRESS` takes the **raw 6-bit address** `0..63`. The Part 102
  special of the same name takes `(short_addr << 1) | 1`. Sending the Part 102
  encoding to a control device programs address `2n + 1`.
- `INITIALISE`'s parameter is a device address byte, where `0xFF` selects all
  control devices and `0x00` selects only those without a short address. This
  reads inverted against Part 102, whose `INITIALISE` takes `0x00` for all gear
  and `0xFF` for unaddressed gear.

Opcode values are transcribed from Espressif's `esp_dali` and are not verified
against the standard text or on a bus here.

### Part 301 push button — instance type 1

| Opcode | Command | DTR | Sends |
|---:|---|---|---|
| `0x00` | SET SHORT TIMER | DTR0 | twice |
| `0x01` | SET DOUBLE TIMER | DTR0 | twice |
| `0x02` | SET REPEAT TIMER | DTR0 | twice |
| `0x03` | SET STUCK TIMER | DTR0 | twice |
| `0x0A` | QUERY SHORT TIMER | none | reply |
| `0x0B` | QUERY SHORT TIMER MIN | none | reply |
| `0x0C` | QUERY DOUBLE TIMER | none | reply |
| `0x0D` | QUERY DOUBLE TIMER MIN | none | reply |
| `0x0E` | QUERY REPEAT TIMER | none | reply |
| `0x0F` | QUERY STUCK TIMER | none | reply |

Short, double, and repeat timers use 20 ms units; the stuck timer uses 1 s units.
A device may reject a value below its own reported physical minimum even when the
value is inside the standard-wide range.

### Part 303 occupancy sensor — instance type 3

| Opcode | Command | DTR | Sends |
|---:|---|---|---|
| `0x20` | CATCH MOVEMENT | none | once |
| `0x21` | SET HOLD TIMER | DTR0 | twice |
| `0x22` | SET REPORT TIMER | DTR0 | twice |
| `0x23` | SET DEADTIME TIMER | DTR0 | twice |
| `0x24` | CANCEL HOLD TIMER | none | once |
| `0x25` | SET DETECTION RANGE | DTR0 | twice |
| `0x26` | SET SENSITIVITY | DTR0 | twice |
| `0x29` | QUERY INSTANCE CAPABILITIES | none | reply |
| `0x2A` | QUERY DETECTION RANGE | none | reply |
| `0x2B` | QUERY SENSITIVITY | none | reply |
| `0x2C` | QUERY DEADTIME TIMER | none | reply |
| `0x2D` | QUERY HOLD TIMER | none | reply |
| `0x2E` | QUERY REPORT TIMER | none | reply |
| `0x2F` | QUERY CATCHING | none | reply |

Hold, report, and deadtime use 10 s, 1 s, and 50 ms units respectively. Opcodes
`0x25`, `0x26`, and `0x29..0x2B` are the 62386-303:2017/AMD1:2024 additions and
can be capability-dependent.

Occupancy output is bit-replicated: `0` unoccupied, `85` movement, `170` present,
`255` present and moving.

### Part 304 light sensor — instance type 4

| Opcode | Command | DTR | Sends |
|---:|---|---|---|
| `0x30` | SET REPORT TIMER | DTR0 | twice |
| `0x31` | SET HYSTERESIS | DTR0 | twice |
| `0x32` | SET DEADTIME TIMER | DTR0 | twice |
| `0x33` | SET HYSTERESIS MIN | DTR0 | twice |
| `0x3C` | QUERY HYSTERESIS MIN | none | reply |
| `0x3D` | QUERY DEADTIME TIMER | none | reply |
| `0x3E` | QUERY REPORT TIMER | none | reply |
| `0x3F` | QUERY HYSTERESIS | none | reply |

Report and deadtime use 1 s and 50 ms units. Hysteresis is a percentage `0..25`;
`hysteresisMin` is the absolute minimum band height in input-value units, not a
generic deadband.

## Cross-Part Interference

Control gear and control devices share one bus, and each part's commissioning
sequence can disturb the other's. Timestamped reply attribution does not solve
this on its own. Gear commissioning now brackets itself with broadcast
`START`/`STOP QUIESCENT MODE`, which stops a conforming control device from
transmitting into a COMPARE reply window; the same commands are available by hand
as the `quiescent` verb. What that does not cover: a device that never received
the broadcast, and the reverse direction below. It is recorded because it is a
plausible explanation for phantom devices in a mixed-installation binary search,
and because it is separate from the COMPARE collision problem rather than another
face of it. Host-tested only.

`0xC1` means two things depending on frame width. As a Part 102 special opcode it
is `ENABLE DEVICE TYPE`; as the first byte of a 24-bit frame it is the Part 103
special-command address. Gear sitting in an initialise window can therefore act
on the Part 103 special frames that a control-device commissioning run emits.

The reverse also holds. A control device that observes a Part 102 `INITIALISE`
can enter its own addressing state, generate a random address, and answer Part
102 `COMPARE` — appearing in the search as gear that is not there.

Espressif's `esp_dali` brackets each part's commissioning with the other part's
`TERMINATE`, repeats that `TERMINATE` after `INITIALISE` because a device can
re-enter the state, and holds control devices in quiescent mode across the whole
run. Its comments name a Steinel DLS-203-P as the device that forced this
handling. Unverified here.

## Event Frames

`dali_event` keeps unsolicited frames raw-first. For Part 103 24-bit input-device
traffic it rejects command frames (`bit 16 = 1`), preserves all ten
event-information bits (`bits 9:0`), and decodes the five standard source schemes:

| Source scheme | Encoded source fields |
|---|---|
| Instance | instance type, instance number |
| Device | device short address, instance type |
| Device/Instance | device short address, instance number |
| Device Group | device-group number, instance type |
| Instance Group | instance-group number, instance type |

Device/Instance frames carry no instance type; it has to come from discovery or
configuration rather than be inferred from the event value. Power notifications
use the reserved Part 103 prefix and are represented as a separate frame kind.

Part 301 push-button event information uses sparse standard values:

| Value | Event |
|---:|---|
| `0x000` | released |
| `0x001` | pressed |
| `0x002` | short press |
| `0x005` | double press |
| `0x009` | long press start |
| `0x00B` | long press repeat |
| `0x00C` | long press stop |
| `0x00E` | button free |
| `0x00F` | button stuck |

### Legacy DALI-1 coupler frames

```text
data = (target_address_byte << 8) | command_or_level_byte
```

Legacy frames identify the target and action, not the source coupler or button.
Existing BF6 couplers use this mode successfully; the ESP32 observes the frames
and syncs Home Assistant state.

## Vendor Opcodes

Lunatone documents these for its DALI-2 sensor instances. They are 24-bit
instance frames but not generic Part 103 commands, which is why they stay out of
`dali_protocol` metadata and live in `dali_lunatone`. Untested on hardware here.

| Opcode | Name | Response |
|---:|---|---|
| `0x40` | LUNATONE QUERY VALUE MULTIPLICATOR | `UINT8` |
| `0x41` | LUNATONE QUERY VALUE DIVISOR | `UINT8` |
| `0x42` | LUNATONE QUERY OFFSET MSB | `UINT8` |
| `0x43` | LUNATONE QUERY OFFSET LSB | `UINT8` |
| `0x44` | LUNATONE QUERY OFFSET MULTIPLICATOR | `UINT8` |
| `0x45` | LUNATONE QUERY OFFSET DIVISOR | `UINT8` |
| `0x46` | LUNATONE QUERY UNIT | `UINT8` |

Steinel HF 360 II memory-bank layout and its tuning workflow are in
`steinel_bank2_reference.md`.

## Device Type Coverage

| DT | Standard | Status |
|---:|---|---|
| DT6 | IEC 62386-207 | Implemented (`dali_gear_dt6`) |
| DT8 | IEC 62386-209 | Implemented (`dali_gear_dt8`) |
| DT1 | IEC 62386-202 | Not implemented; add following the DT6/DT8 pattern |

## Bus Timing

| Parameter | Value |
|---|---|
| GPTIMER tick | 104 us |
| Bit period | ~833.3 us |
| Half-bit period | ~416.7 us |
| TX-to-RX settle suppression | 2 ms |
| Attribution opens — undecodable activity | 5.5 ms after precise local TX end |
| Attribution opens — decoded backward frame | 2 ms after precise local TX end |
| Timestamped reply attribution closes | 27 ms after precise local TX end |
| Scheduler reply wait after TX handoff | 25 ms |
| Send-twice window | 100 ms |
| Post-RANDOMISE settle, before the first COMPARE | 100 ms |

Every row above except the last is frame-level timing the PHY and scheduler
enforce. The 2 ms handoff suppression plus the 25 ms reply wait gives the precise
TX-end-relative 27 ms attribution close; observations are accepted by their
captured edge timestamps, not by when task context delivers them.

The open edge has been two values since 2026-08-25, and which one applies is
decided by whether the observation decoded. `DALI_REPLY_WINDOW_OPEN_US`
(5,500 us) is the standard's minimum settling time and guards *undecodable*
activity, because that is the path `COMPARE` reads as YES and where a wrong call
invents gear. `DALI_REPLY_WINDOW_OPEN_DECODED_US` is derived from
`DALI_SETTLE_MS` (2,000 us) and applies to a complete 8-bit backward frame,
which carries none of that ambiguity while a query is outstanding: the local
16-bit transmission cannot decode as one, ringing cannot, and another master's
forward frame is caught by the intervening-frame branch. What is left is the
PHY's own RX self-echo suppression. Deriving rather than choosing it is
deliberate — a 1k-site DT6 driver answers between 4.12 and 5.85 ms on
consecutive queries, so any hand-picked margin gets overtaken. The RANDOMISE
settle is a commissioning-sequence delay in
`dali_commissioning`, raised from 15 ms on 2026-08-24 to match the figure
Espressif's `esp_dali` attributes to IEC 62386-102 §11.3. Unconfirmed against the
standard text and unexercised on a bus since the change.

## Sources

- IEC 62386-103:2022 — https://webstore.iec.ch/en/publication/67776
- IEC 62386-301:2017 — https://webstore.iec.ch/en/publication/28605
- IEC 62386-303:2017+AMD1:2024 — https://webstore.iec.ch/en/publication/94036
- IEC 62386-304:2017+AMD1:2024 — https://webstore.iec.ch/en/publication/94037
- Microchip DALI command table —
  https://onlinedocs.microchip.com/oxy/GUID-0CDBB4BA-5972-4F58-98B2-3F0408F3E10B-en-US-1/GUID-DA5EBBA5-6A56-4135-AF78-FB1F780EF475.html
- Microchip TB3200 — https://ww1.microchip.com/downloads/en/Appnotes/90003200A.pdf
- Espressif DALI driver timing notes —
  https://docs.espressif.com/projects/esp-iot-solution/en/latest/electrical_lighting_solution/dali.html
- Espressif `esp_dali` component (Apache-2.0), source of the Part 103 device and
  special opcode values and the cross-part interference notes —
  https://github.com/espressif/esp-iot-solution/tree/master/components/dali
- Beckhoff DALI frame timing and send-twice —
  https://infosys.beckhoff.com/content/1033/tcplclib_tc3_dali/12346803211.html
- Beckhoff DALI-2 Query Input Value —
  https://infosys.beckhoff.com/content/1033/tcplclib_tc2_dali/4346134027.html
- Lunatone DALI-2 instance guide —
  https://www.lunatone.com/wp-content/uploads/2021/10/DALI-2_Instance-Guide_EN_M0024.pdf
- Lunatone DALI-2 sensor instances —
  https://www.lunatone.com/wp-content/uploads/2022/11/Lunatone_DALI-2_Sensor_Instances_EN_M0026.pdf
- Steinel DALI-2 interface description —
  https://www.steinel.de/out/media/interfacedoc/94546_DALI-2%20Interface%20Description_V1.5.pdf
