# DALI Protocol Reference

Frame layouts, opcode tables, and standard behaviour, mapped to what this
project implements. It is not a replacement for IEC 62386.

**Last reviewed:** 2026-08-14

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
cannot prove that an external command did not intervene, and it does not derive
backward/external-frame gaps from a PHY frame-end timestamp.

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
| `0xA7` | RANDOMIZE | Send twice |
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

Single-send commands expecting an 8-bit backward frame.

| Instance byte | Opcode | Query |
|---:|---:|---|
| `0xFE` | `0x35` | QUERY NUMBER OF INSTANCES |
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
| Reply window opens | 7 ms |
| Reply timeout | 25 ms |
| Send-twice window | 100 ms |

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
