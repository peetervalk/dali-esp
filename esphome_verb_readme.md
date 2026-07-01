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

Commands are whitespace-separated and lowercase:

```text
<verb> [parameters...]
```

The parser accepts up to 5 tokens. There is no quoting, so every parameter must
be a single token.

Results are short strings:

| Result | Meaning |
|---|---|
| `OK` | Command or helper sequence was queued successfully. |
| `err` | The DALI stack rejected the command or sequence. |
| `queue full` | The scheduler queue was full; retry after bus traffic settles. |
| `no reply` | A query expected a reply but none arrived. |
| `N (0xHH)` | Query reply byte in decimal and hex. |
| `TX OK` / `TX2 OK` | Raw frame sent once / twice without waiting for a reply. |
| `RX N (0xHH)` | Raw frame sent with `wait` and received reply byte. |
| `RX timeout` | Raw frame sent with `wait`, but no reply arrived. |

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

Available `iquery` names:

| Query name | Meaning |
|---|---|
| `hold-timer` | DT303 occupancy hold timer. |
| `deadtime` | DT303 occupancy deadtime. |
| `hysteresis` | Generic input hysteresis. |
| `deadtime-gen` | Generic input deadtime timer. |
| `report-timer` | DT303 occupancy report timer. |
| `instance-type` | Input instance type. |
| `resolution` | Input resolution. |
| `instance-enabled` | Whether the instance is enabled. |
| `instance-status` | Instance status byte. |
| `input-value` | Current input value. |
| `input-value-latch` | Latched input value. |

Examples:

```text
iquery a0:1 instance-type
iquery a0:1 input-value
iquery a0:1 hold-timer
```

## Input Instance Config Verb

### `iconfig <instance-target> <config-name> [dtr0]`

Sends a 24-bit DALI-2 input-instance configuration command. All `iconfig`
SET commands use the send-twice path.

Warning: the `iconfig` SET paths are implemented, but not every parameter has
been round-trip validated on hardware. For field testing, read first, write one
parameter, then read it back before making another change.

Available `iconfig` names:

| Config name | DTR0 | Meaning |
|---|---|---|
| `set-hold-timer` | required, `0-255` | Set DT303 occupancy hold timer. |
| `set-deadtime` | required, `0-255` | Set DT303 occupancy deadtime. |
| `set-hysteresis` | required, `0-255` | Set generic input hysteresis. |
| `set-report-timer` | required, `0-255` | Set DT303 occupancy report timer. |
| `set-deadtime-gen` | required, `0-255` | Set generic deadtime timer. |
| `enable-instance` | none | Enable the selected instance. |
| `disable-instance` | none | Disable the selected instance. |

Recommended test pattern:

```text
iquery a0:1 hold-timer
iconfig a0:1 set-hold-timer 20
iquery a0:1 hold-timer
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
| `<bank>` | `0-255` | Memory bank number. |
| `<offset>` | `0-255` | Memory offset. |
| `<value>` | `0-255` | Byte to write. |

This helper unlocks the bank by writing `0x55` to lock byte offset `0x02`, then
writes the requested byte. Treat this as persistent device configuration: read
the current value first and write only one value at a time.

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

