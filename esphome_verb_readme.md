# ESPHome DALI Verb README

This guide documents the ESPHome DALI command-console verbs accepted by
`esphome/components/dali/dali_component.cpp`.

The command console is the ESPHome `text:` entity, usually named
`DALI Command`. Type one command string into that entity. The result appears in
the DALI component `command_result` text sensor, usually named
`DALI Command Result`.

Example YAML:

```yaml
dali:
  id: dali_bus
  tx_pin: 18
  rx_pin: 19
  command_result:
    name: "DALI Command Result"

text:
  - platform: dali
    dali_id: dali_bus
    name: "DALI Command"
    mode: text
```

## Syntax Rules

Commands are space-separated and lowercase:

```text
<verb> [parameters...]
```

Commands currently use at most 5 tokens and 95 characters. There is no quoting,
so every parameter must be a single token.

Results are short strings:

| Result | Meaning |
|---|---|
| `OK` | Command or helper sequence was queued successfully. |
| `pending` | An asynchronous command was queued and has not completed yet. |
| `err` | The DALI stack rejected the command or its execution later failed. |
| `queue full` | The scheduler queue was full; retry after bus traffic settles. |
| `no reply` | A query expected a reply but none arrived. |
| `N (0xHH)` | Query reply byte in decimal and hex. |
| `TX OK` / `TX2 OK` | Raw frame sent once / twice without waiting for a reply. |
| `TX ERR N` / `TX2 ERR N` | Raw transmission failed with DALI error code `N`. |
| `RX N (0xHH)` | Raw frame sent with `wait` and received reply byte. |
| `RX timeout` | Raw frame sent with `wait`, but no reply arrived. |

Every console path reports scheduler admission failures immediately. An
asynchronous command publishes `pending` until its completion callback replaces
the result. Only the newest submitted command may update the result, so a delayed
callback from an older command cannot overwrite a newer result. `OK` means queued,
not that a no-reply command has already executed or been accepted by the target
device.

## Target Parameters

Normal gear commands use `<target>`:

| Form | Range | Meaning |
|---|---:|---|
| `a<N>` | `0-63` | Short address. |
| `s<N>` | `0-63` | Alias for short address. |
| `g<N>` | `0-15` | Group address. |
| `b` | n/a | Broadcast. |

Input-device commands use `<instance-target>`:

| Form | Range | Meaning |
|---|---:|---|
| `a<N>:<I>` | address `0-63`, instance `0-31` | Short-addressed DALI-2 input instance. |
| `s<N>:<I>` | address `0-63`, instance `0-31` | Alias for `a<N>:<I>`. |

`iquery`, `iconfig`, `memread`, `memwrite`, `devquery`, and `dtrcheck` require
a short address. They do not accept group or broadcast targets.

## Quick Examples

```text
query a0 actual-level
level g0 180
off b
iquery a0:1 hold-timer
iconfig a0:1 set-hold-timer 20
memread a0 2 4
dtrcheck a0 0 66
raw 01FE36 len=24 wait
raw2 01FE15 len=24
```

## Direct Gear Verbs

### `off <target>`

Sends DALI `OFF`.

Examples:

```text
off a0
off g0
off b
```

### `max <target>`

Sends `RECALL MAX LEVEL`.

Example:

```text
max g0
```

### `min <target>`

Sends `RECALL MIN LEVEL`.

Example:

```text
min a3
```

### `level <target> <level>`

Sends a DAPC direct arc power command.

| Parameter | Range | Meaning |
|---|---:|---|
| `<level>` | `0-254` | Arc power level. `254` is maximum. `255` is not accepted. |

Example:

```text
level g0 128
```

## Raw Frame Verbs

### `raw <hex> len=<16|24> [wait]`

Sends one raw forward frame.

| Parameter | Meaning |
|---|---|
| `<hex>` | Frame data as hexadecimal. `0x` prefix is optional. |
| `len=16` | Treat `<hex>` as a 16-bit DALI frame. |
| `len=24` | Treat `<hex>` as a 24-bit DALI-2 frame. |
| `wait` | Optional. Wait for an 8-bit backward-frame reply. |

Examples:

```text
raw 01FE36 len=24 wait
raw C13004 len=24
```

### `raw2 <hex> len=<16|24> [wait]`

Same as `raw`, but sends the frame twice inside the DALI send-twice window.
Use this for commands that DALI requires to be transmitted twice.

Example:

```text
raw2 01FE15 len=24
```

## Gear Query Verb

### `query <target> <query-name>`

Sends a 16-bit control-gear query and waits for one reply byte.

Broadcast and group queries can cause multiple gear to reply at once. Prefer a
short address when you need a clean numeric result.

Available query names:

| Query name | Meaning |
|---|---|
| `actual-level` | Current arc power level. |
| `max-level` | Configured maximum level. |
| `min-level` | Configured minimum level. |
| `power-on-level` | Level used after power-on. |
| `failure-level` | Level used after system failure. |
| `status` | DALI status byte. |
| `version` | Version number. |
| `device-type` | Device type. |
| `power-on-flag` | Lamp power-on flag. |
| `power-fail-flag` | Power failure flag. |
| `groups-0-7` | Group membership bits for groups 0 through 7. |
| `groups-8-15` | Group membership bits for groups 8 through 15. |
| `physical-min` | Physical minimum level. |
| `operating-mode` | Operating mode byte. |
| `fade` | Combined fade time and fade rate byte. |
| `content-dtr0` | Current gear DTR0 value. |
| `content-dtr1` | Current gear DTR1 value. |
| `content-dtr2` | Current gear DTR2 value. |

Examples:

```text
query a0 actual-level
query a0 fade
query a0 groups-0-7
```

## Gear Config Verb

### `config <target> <config-name> [value]`

Sends a 16-bit control-gear configuration command. These commands use the DALI
send-twice path where required by the protocol.

For `*-dtr0` commands and `set-operating-mode`, `[value]` is loaded into DTR0
before the config command is sent. The parser accepts DTR0 values `0-255`.

For `add-group` and `remove-group`, `[value]` is required and is the group
number encoded into the DALI opcode. Use `0-15`. Short and group targets are
accepted; broadcast targets are rejected because they make the runtime group
query cache ambiguous. After group-target edits, run a scan if the source group
has not already been scan-verified.

Available config names:

| Config name | Value | Meaning |
|---|---|---|
| `reset` | none | Reset control gear variables. |
| `set-max-dtr0` | DTR0 `0-255` | Set maximum level from DTR0. |
| `set-min-dtr0` | DTR0 `0-255` | Set minimum level from DTR0. |
| `set-power-on-dtr0` | DTR0 `0-255` | Set power-on level from DTR0. |
| `set-failure-dtr0` | DTR0 `0-255` | Set system-failure level from DTR0. |
| `set-fade-time-dtr0` | DTR0 `0-255` | Set fade time from DTR0. |
| `set-fade-rate-dtr0` | DTR0 `0-255` | Set fade rate from DTR0. |
| `set-operating-mode` | DTR0 `0-255` | Set operating mode from DTR0. |
| `add-group` | group `0-15` | Add target gear to a group. |
| `remove-group` | group `0-15` | Remove target gear from a group. |
| `identify-device` | none | Send DALI identify-device command. |
| `save-persistent` | none | Ask gear to save persistent variables. |

Examples:

```text
config a0 set-max-dtr0 200
config a0 set-fade-time-dtr0 4
config a0 add-group 3
config g0 remove-group 3
config a0 save-persistent
```

## Input Instance Query Verb

### `iquery <instance-target> <query-name>`

Sends a 24-bit DALI-2 input-instance query and waits for one reply byte.
Run `iquery <target> instance-type` first and use Part 301, 303, or 304 names only
on an instance of type 1, 3, or 4 respectively.

Available `iquery` names:

| Query name | Part / opcode | Meaning |
|---|---:|---|
| `instance-type` | 103 / `0x80` | Input instance type. |
| `resolution` | 103 / `0x81` | Input resolution. |
| `instance-error` | 103 / `0x82` | Part/type-specific instance error byte. |
| `instance-status` | 103 / `0x83` | Instance status byte. |
| `event-priority` | 103 / `0x84` | Configured event priority. |
| `instance-enabled` | 103 / `0x86` | Whether the instance is enabled. |
| `primary-group` | 103 / `0x88` | Primary instance-group assignment. |
| `instance-group-1` | 103 / `0x89` | Additional instance-group assignment 1. |
| `instance-group-2` | 103 / `0x8A` | Additional instance-group assignment 2. |
| `event-scheme` | 103 / `0x8B` | Configured event source scheme. |
| `input-value` | 103 / `0x8C` | Current input value. |
| `input-value-latch` | 103 / `0x8D` | Latched input value. |
| `event-filter-0` | 103 / `0x90` | Event-filter bits 0 through 7. |
| `event-filter-1` | 103 / `0x91` | Event-filter bits 8 through 15. |
| `event-filter-2` | 103 / `0x92` | Event-filter bits 16 through 23. |
| `pb-short-timer` | 301 / `0x0A` | Push-button short timer. |
| `pb-short-timer-min` | 301 / `0x0B` | Push-button physical short-timer minimum. |
| `pb-double-timer` | 301 / `0x0C` | Push-button double-press timer. |
| `pb-double-timer-min` | 301 / `0x0D` | Push-button physical double-timer minimum. |
| `pb-repeat-timer` | 301 / `0x0E` | Push-button long-press repeat timer. |
| `pb-stuck-timer` | 301 / `0x0F` | Push-button stuck timer. |
| `deadtime` | 303 / `0x2C` | Occupancy deadtime. |
| `hold-timer` | 303 / `0x2D` | Occupancy hold timer. |
| `report-timer` | 303 / `0x2E` | Occupancy report timer. |
| `occupancy-capabilities` | 303 / `0x29` | Range/sensitivity capability bits. |
| `detection-range` | 303 / `0x2A` | Configured detection range. |
| `sensitivity` | 303 / `0x2B` | Configured detection sensitivity. |
| `catching` | 303 / `0x2F` | Whether movement catching is active. |
| `light-hysteresis-min` | 304 / `0x3C` | Absolute minimum hysteresis-band height. |
| `light-deadtime` | 304 / `0x3D` | Light-sensor deadtime timer. |
| `light-report-timer` | 304 / `0x3E` | Light-sensor report timer. |
| `light-hysteresis` | 304 / `0x3F` | Light-sensor hysteresis percentage. |

The removed `hysteresis` and `deadtime-gen` names were not generic Part 103
queries: opcodes `0x82` and `0x83` are QUERY INSTANCE ERROR and QUERY INSTANCE
STATUS. Use the `light-*` names for Part 304 values.

Examples:

```text
iquery a0:1 instance-type
iquery a0:1 input-value
iquery a0:1 hold-timer
iquery a0:1 occupancy-capabilities
```

## Input Instance Config Verb

### `iconfig <instance-target> <config-name> [dtr0]`

Sends a 24-bit DALI-2 input-instance configuration or control command. Commands
that consume DTR0 load it first and then use the send-twice path. The two Part
303 control instructions, `catch-movement` and `cancel-hold-timer`, are sent once
and do not consume DTR0.

Warning: the opcode surface has been independently audited, but these writes have
not been round-trip validated on the project hardware. For field testing, read
first, write one parameter, and read it back before making another change. `OK`
means the sequence was queued; it does not prove that the device accepted it.

Available `iconfig` names:

| Config name | Part / opcode | Value | Meaning |
|---|---:|---|---|
| `enable-instance` | 103 / `0x62` | none | Enable the selected instance. |
| `disable-instance` | 103 / `0x63` | none | Disable the selected instance. |
| `set-event-priority` | 103 / `0x61` | DTR0 `2-5` | Set event priority. |
| `set-primary-group` | 103 / `0x64` | DTR0 `0-31` or `255` | Set or clear the primary instance group. |
| `set-instance-group-1` | 103 / `0x65` | DTR0 `0-31` or `255` | Set or clear instance group 1. |
| `set-instance-group-2` | 103 / `0x66` | DTR0 `0-31` or `255` | Set or clear instance group 2. |
| `set-event-scheme` | 103 / `0x67` | DTR0 `0-4` | Set the event source scheme. |
| `pb-set-short-timer` | 301 / `0x00` | DTR0 `10-255` | Set the push-button short timer; 20 ms units. |
| `pb-set-double-timer` | 301 / `0x01` | DTR0 `0` or `10-100` | Set the double-press timer; 20 ms units. |
| `pb-set-repeat-timer` | 301 / `0x02` | DTR0 `5-100` | Set the long-press repeat timer; 20 ms units. |
| `pb-set-stuck-timer` | 301 / `0x03` | DTR0 `5-255` | Set the stuck timer; 1 s units. |
| `catch-movement` | 303 / `0x20` | none | Start movement catching; single send. |
| `set-hold-timer` | 303 / `0x21` | DTR0 `0-254` | Set occupancy hold timer; `0` selects 1 s, otherwise 10 s units. |
| `set-report-timer` | 303 / `0x22` | DTR0 `0-255` | Set occupancy report timer; 1 s units, `0` disables it. |
| `set-deadtime` | 303 / `0x23` | DTR0 `0-255` | Set occupancy deadtime; 50 ms units, `0` disables it. |
| `cancel-hold-timer` | 303 / `0x24` | none | Cancel the hold timer; single send. |
| `set-detection-range` | 303 / `0x25` | DTR0 `0-100` | Set detection range if supported. |
| `set-sensitivity` | 303 / `0x26` | DTR0 `0-100` | Set detection sensitivity if supported. |
| `light-set-report-timer` | 304 / `0x30` | DTR0 `0-255` | Set light report timer; 1 s units, `0` disables it. |
| `light-set-hysteresis` | 304 / `0x31` | DTR0 `0-25` | Set light hysteresis percentage. |
| `light-set-deadtime` | 304 / `0x32` | DTR0 `0-255` | Set light deadtime; 50 ms units, `0` disables it. |
| `light-set-hysteresis-min` | 304 / `0x33` | DTR0 `0-255` | Set the absolute minimum hysteresis-band height. |

Before setting a Part 301 short or double timer, query `pb-short-timer-min` or
`pb-double-timer-min`. The console enforces the standard-wide floor, but a device
may reject a value below its own reported physical minimum.

Generic Part 103 `SET EVENT FILTER` is not exposed by `iconfig`: it consumes
DTR2:DTR1:DTR0 and requires a three-register atomic transaction. Part 103:2022
SET INSTANCE TYPE and SET INSTANCE CONFIGURATION are also shared C builders only.
The old generic `set-hysteresis`/`set-deadtime-gen` aliases and non-standard
Part 301/304 setters are intentionally removed.

Recommended test pattern:

```text
iquery a0:1 hold-timer
iconfig a0:1 set-hold-timer 20
iquery a0:1 hold-timer
```

Example Part 301 query/write/read-back sequence:

```text
iquery a0:0 pb-short-timer
iconfig a0:0 pb-set-short-timer 25
iquery a0:0 pb-short-timer
```

## Control-Device Memory Verbs

These helpers use 24-bit DALI-2 control-device frames. They are useful for
devices such as the Steinel HF 360 II.

### `memread a<N> <bank> <offset>`

Reads one byte from a control-device memory bank.

| Parameter | Range | Meaning |
|---|---:|---|
| `a<N>` | `0-63` | Short address. |
| `<bank>` | `0-255` | Memory bank number loaded into control-device DTR1. |
| `<offset>` | `0-255` | Memory offset loaded into control-device DTR0. |

Example:

```text
memread a0 2 4
```

### `memwrite a<N> <bank> <offset> <value>`

Writes one byte to a control-device memory bank.

| Parameter | Range | Meaning |
|---|---:|---|
| `a<N>` | `0-63` | Short address. |
| `<bank>` | `1-255` | Writable memory bank number. Bank 0 is read-only. |
| `<offset>` | `0-255` | Memory offset. |
| `<value>` | `0-255` | Byte to write. |

This helper unlocks the bank by writing `0x55` to lock byte offset `0x02`, then
writes the requested byte. Its seven logical steps are copied into one scheduler
queue entry and run contiguously relative to other traffic queued by this
controller. `OK` means the complete helper sequence was queued; the no-reply
write does not acknowledge or verify that the device committed the byte, and an
external bus master is not excluded. Treat this as persistent device
configuration: read the current value first, write one value at a time, and read
it back afterward.

Example:

```text
memread a0 2 4
memwrite a0 2 4 255
memread a0 2 4
```

## Device-Level Diagnostic Verbs

### `devquery a<N> <opcode>`

Sends a single 24-bit device-level command with instance byte `0xFE` and waits
for one reply byte.

| Parameter | Range | Meaning |
|---|---:|---|
| `a<N>` | `0-63` | Short address. |
| `<opcode>` | `0-255` | Device-level opcode in decimal. |

Examples:

```text
devquery a0 54
devquery a0 55
```

`54` is `0x36`, query control-device DTR0. `55` is `0x37`, query
control-device DTR1.

### `dtrcheck a<N> <reg> <value>`

Loads control-device DTR0 or DTR1, then queries it back.

| Parameter | Range | Meaning |
|---|---:|---|
| `a<N>` | `0-63` | Short address. |
| `<reg>` | `0-1` | `0` = DTR0, `1` = DTR1. |
| `<value>` | `0-255` | Byte to load and verify. |

Example:

```text
dtrcheck a0 0 66
```

The expected result is `66 (0x42)`.

## Practical Bus Test Recipes

Read a lamp level:

```text
query a0 actual-level
```

Check group membership:

```text
query a0 groups-0-7
query a0 groups-8-15
```

Set and verify a DALI-2 occupancy hold timer:

```text
iquery a0:1 hold-timer
iconfig a0:1 set-hold-timer 20
iquery a0:1 hold-timer
```

Check Steinel Bank 2 global sensitivity byte:

```text
memread a0 2 4
```

Verify control-device DTR writes:

```text
dtrcheck a0 0 66
dtrcheck a0 1 2
```

Use raw frames when helper verbs are suspect:

```text
raw C13102 len=24
raw C13004 len=24
raw 01FE3C len=24 wait
```

