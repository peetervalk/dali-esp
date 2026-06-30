# DALI Command Reference

This is the project command/protocol reference for the native CLI, ESPHome
command console, and reusable protocol builders. It is not a replacement for
IEC 62386.

**Last reviewed:** 2026-06-26

## Current Implementation Status

- Standard control-gear addressing, DAPC, output commands, queries, and
  configuration commands are implemented in `dali_protocol` and `dali_control`.
- DTR0/DTR1/DTR2 DATA builders and DTR0-consuming configuration sequences are
  implemented.
- Commissioning, memory-bank reads, DT6 helpers, DT8 helpers, input-device
  queries, input polling, and input-device configuration frame builders are
  implemented.
- `dali_input_config` SET builders exist, but real-bus writes still require
  cautious validation. Read the current value first, write one parameter, then
  read back before broader use.
- ESPHome command console supports the practical subset needed from HA:
  `off`, `max`, `min`, `level`, `query`, `config`, `iquery`, and `iconfig`.
- Active code gaps and cleanup work live in `current_status.md`.

## Sources

- Microchip DALI command table:
  https://onlinedocs.microchip.com/oxy/GUID-0CDBB4BA-5972-4F58-98B2-3F0408F3E10B-en-US-1/GUID-DA5EBBA5-6A56-4135-AF78-FB1F780EF475.html
- Microchip TB3200:
  https://ww1.microchip.com/downloads/en/Appnotes/90003200A.pdf
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
```

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
the same text entity state twice.

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

Generic query builders live in `dali_input_device`; configuration/query builders
for IEC 62386-103 plus DT301/DT303/DT304 live in `dali_input_config`.

| Instance byte | Opcode | Name | Status |
|---:|---:|---|---|
| `0xFE` | `0x35` | QUERY NUMBER OF INSTANCES | Implemented |
| instance | `0x80` | QUERY INSTANCE TYPE | Implemented |
| instance | `0x81` | QUERY RESOLUTION | Implemented |
| instance | `0x82` | QUERY INSTANCE ERROR | Implemented |
| instance | `0x83` | QUERY INSTANCE STATUS | Implemented |
| instance | `0x86` | QUERY INSTANCE ENABLED | Implemented |
| instance | `0x8C` | QUERY INPUT VALUE | Implemented |
| instance | `0x8D` | QUERY INPUT VALUE LATCH | Implemented |

ESPHome `iquery` names:

| Name | Meaning |
|---|---|
| `instance-type` | Input instance type |
| `resolution` | Bit resolution |
| `instance-enabled` | Whether the instance is active |
| `instance-status` | Status byte |
| `hysteresis` | Generic input hysteresis |
| `deadtime-gen` | Generic deadtime timer |
| `hold-timer` | DT303 occupancy hold timer (`0x2D`) |
| `report-timer` | DT303 occupancy report timer (`0x2E`) |
| `deadtime` | DT303 occupancy deadtime (`0x2C`) |

ESPHome `iconfig` names:

> **Warning — hardware validation incomplete.**
> `iconfig` SET commands are implemented and reviewed against DALI-2 Part 103 but have not
> been round-trip validated on hardware for every parameter. A wrong encoding or instance
> address silently does nothing or corrupts an adjacent parameter. Always follow the sequence:
> read with `iquery` first, write one parameter with `iconfig`, then verify with `iquery`
> again. Do not write multiple parameters in a single session until each is individually
> confirmed.

| Name | Meaning |
|---|---|
| `enable-instance` | Enable the selected instance |
| `disable-instance` | Disable the selected instance |
| `set-hysteresis` | Set generic hysteresis from DTR0 |
| `set-deadtime-gen` | Set generic deadtime from DTR0 |
| `set-hold-timer` | Set DT303 hold timer from control-device DTR0 (`0x21`) |
| `set-report-timer` | Set DT303 report timer from control-device DTR0 (`0x22`) |
| `set-deadtime` | Set DT303 deadtime from control-device DTR0 (`0x23`) |

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

`dali_event` keeps unsolicited frames raw-first and decodes:

- DALI-2 24-bit input-device event frames:

```text
data = (address_byte << 16) | (instance_byte << 8) | event_byte
```

- Legacy/DALI-1 pushbutton coupler frames:

```text
data = (target_address_byte << 8) | command_or_level_byte
```

Legacy frames identify the target/action, not the source coupler or source
button. Existing BF6 couplers use this mode successfully; the ESP32 observes the
frames and syncs HA state.
