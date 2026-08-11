# DALI Command Reference

This is the project command/protocol reference for the native CLI, ESPHome
command console, and reusable protocol builders. It is not a replacement for
IEC 62386.

**Last reviewed:** 2026-08-10

## Current Implementation Status

- Standard control-gear addressing, DAPC, output commands, queries, and
  configuration commands are implemented in `dali_protocol` and `dali_control`.
- DTR0/DTR1/DTR2 DATA builders and DTR0-consuming configuration sequences are
  implemented.
- Commissioning, memory-bank reads, DT6 helpers, DT8 helpers, input-device
  queries, input polling, and input-device configuration frame builders are
  implemented.
- The input-configuration opcode surface has been independently audited against
  the current Part 103, 301, 303, and 304 command tables. Real-bus configuration
  writes remain unverified: read the current value first, write one parameter,
  then read it back before broader use.
- ESPHome command console supports the practical subset needed from HA:
  `off`, `max`, `min`, `level`, `query`, `config`, `iquery`, and `iconfig`.
- Active code gaps and cleanup work live in `current_status.md`.

## Sources

- IEC 62386-103:2022 publication record:
  https://webstore.iec.ch/en/publication/67776
- IEC 62386-301:2017 publication record:
  https://webstore.iec.ch/en/publication/28605
- IEC 62386-303:2017+AMD1:2024 consolidated publication record:
  https://webstore.iec.ch/en/publication/94036
- IEC 62386-304:2017+AMD1:2024 consolidated publication record:
  https://webstore.iec.ch/en/publication/94037
- Microchip DALI command table:
  https://onlinedocs.microchip.com/oxy/GUID-0CDBB4BA-5972-4F58-98B2-3F0408F3E10B-en-US-1/GUID-DA5EBBA5-6A56-4135-AF78-FB1F780EF475.html
- Microchip TB3200:
  https://ww1.microchip.com/downloads/en/Appnotes/90003200A.pdf
- Espressif DALI driver timing notes:
  https://docs.espressif.com/projects/esp-iot-solution/en/latest/electrical_lighting_solution/dali.html
- Beckhoff DALI frame timing and send-twice notes:
  https://infosys.beckhoff.com/content/1033/tcplclib_tc3_dali/12346803211.html
- Beckhoff DALI-2 Query Input Value:
  https://infosys.beckhoff.com/content/1033/tcplclib_tc2_dali/4346134027.html
- Lunatone DALI-2 instance guide:
  https://www.lunatone.com/wp-content/uploads/2021/10/DALI-2_Instance-Guide_EN_M0024.pdf
- Lunatone DALI-2 sensor instances:
  https://www.lunatone.com/wp-content/uploads/2022/11/Lunatone_DALI-2_Sensor_Instances_EN_M0026.pdf
- Steinel DALI-2 interface description:
  https://www.steinel.de/out/media/interfacedoc/94546_DALI-2%20Interface%20Description_V1.5.pdf

## Target Syntax

Native CLI and ESPHome command console both use this target language where
supported:

| Syntax | Meaning |
|---|---|
| `a<N>` or `s<N>` | Short address `0..63` |
| `g<N>` | Group address `0..15` |
| `b` | Broadcast |
| `a<N>:<I>` | Input-device short address plus instance `0..31` |

Queries should normally target one short address. Group or broadcast queries can
create reply collisions if multiple devices answer.

## Frame Encoding

### Standard 16-bit forward frame

```text
data = (address_byte << 8) | command_or_level_byte
bit_length = 16
```

Address byte:

| Target | Selector | Address byte |
|---|---:|---|
| Short DAPC | 0 | `(short_addr << 1) | 0` |
| Short command | 1 | `(short_addr << 1) | 1` |
| Group DAPC | 0 | `0x80 | (group << 1) | 0` |
| Group command | 1 | `0x80 | (group << 1) | 1` |
| Broadcast DAPC | 0 | `0xFE` |
| Broadcast command | 1 | `0xFF` |

`DAPC` is selected by address selector bit `0`; the second byte is the requested
arc power level `0x00..0xFE`.

### DALI-2 24-bit input-device frame

```text
data = (device_address_byte << 16) | (instance_byte << 8) | command_or_event
bit_length = 24
```

For short-addressed input devices, `device_address_byte = (short_addr << 1) | 1`.
Instance byte `0x00..0x1F` addresses one instance, `0xFE` addresses device-level
commands, and `0xFF` addresses all instances where supported.

## Native CLI Reference

Important diagnostic commands:

```text
stats
queue [reset]
bus check
capture start|stop|clear|status|export
scan
discover
inventory
export inventory
identify <addr>
find switches [seconds]
events
instances <addr>
sensor poll <addr> [instance]
smoke <addr>
commission unaddressed [first-addr] [max-devices]
```

Control commands:

```text
off <target>
max <target>
min <target>
level <target> <0-254>
up <target>
down <target>
step-up <target>
step-down <target>
step-off <target>
on-step <target>
last <target>
scene <target> <0-15>
dapc-seq <target>
```

Query/config helpers:

```text
query-list
query <target> <query-name> [param]
config-list
config <target> <config-name> [param]
```

## ESPHome Command Console

Available through `text:` platform entity `DALI Command` when configured:
see `esphome_verb_readme.md` for the practical parameter guide.

```text
off <target>
max <target>
min <target>
level <target> <0-254>
raw <hex> len=<16|24> [wait]
raw2 <hex> len=<16|24> [wait]
query <target> <query-name>
config <target> <config-name> [dtr0]
iquery a<N>:<instance> <query-name>
iconfig a<N>:<instance> <config-name> [dtr0]
queue [reset]
```

`queue` is the only console verb that generates no bus traffic, so it is the
only one accepted while a scan is running. It reports queue depth, capacity,
high-water mark, and the cumulative accepted/refused submission counts, which
`stats` also includes on the native CLI. A refused submission is dropped work,
not deferred work; the same counters are logged whenever they advance.

For ESPHome `add-group` and `remove-group`, the final group value `0-15` is
required. Short and group targets are accepted; broadcast targets are rejected.

Common examples:

```text
query a0 actual-level
raw C13102 len=24
raw C13004 len=24
raw 01FE3C len=24 wait
raw2 01FE15 len=24
query a0 power-on-level
query a0 failure-level
config a0 set-max-dtr0 200
iquery a0:1 hold-timer
iconfig a0:1 set-hold-timer 20
```

`raw2` uses the scheduler send-twice path for commands that must be transmitted
twice inside the DALI timing window. It avoids relying on Home Assistant to send
the same text entity state twice. The scheduler keeps the local pair adjacent,
waits the local forward-frame gap, and conservatively brackets the pair from
before the first blocking PHY call until after the second. When both PHY calls
succeed, it reports `DALI_ERR_TIMING` if that interval exceeds 100 ms; a PHY
error takes precedence. The pre-call check avoids knowingly starting a repeat
with no remaining time budget. If the second PHY call itself crosses the
deadline, the late frame may already be on the wire before the error is reported.

All locally generated forward frames share the same rounded 22 Te minimum gap.
This is not full DALI-2 priority/backoff or multi-master arbitration. The
scheduler cannot yet prove that an external command did not intervene, and it
does not derive backward/external-frame gaps from a PHY frame-end timestamp.

## Control-Gear Command Groups

### Direct arc power control

| Code | Meaning | Status |
|---:|---|---|
| `0x00..0xFE` | DAPC level selected by address selector bit `0` | Implemented for short/group/broadcast |

### Output-level instructions

| Opcode | Name | Status |
|---:|---|---|
| `0x00` | OFF | Implemented |
| `0x01` | UP | Implemented |
| `0x02` | DOWN | Implemented |
| `0x03` | STEP UP | Implemented |
| `0x04` | STEP DOWN | Implemented |
| `0x05` | RECALL MAX LEVEL | Implemented |
| `0x06` | RECALL MIN LEVEL | Implemented |
| `0x07` | STEP DOWN AND OFF | Implemented |
| `0x08` | ON AND STEP UP | Implemented |
| `0x09` | ENABLE DAPC SEQUENCE | Implemented |
| `0x0A` | GO TO LAST ACTIVE LEVEL | Implemented |
| `0x10..0x1F` | GO TO SCENE 0..15 | Implemented |

### Configuration instructions

These addressed commands require send-twice scheduling. DTR0-consuming commands
can use an existing DTR0 value or a helper sequence that loads DTR0 first.

| Opcode/range | Name |
|---:|---|
| `0x20` | RESET |
| `0x21` | STORE ACTUAL LEVEL IN DTR0 |
| `0x22` | SAVE PERSISTENT VARIABLES |
| `0x23` | SET OPERATING MODE DTR0 |
| `0x24` | RESET MEMORY BANK DTR0 |
| `0x25` | IDENTIFY DEVICE |
| `0x2A..0x30` | SET MAX/MIN/FAILURE/POWER-ON/FADE/EXTENDED-FADE values from DTR0 |
| `0x40..0x4F` | SET SCENE 0..15 DTR0 |
| `0x50..0x5F` | REMOVE FROM SCENE 0..15 |
| `0x60..0x6F` | ADD TO GROUP 0..15 |
| `0x70..0x7F` | REMOVE FROM GROUP 0..15 |
| `0x80` | SET SHORT ADDRESS DTR0 |
| `0x81` | ENABLE WRITE MEMORY |

### Native CLI query names

Run `query-list` on the native CLI for the authoritative list. Common names:

| Query name | Underlying command |
|---|---|
| `status` | QUERY STATUS |
| `present` | QUERY CONTROL GEAR PRESENT |
| `lamp-failure` | QUERY LAMP FAILURE |
| `limit-error` | QUERY LIMIT ERROR |
| `reset-state` | QUERY RESET STATE |
| `missing-address` | QUERY MISSING SHORT ADDRESS |
| `version` | QUERY VERSION NUMBER |
| `dtr0`, `dtr1`, `dtr2` | QUERY CONTENT DTR0/DTR1/DTR2 |
| `device-type` | QUERY DEVICE TYPE |
| `physical-min` | QUERY PHYSICAL MINIMUM |
| `power-failure` | QUERY POWER FAILURE |
| `operating-mode` | QUERY OPERATING MODE |
| `light-source` | QUERY LIGHT SOURCE TYPE |
| `actual` | QUERY ACTUAL LEVEL |
| `max-level` | QUERY MAX LEVEL |
| `min-level` | QUERY MIN LEVEL |
| `power-on` | QUERY POWER ON LEVEL |
| `failure-level` | QUERY SYSTEM FAILURE LEVEL |
| `fade` | QUERY FADE TIME / FADE RATE |
| `next-device-type` | QUERY NEXT DEVICE TYPE |
| `scene-level <0-15>` | QUERY SCENE LEVEL |
| `groups-0-7`, `groups-8-15` | QUERY GROUPS |
| `random-h`, `random-m`, `random-l` | QUERY RANDOM ADDRESS |
| `memory` | READ MEMORY LOCATION |
| `extended-version` | QUERY EXTENDED VERSION NUMBER |

For control-gear device-type discovery, `QUERY DEVICE TYPE` returns `0x00..0xFD`
for one type, `0xFE` when no device type is implemented, or `0xFF` (MASK) when
multiple types are implemented. Only the MASK result starts `QUERY NEXT DEVICE
TYPE` enumeration. Each next query returns a strictly increasing type value;
`0xFE` or no reply ends the list. The reusable discovery inventory retains up to
four types and marks the result as truncated if it observes a fifth valid type.

The shared Part 102 memory helper reads control-gear Bank 0 identity locations
`0x03..0x14`: GTIN, firmware version, the eight-byte identification number, and
hardware version. Location `0x01` is reserved/not implemented and is not read;
location `0x02` reports the last accessible bank and is not identity data. Bank 1
remains available through generic byte reads but has no common identity parser,
because its contents are optional OEM/part-specific data. Memory transactions are
not yet protected by a scheduler-level session, and this layout correction has
not been hardware-verified.

### ESPHome command-console query names

| Query name | Underlying command |
|---|---|
| `status` | QUERY STATUS |
| `version` | QUERY VERSION NUMBER |
| `device-type` | QUERY DEVICE TYPE |
| `physical-min` | QUERY PHYSICAL MINIMUM |
| `operating-mode` | QUERY OPERATING MODE |
| `actual-level` | QUERY ACTUAL LEVEL |
| `max-level` | QUERY MAX LEVEL |
| `min-level` | QUERY MIN LEVEL |
| `power-on-level` | QUERY POWER ON LEVEL |
| `failure-level` | QUERY SYSTEM FAILURE LEVEL |
| `power-on-flag` | QUERY LAMP POWER ON |
| `power-fail-flag` | QUERY POWER FAILURE |
| `fade` | QUERY FADE TIME / FADE RATE |
| `groups-0-7`, `groups-8-15` | QUERY GROUPS |
| `content-dtr0`, `content-dtr1`, `content-dtr2` | QUERY CONTENT DTR0/DTR1/DTR2 |

## Special Commands

Special commands do not use the normal addressed-command shape. Builders are in
`dali_protocol`; higher-level flows live in commissioning, memory, and device
helpers.

| Opcode | Name | Notes |
|---:|---|---|
| `0xA1` | TERMINATE | Implemented |
| `0xA3` | DTR0 DATA | Implemented |
| `0xA5` | INITIALISE | Implemented, send twice |
| `0xA7` | RANDOMIZE | Implemented, send twice |
| `0xA9` | COMPARE | Used by commissioning |
| `0xAB` | WITHDRAW | Used by commissioning |
| `0xAD` | PING | Implemented |
| `0xB1/0xB3/0xB5` | SEARCH ADDRH/M/L | Used by commissioning |
| `0xB7` | PROGRAM SHORT ADDRESS | Used by commissioning |
| `0xB9` | VERIFY SHORT ADDRESS | Used by commissioning |
| `0xBB` | QUERY SHORT ADDRESS | Used by commissioning |
| `0xC1` | ENABLE DEVICE TYPE | Used before device-type-specific commands |
| `0xC3` | DTR1 DATA | Implemented |
| `0xC5` | DTR2 DATA | Implemented |
| `0xC7` | WRITE MEMORY LOCATION | Builder implemented |
| `0xC9` | WRITE MEMORY LOCATION NO REPLY | Builder implemented |

## DALI-2 Input Device Commands

Input devices use instance types rather than control-gear DT numbers. Part 301
defines push-button instance type 1, Part 303 defines occupancy instance type 3,
and Part 304 defines light-sensor instance type 4.

Generic Part 103 queries live in `dali_input_device`; common configuration and
type-specific Part 301/303/304 builders live in `dali_input_config`. Configuration
instructions marked “twice” require two identical addressed frames inside the
send-twice window. A DTR load is a separate 24-bit special command and is not
itself duplicated.

### Generic Part 103 instance configuration

| Opcode | Command | DTR requirement | Sends | ESPHome `iconfig` name |
|---:|---|---|---:|---|
| `0x61` | SET EVENT PRIORITY | DTR0 | twice | `set-event-priority` |
| `0x62` | ENABLE INSTANCE | none | twice | `enable-instance` |
| `0x63` | DISABLE INSTANCE | none | twice | `disable-instance` |
| `0x64` | SET PRIMARY INSTANCE GROUP | DTR0 | twice | `set-primary-group` |
| `0x65` | SET INSTANCE GROUP 1 | DTR0 | twice | `set-instance-group-1` |
| `0x66` | SET INSTANCE GROUP 2 | DTR0 | twice | `set-instance-group-2` |
| `0x67` | SET EVENT SCHEME | DTR0 | twice | `set-event-scheme` |
| `0x68` | SET EVENT FILTER | DTR2:DTR1:DTR0 | twice | Not exposed; shared builder only |
| `0x69` | SET INSTANCE TYPE | DTR0 | twice | Not exposed; Part 103:2022 shared builder |
| `0x6A` | SET INSTANCE CONFIGURATION | DTR0 and DTR2:DTR1 | twice | Not exposed; Part 103:2022 shared builder |

Opcodes `0x6E`, `0x6F`, and `0x70` are reserved in Part 103:2022. They are not
generic report-timer, hysteresis, or deadtime setters.

### Generic Part 103 instance queries

These are single-send commands and expect an 8-bit backward-frame reply.

| Instance byte | Opcode | Query | ESPHome `iquery` name |
|---:|---:|---|---|
| `0xFE` | `0x35` | QUERY NUMBER OF INSTANCES | Discovery/device path |
| instance | `0x80` | QUERY INSTANCE TYPE | `instance-type` |
| instance | `0x81` | QUERY RESOLUTION | `resolution` |
| instance | `0x82` | QUERY INSTANCE ERROR | `instance-error` |
| instance | `0x83` | QUERY INSTANCE STATUS | `instance-status` |
| instance | `0x84` | QUERY EVENT PRIORITY | `event-priority` |
| instance | `0x86` | QUERY INSTANCE ENABLED | `instance-enabled` |
| instance | `0x88` | QUERY PRIMARY INSTANCE GROUP | `primary-group` |
| instance | `0x89` | QUERY INSTANCE GROUP 1 | `instance-group-1` |
| instance | `0x8A` | QUERY INSTANCE GROUP 2 | `instance-group-2` |
| instance | `0x8B` | QUERY EVENT SCHEME | `event-scheme` |
| instance | `0x8C` | QUERY INPUT VALUE | `input-value` |
| instance | `0x8D` | QUERY INPUT VALUE LATCH | `input-value-latch` |
| feature selector | `0x8E` | QUERY FEATURE TYPE | Not implemented; feature-address selector required |
| feature selector | `0x8F` | QUERY NEXT FEATURE TYPE | Not implemented; feature-address selector required |
| instance | `0x90` | QUERY EVENT FILTER 0-7 | `event-filter-0` |
| instance | `0x91` | QUERY EVENT FILTER 8-15 | `event-filter-1` |
| instance | `0x92` | QUERY EVENT FILTER 16-23 | `event-filter-2` |
| instance | `0x93` | QUERY INSTANCE CONFIGURATION | Shared frame builder; DTR0 selects the value and DTR2:DTR1 hold the 16-bit result |
| instance | `0x94` | QUERY AVAILABLE INSTANCE TYPES | Shared frame builder; DTR2:DTR1:DTR0 complete the 32-bit result |

The former generic `hysteresis`, `deadtime-gen`, and report-timer aliases at
`0x82`/`0x83`/`0x84` were incorrect. Those opcodes have only the query meanings
shown above.

Feature queries are deliberately not exposed until the shared addressing layer
can encode Part 103 feature selectors. The `0x93` and `0x94` builders construct
only the addressed query frame; a complete typed operation must keep the query
and its dependent DTR reads atomic.

### Part 301 push button, instance type 1

| Opcode | Command | DTR | Sends/reply | ESPHome name |
|---:|---|---|---|---|
| `0x00` | SET SHORT TIMER | DTR0 | twice | `pb-set-short-timer` |
| `0x01` | SET DOUBLE TIMER | DTR0 | twice | `pb-set-double-timer` |
| `0x02` | SET REPEAT TIMER | DTR0 | twice | `pb-set-repeat-timer` |
| `0x03` | SET STUCK TIMER | DTR0 | twice | `pb-set-stuck-timer` |
| `0x0A` | QUERY SHORT TIMER | none | reply | `pb-short-timer` |
| `0x0B` | QUERY SHORT TIMER MIN | none | reply | `pb-short-timer-min` |
| `0x0C` | QUERY DOUBLE TIMER | none | reply | `pb-double-timer` |
| `0x0D` | QUERY DOUBLE TIMER MIN | none | reply | `pb-double-timer-min` |
| `0x0E` | QUERY REPEAT TIMER | none | reply | `pb-repeat-timer` |
| `0x0F` | QUERY STUCK TIMER | none | reply | `pb-stuck-timer` |

Short, double, and repeat timer values use 20 ms units; the stuck timer uses
1 s units. The old `0xE0..0xE4` push-button builders were non-standard and have
been removed.

### Part 303 occupancy sensor, instance type 3

| Opcode | Command | DTR | Sends/reply | ESPHome name |
|---:|---|---|---|---|
| `0x20` | CATCH MOVEMENT | none | once | `catch-movement` |
| `0x21` | SET HOLD TIMER | DTR0 | twice | `set-hold-timer` |
| `0x22` | SET REPORT TIMER | DTR0 | twice | `set-report-timer` |
| `0x23` | SET DEADTIME TIMER | DTR0 | twice | `set-deadtime` |
| `0x24` | CANCEL HOLD TIMER | none | once | `cancel-hold-timer` |
| `0x25` | SET DETECTION RANGE | DTR0 | twice | `set-detection-range` |
| `0x26` | SET SENSITIVITY | DTR0 | twice | `set-sensitivity` |
| `0x29` | QUERY INSTANCE CAPABILITIES | none | reply | `occupancy-capabilities` |
| `0x2A` | QUERY DETECTION RANGE | none | reply | `detection-range` |
| `0x2B` | QUERY SENSITIVITY | none | reply | `sensitivity` |
| `0x2C` | QUERY DEADTIME TIMER | none | reply | `deadtime` |
| `0x2D` | QUERY HOLD TIMER | none | reply | `hold-timer` |
| `0x2E` | QUERY REPORT TIMER | none | reply | `report-timer` |
| `0x2F` | QUERY CATCHING | none | reply | `catching` |

Hold, report, and deadtime timer values use 10 s, 1 s, and 50 ms units
respectively. Opcodes `0x25`, `0x26`, and `0x29..0x2B` are the
IEC 62386-303:2017/AMD1:2024 additions and can be capability-dependent.

### Part 304 light sensor, instance type 4

| Opcode | Command | DTR | Sends/reply | ESPHome name |
|---:|---|---|---|---|
| `0x30` | SET REPORT TIMER | DTR0 | twice | `light-set-report-timer` |
| `0x31` | SET HYSTERESIS | DTR0 | twice | `light-set-hysteresis` |
| `0x32` | SET DEADTIME TIMER | DTR0 | twice | `light-set-deadtime` |
| `0x33` | SET HYSTERESIS MIN | DTR0 | twice | `light-set-hysteresis-min` |
| `0x3C` | QUERY HYSTERESIS MIN | none | reply | `light-hysteresis-min` |
| `0x3D` | QUERY DEADTIME TIMER | none | reply | `light-deadtime` |
| `0x3E` | QUERY REPORT TIMER | none | reply | `light-report-timer` |
| `0x3F` | QUERY HYSTERESIS | none | reply | `light-hysteresis` |

Report and deadtime values use 1 s and 50 ms units. Hysteresis is a percentage
in the range `0..25`; `hysteresisMin` is the absolute minimum band height in
input-value units. It is not a generic “deadband”.

ESPHome `iquery` names:

| Name | Meaning |
|---|---|
| `instance-type` | Input instance type |
| `resolution` | Bit resolution |
| `instance-error` | Part/type-specific instance error byte |
| `instance-enabled` | Whether the instance is active |
| `instance-status` | Status byte |
| `event-priority` | Configured event priority |
| `primary-group` | Primary instance-group assignment |
| `instance-group-1`, `instance-group-2` | Additional instance-group assignments |
| `event-scheme` | Configured Part 103 event source scheme |
| `event-filter-0`, `event-filter-1`, `event-filter-2` | Event-filter bytes 0-7, 8-15, and 16-23 |
| `input-value`, `input-value-latch` | Current and latched input values |
| `pb-short-timer`, `pb-short-timer-min` | Part 301 short timer and physical minimum |
| `pb-double-timer`, `pb-double-timer-min` | Part 301 double timer and physical minimum |
| `pb-repeat-timer`, `pb-stuck-timer` | Part 301 repeat and stuck timers |
| `hold-timer`, `report-timer`, `deadtime` | Part 303 occupancy timers (`0x2D`, `0x2E`, `0x2C`) |
| `occupancy-capabilities` | Part 303 occupancy capability byte |
| `detection-range`, `sensitivity`, `catching` | Part 303 range, sensitivity, and catch state |
| `light-report-timer`, `light-deadtime` | Part 304 light-sensor timers |
| `light-hysteresis`, `light-hysteresis-min` | Part 304 light-sensor hysteresis values |

ESPHome `iconfig` names:

> **Warning — hardware validation incomplete.**
> The opcode mapping has been independently audited, but these configuration
> writes have not been round-trip validated on the project hardware. Read with
> `iquery`, change one parameter, and read it back before another write. Do not
> treat successful queueing as proof that the device accepted the value.

| Name | Value | Meaning |
|---|---|---|
| `enable-instance` | none | Enable the selected instance |
| `disable-instance` | none | Disable the selected instance |
| `set-event-priority` | DTR0 | Set generic Part 103 event priority |
| `set-primary-group` | DTR0 | Set the primary instance group |
| `set-instance-group-1` | DTR0 | Set additional instance group 1 |
| `set-instance-group-2` | DTR0 | Set additional instance group 2 |
| `set-event-scheme` | DTR0 | Set the Part 103 event source scheme |
| `pb-set-short-timer` | DTR0 | Set the Part 301 short timer |
| `pb-set-double-timer` | DTR0 | Set the Part 301 double timer |
| `pb-set-repeat-timer` | DTR0 | Set the Part 301 repeat timer |
| `pb-set-stuck-timer` | DTR0 | Set the Part 301 stuck timer |
| `catch-movement` | none | Start Part 303 movement catching; single send |
| `cancel-hold-timer` | none | Cancel the Part 303 hold timer; single send |
| `set-hold-timer` | DTR0 | Set the Part 303 hold timer (`0x21`) |
| `set-report-timer` | DTR0 | Set the Part 303 report timer (`0x22`) |
| `set-deadtime` | DTR0 | Set the Part 303 deadtime (`0x23`) |
| `set-detection-range` | DTR0 | Set Part 303 detection range if supported |
| `set-sensitivity` | DTR0 | Set Part 303 sensitivity if supported |
| `light-set-report-timer` | DTR0 | Set the Part 304 report timer |
| `light-set-hysteresis` | DTR0 | Set Part 304 hysteresis |
| `light-set-deadtime` | DTR0 | Set the Part 304 deadtime timer |
| `light-set-hysteresis-min` | DTR0 | Set the Part 304 absolute hysteresis minimum |

For Part 301 timer writes, query `pb-short-timer-min` and
`pb-double-timer-min` first. A device may reject values below its reported
physical minimum even when they are inside the console's standard-wide range.

The next release intentionally removes the incorrect generic timer/hysteresis/
deadtime and non-standard Part 301/304 C builders and console aliases. Migrate
C callers to the explicit `pb`, `occ`, and `light` type-specific APIs. ESPHome
uses explicit `pb-*` and `light-*` names while retaining the established Part 303
occupancy names shown above.

## Vendor-Specific Instance Queries

Lunatone documents these commands for its DALI-2 sensor instances. They are
implemented in `dali_lunatone`, but are **untested on hardware** in this project
and are not exposed through the native CLI or ESPHome command console.

They are intentionally kept out of generic `dali_protocol` metadata because they
are not generic IEC 62386-103 input-device commands.

| Opcode | Name | Response |
|---:|---|---|
| `0x40` | LUNATONE QUERY VALUE MULTIPLICATOR | `UINT8` |
| `0x41` | LUNATONE QUERY VALUE DIVISOR | `UINT8` |
| `0x42` | LUNATONE QUERY OFFSET MSB | `UINT8` |
| `0x43` | LUNATONE QUERY OFFSET LSB | `UINT8` |
| `0x44` | LUNATONE QUERY OFFSET MULTIPLICATOR | `UINT8` |
| `0x45` | LUNATONE QUERY OFFSET DIVISOR | `UINT8` |
| `0x46` | LUNATONE QUERY UNIT | `UINT8` |

## Event Frames

`dali_event` keeps unsolicited frames raw-first. For IEC 62386-103 24-bit
input-device traffic it rejects command frames (`bit 16 = 1`), preserves all
ten event-information bits (`bits 9:0`), and decodes the five standard source
schemes:

| Source scheme | Encoded source fields |
|---|---|
| Instance | instance type, instance number |
| Device | device short address, instance type |
| Device/Instance | device short address, instance number |
| Device Group | device-group number, instance type |
| Instance Group | instance-group number, instance type |

Device/Instance frames do not contain an instance type; consumers must obtain
it from discovery/configuration rather than infer it from the event value.
Power notifications use the reserved Part 103 prefix and are represented as a
separate frame kind.

Part 301/type 1 push-button event information uses sparse standard values: released
`0x000`, pressed `0x001`, short press `0x002`, double press `0x005`, long-press
start/repeat/stop `0x009`/`0x00B`/`0x00C`, free `0x00E`, and stuck `0x00F`.

Legacy/DALI-1 pushbutton coupler frames remain decoded as:

```text
data = (target_address_byte << 8) | command_or_level_byte
```

Legacy frames identify the target/action, not the source coupler or source
button. Existing BF6 couplers use this mode successfully; the ESP32 observes the
frames and syncs HA state.
