# DALI Command Reference - Working Draft

This is a working implementation reference for `dali_protocol`, parsers, and
dedicated vendor/profile helpers. It is not a replacement for IEC 62386. Public
manufacturer/library references are used to build the first command database.

Verification status as of 2026-06-09:

- Standard control-gear opcodes, response kind assignments, and special command
  opcodes are source-checked against the Microchip/IEC references.
- Standard DALI-2 input-device opcodes `0x81`, `0x8C`, and `0x8D` are
  source-checked against IEC 62386-103 references.
- Instance opcodes `0x40`..`0x46` are real Lunatone sensor-instance scaling
  queries implemented in `dali_lunatone`; they are intentionally excluded from
  generic `dali_protocol` metadata because they are sensor-specific extensions
  rather than generic IEC 62386-103 commands.
- Frame length, address range, instance range, broadcast-byte, and DAPC-level
  limits are centralized in `dali_frame` and reused by protocol/control helpers.
- Output-level helpers now cover the normal short/group/broadcast control
  commands through `GO TO SCENE`.
- Addressed 16-bit configuration instructions are available through the generic
  `dali_control_build_config` / `dali_control_config` path and the diagnostic
  `config` CLI. They use command metadata for opcode ranges and send-twice
  scheduling. DTR0-consuming configuration can be sequenced with an explicit
  DTR0 DATA load through `dali_control_config_with_dtr0` and diagnostic
  `config-dtr0`.
- Single-frame special command builders are available through
  `dali_build_special` and the diagnostic `special` CLI. Commissioning,
  device-type selection, and memory workflows that combine these frames remain
  separate flow-module work.
- Addressed 16-bit control-gear queries are available through the generic
  `dali_control_build_query` / `dali_control_query` path and the diagnostic
  `query` CLI. Higher-level flows that prepare DTR state or perform
  commissioning searches remain separate planned work.

## Sources Used

- Microchip DALI command table: https://onlinedocs.microchip.com/oxy/GUID-0CDBB4BA-5972-4F58-98B2-3F0408F3E10B-en-US-1/GUID-DA5EBBA5-6A56-4135-AF78-FB1F780EF475.html
- Microchip TB3200 PDF: https://ww1.microchip.com/downloads/en/Appnotes/90003200A.pdf
- Beckhoff DALI-2 Query Input Value: https://infosys.beckhoff.com/content/1033/tcplclib_tc2_dali/4346134027.html
- Lunatone DALI-2 instance guide: https://www.lunatone.com/wp-content/uploads/2021/10/DALI-2_Instance-Guide_EN_M0024.pdf
- Lunatone DALI-2 sensor instances: https://www.lunatone.com/wp-content/uploads/2022/11/Lunatone_DALI-2_Sensor_Instances_EN_M0026.pdf
- Steinel DALI-2 interface description: https://www.steinel.de/out/media/interfacedoc/94546_DALI-2%20Interface%20Description_V1.5.pdf

## Frame Encoding Notes

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

`DAPC` is not a named command byte. It is selected by address selector bit `0`,
and the second byte is the requested arc power level `0x00..0xFE`.

### DALI-2 24-bit instance query frame

Working layout for DALI-2 input-device queries:

```text
data = (device_address_byte << 16) | (instance_byte << 8) | command_byte
bit_length = 24
```

For a short-addressed input device, `device_address_byte = (short_addr << 1) | 1`.
Instance byte `0x00..0x1F` addresses a specific instance, `0xFE` addresses the
device-level command space, and `0xFF` addresses all instances where supported.

## Response Kinds To Model

| Kind | Meaning | Parser target |
|---|---|---|
| `NONE` | no backward frame expected | set commands |
| `YES_NO` | `0xFF` means YES; missing/no reply means NO in many query contexts | simple presence/status queries |
| `UINT8` | raw 8-bit value | levels, DTR values, device type |
| `STATUS` | QUERY STATUS bitfield | `DaliStatus` |
| `BITSET8` | 8-bit group/scene membership mask | group queries |
| `MEMORY_BYTE` | memory location value | read memory |
| `INPUT_VALUE_MSB` | first byte of latched DALI-2 input value | input devices |
| `INPUT_VALUE_LATCH` | subsequent byte of latched DALI-2 input value | input devices |
| `FADE_TIME_RATE` | packed high/low nibbles | fade-time/fade-rate query |

## Standard Control Gear Commands

### Direct Arc Power Control

| Code | Name | Response | Status |
|---:|---|---|---|
| `0x00..0xFE` | DAPC level, selected by address selector bit `0` | none | implemented for short/group/broadcast |

### Output Level Instructions

| Opcode | Name | Response | Status |
|---:|---|---|---|
| `0x00` | OFF | none | implemented for short/group/broadcast |
| `0x01` | UP | none | implemented for short/group/broadcast |
| `0x02` | DOWN | none | implemented for short/group/broadcast |
| `0x03` | STEP UP | none | implemented for short/group/broadcast |
| `0x04` | STEP DOWN | none | implemented for short/group/broadcast |
| `0x05` | RECALL MAX LEVEL | none | implemented for short/group/broadcast |
| `0x06` | RECALL MIN LEVEL | none | implemented for short/group/broadcast |
| `0x07` | STEP DOWN AND OFF | none | implemented for short/group/broadcast |
| `0x08` | ON AND STEP UP | none | implemented for short/group/broadcast |
| `0x09` | ENABLE DAPC SEQUENCE | none | implemented for short/group/broadcast, DALI-2 |
| `0x0A` | GO TO LAST ACTIVE LEVEL | none | implemented for short/group/broadcast, DALI-2 |
| `0x10..0x1F` | GO TO SCENE 0..15 | none | implemented for short/group/broadcast |

### Configuration Instructions

These are normal addressed 16-bit configuration instructions. The generic
control/config path builds them and schedules their required send-twice
transmission. Commands that consume DTR0 can either use the current DTR0 value
through `config`, or load DTR0 and run the consuming command as one scheduler
sequence through `config-dtr0`.

| Opcode | Name | Response | Status |
|---:|---|---|---|
| `0x20` | RESET | none | generic config/CLI implemented, send twice |
| `0x21` | STORE ACTUAL LEVEL IN DTR0 | none | generic config/CLI implemented, send twice |
| `0x22` | SAVE PERSISTENT VARIABLES | none | generic config/CLI implemented, send twice, DALI-2 |
| `0x23` | SET OPERATING MODE DTR0 | none | generic config/CLI implemented, send twice, DALI-2; sequenced DTR0 load available |
| `0x24` | RESET MEMORY BANK DTR0 | none | generic config/CLI implemented, send twice, DALI-2; sequenced DTR0 load available |
| `0x25` | IDENTIFY DEVICE | none | generic config/CLI implemented, send twice |
| `0x2A` | SET MAX LEVEL DTR0 | none | generic config/CLI implemented, send twice; sequenced DTR0 load available |
| `0x2B` | SET MIN LEVEL DTR0 | none | generic config/CLI implemented, send twice; sequenced DTR0 load available |
| `0x2C` | SET SYSTEM FAILURE LEVEL DTR0 | none | generic config/CLI implemented, send twice; sequenced DTR0 load available |
| `0x2D` | SET POWER ON LEVEL DTR0 | none | generic config/CLI implemented, send twice; sequenced DTR0 load available |
| `0x2E` | SET FADE TIME DTR0 | none | generic config/CLI implemented, send twice; sequenced DTR0 load available |
| `0x2F` | SET FADE RATE DTR0 | none | generic config/CLI implemented, send twice; sequenced DTR0 load available |
| `0x30` | SET EXTENDED FADE TIME DTR0 | none | generic config/CLI implemented, send twice, DALI-2; sequenced DTR0 load available |
| `0x40..0x4F` | SET SCENE 0..15 DTR0 | none | generic config/CLI implemented, send twice; sequenced DTR0 load available |
| `0x50..0x5F` | REMOVE FROM SCENE 0..15 | none | generic config/CLI implemented, send twice |
| `0x60..0x6F` | ADD TO GROUP 0..15 | none | generic config/CLI implemented, send twice |
| `0x70..0x7F` | REMOVE FROM GROUP 0..15 | none | generic config/CLI implemented, send twice |
| `0x80` | SET SHORT ADDRESS DTR0 | none | generic config/CLI implemented, send twice; sequenced DTR0 load available |
| `0x81` | ENABLE WRITE MEMORY | none | generic config/CLI implemented, send twice |

### Query Instructions

Queries should normally target a single short address. Group/broadcast queries
can create multiple simultaneous backward frames.

| Opcode | Name | Response | Status |
|---:|---|---|---|
| `0x90` | QUERY STATUS | `STATUS` | generic query plus named diagnostic CLI implemented |
| `0x91` | QUERY CONTROL GEAR PRESENT | `YES_NO` | generic query/parse/CLI implemented |
| `0x92` | QUERY LAMP FAILURE | `YES_NO` | generic query/parse/CLI implemented |
| `0x93` | QUERY LAMP POWER ON | `YES_NO` | generic query/parse/CLI implemented |
| `0x94` | QUERY LIMIT ERROR | `YES_NO` | generic query/parse/CLI implemented |
| `0x95` | QUERY RESET STATE | `YES_NO` | generic query/parse/CLI implemented |
| `0x96` | QUERY MISSING SHORT ADDRESS | `YES_NO` | generic query/parse/CLI implemented |
| `0x97` | QUERY VERSION NUMBER | `UINT8` | generic query/parse/CLI implemented |
| `0x98` | QUERY CONTENT DTR0 | `UINT8` | generic query/parse/CLI implemented |
| `0x99` | QUERY DEVICE TYPE | `UINT8` | generic query/parse/CLI implemented |
| `0x9A` | QUERY PHYSICAL MINIMUM | `UINT8` | generic query/parse/CLI implemented |
| `0x9B` | QUERY POWER FAILURE | `YES_NO` | generic query/parse/CLI implemented |
| `0x9C` | QUERY CONTENT DTR1 | `UINT8` | generic query/parse/CLI implemented |
| `0x9D` | QUERY CONTENT DTR2 | `UINT8` | generic query/parse/CLI implemented |
| `0x9E` | QUERY OPERATING MODE | `UINT8` | generic query/parse/CLI implemented, DALI-2 |
| `0x9F` | QUERY LIGHT SOURCE TYPE | `UINT8` | generic query/parse/CLI implemented, DALI-2 |
| `0xA0` | QUERY ACTUAL LEVEL | `UINT8` | generic query/parse/CLI implemented |
| `0xA1` | QUERY MAX LEVEL | `UINT8` | generic query/parse/CLI implemented |
| `0xA2` | QUERY MIN LEVEL | `UINT8` | generic query/parse/CLI implemented |
| `0xA3` | QUERY POWER ON LEVEL | `UINT8` | generic query/parse/CLI implemented |
| `0xA4` | QUERY SYSTEM FAILURE LEVEL | `UINT8` | generic query/parse/CLI implemented |
| `0xA5` | QUERY FADE TIME / FADE RATE | `FADE_TIME_RATE` | generic query/parse/CLI implemented |
| `0xA6` | QUERY MANUFACTURER SPECIFIC MODE | `YES_NO` | generic query/parse/CLI implemented, DALI-2 |
| `0xA7` | QUERY NEXT DEVICE TYPE | `UINT8` or special sequence | generic query/parse/CLI implemented; sequence interpretation pending |
| `0xA8` | QUERY EXTENDED FADE TIME | `UINT8` packed nibbles | generic query/parse/CLI implemented, DALI-2 |
| `0xAA` | QUERY CONTROL GEAR FAILURE | `YES_NO` | generic query/parse/CLI implemented, DALI-2 |
| `0xB0..0xBF` | QUERY SCENE LEVEL 0..15 | `UINT8` | generic query/parse/CLI implemented |
| `0xC0` | QUERY GROUPS 0-7 | `BITSET8` | generic query/parse/CLI implemented |
| `0xC1` | QUERY GROUPS 8-15 | `BITSET8` | generic query/parse/CLI implemented |
| `0xC2` | QUERY RANDOM ADDRESS H | `UINT8` | generic query/parse/CLI implemented; commissioning flow pending |
| `0xC3` | QUERY RANDOM ADDRESS M | `UINT8` | generic query/parse/CLI implemented; commissioning flow pending |
| `0xC4` | QUERY RANDOM ADDRESS L | `UINT8` | generic query/parse/CLI implemented; commissioning flow pending |
| `0xC5` | READ MEMORY LOCATION | `MEMORY_BYTE` | generic query/parse/CLI implemented; memory-address setup pending |
| `0xFF` | QUERY EXTENDED VERSION NUMBER | `UINT8` | generic query/parse/CLI implemented |

### Special Commands

Special commands do not use the same normal addressed-command shape as ordinary
16-bit gear commands. Single-frame builders are implemented separately from
commissioning and memory flow modules.

| Opcode | Name | Response | Status |
|---:|---|---|---|
| `0xA1` | TERMINATE | none | special frame builder/CLI implemented |
| `0xA3` | DTR0 DATA | none | special frame builder/control/CLI implemented |
| `0xA5` | INITIALISE | none | special frame builder/CLI implemented, send twice; commissioning flow pending |
| `0xA7` | RANDOMIZE | none | special frame builder/CLI implemented, send twice; commissioning flow pending |
| `0xA9` | COMPARE | `YES_NO` | special frame builder/CLI implemented; commissioning flow pending |
| `0xAB` | WITHDRAW | none | special frame builder/CLI implemented; commissioning flow pending |
| `0xAD` | PING | none | special frame builder/CLI implemented, DALI-2 |
| `0xB1` | SEARCH ADDRH | none | special frame builder/CLI implemented; commissioning flow pending |
| `0xB3` | SEARCH ADDRM | none | special frame builder/CLI implemented; commissioning flow pending |
| `0xB5` | SEARCH ADDRL | none | special frame builder/CLI implemented; commissioning flow pending |
| `0xB7` | PROGRAM SHORT ADDRESS | none | special frame builder/CLI implemented; commissioning flow pending |
| `0xB9` | VERIFY SHORT ADDRESS | `YES_NO` | special frame builder/CLI implemented; commissioning flow pending |
| `0xBB` | QUERY SHORT ADDRESS | `UINT8` | special frame builder/CLI implemented; commissioning flow pending |
| `0xC1` | ENABLE DEVICE TYPE | none | special frame builder/CLI implemented; device-type flow pending |
| `0xC3` | DTR1 DATA | none | special frame builder/control/CLI implemented |
| `0xC5` | DTR2 DATA | none | special frame builder/control/CLI implemented |
| `0xC7` | WRITE MEMORY LOCATION | `MEMORY_BYTE` | special frame builder/CLI implemented; memory flow pending |
| `0xC9` | WRITE MEMORY LOCATION NO REPLY | none | special frame builder/CLI implemented; memory flow pending |

## DALI-2 Input Device Commands

These are 24-bit device/instance commands used for generic DALI-2 input-device
discovery. They intentionally do not apply Steinel or Lunatone profiles.

| Instance byte | Opcode | Name | Response | Status |
|---:|---:|---|---|---|
| `0xFE` | `0x35` | QUERY NUMBER OF INSTANCES | `UINT8` | implemented in `dali_input_device` |
| instance | `0x80` | QUERY INSTANCE TYPE | `UINT8` | implemented in `dali_input_device` |
| instance | `0x81` | QUERY RESOLUTION | `UINT8` | implemented in `dali_input_device` |
| instance | `0x82` | QUERY INSTANCE ERROR | `YES_NO` | implemented in `dali_input_device` |
| instance | `0x83` | QUERY INSTANCE STATUS | `UINT8` | implemented in `dali_input_device` |
| instance | `0x86` | QUERY INSTANCE ENABLED | `YES_NO` | implemented in `dali_input_device` |
| instance | `0x8C` | QUERY INPUT VALUE | `INPUT_VALUE_MSB` | metadata/builder implemented; value CLI pending |
| instance | `0x8D` | QUERY INPUT VALUE LATCH | `INPUT_VALUE_LATCH` | metadata/builder implemented; value CLI pending |

Generic role classification in `dali_input_device`:

| Type | Role | Usable state |
|---:|---|---|
| `0` | generic | unverified until self-described, profiled, or user-confirmed |
| `1` | push-button | standard |
| `2` | absolute | standard |
| `3` | occupancy/motion | standard |
| `4` | light | standard |

### Lunatone Sensor-Specific Instance Queries

These are documented by Lunatone for their generic-purpose sensor instances.
They should not be treated as standard IEC 62386-103 commands or assumed to
work on the Steinel sensor until confirmed on hardware or in Steinel docs.

| Opcode | Name | Response | Status |
|---:|---|---|---|
| `0x40` | QUERY VALUE MULTIPLICATOR | `UINT8` | implemented in `dali_lunatone` |
| `0x41` | QUERY VALUE DIVISOR | `UINT8` | implemented in `dali_lunatone` |
| `0x42` | QUERY OFFSET MSB | `UINT8` | implemented in `dali_lunatone` |
| `0x43` | QUERY OFFSET LSB | `UINT8` | implemented in `dali_lunatone` |
| `0x44` | QUERY OFFSET MULTIPLICATOR | `UINT8` | implemented in `dali_lunatone` |
| `0x45` | QUERY OFFSET DIVISOR | `UINT8` | implemented in `dali_lunatone` |
| `0x46` | QUERY UNIT | `UINT8` | implemented in `dali_lunatone` |

Known Steinel HF 360 II DALI-2 IPD instance targets:

| Instance | Type | Meaning | Initial query plan |
|---:|---:|---|---|
| 0 | 4 | brightness | query resolution, query input value, latch if needed |
| 1 | 3 | motion | query resolution, query input value |
| 2 | 0 | generic temperature | query input value + latch, apply Steinel conversion |
| 3 | 0 | generic humidity | query input value, apply Steinel conversion |

## Implementation Plan

- [x] Add command metadata enums/tables in `dali_protocol`.
- [x] Add generic addressed command builders by target type.
- [x] Add response parser functions by `ResponseKind`.
- [x] Add DALI-2 input-device constants and short-address instance builders.
- [x] Add generic DALI-2 input-device count/type discovery helpers and CLI
      command `instances <addr>`.
- [x] Add Lunatone-specific query helpers outside generic `dali_protocol`.
- [x] Add Steinel-specific value conversion helpers outside PHY/scheduler.
- [x] Add static mapping validation helpers without entity-name assumptions.
- [x] Add initial unit tests for command metadata, generic builders, and parser dispatch.
- [x] Add focused unit tests for every new specialized command/parser before hardware tests.
- [x] Centralize shared protocol limits used by protocol, control, PHY,
      scheduler, mapping, and vendor helpers.
- [x] Add generic addressed control-gear query API, diagnostic CLI, and tests.
- [x] Add generic addressed configuration API, diagnostic CLI, send-twice
      scheduling, and tests.
- [x] Add DTR0/DTR1/DTR2 DATA builders plus fixed scheduler sequences for
      DTR0-consuming configuration commands.
- [x] Add generic single-frame special command builders and diagnostic CLI.
