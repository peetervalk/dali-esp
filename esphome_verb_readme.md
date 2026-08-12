# ESPHome DALI Verb README

This guide documents the ESPHome DALI command-console verbs accepted by
`esphome/components/dali/dali_component.cpp`.

The console shares its tokeniser, argument parsers, and named command tables
with the native CLI (`components/dali/dali_cli.c`), so a verb, an argument form,
and a command name mean the same thing on both surfaces. Several verbs were
renamed when that became true; the full before/after table is in
`dali_command_reference.md` under "Verbs renamed when the console adopted the
shared tables". There are no aliases for the old names, so anything in Home
Assistant that writes a command string to the `text:` entity needs updating.

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

A line may hold at most 8 tokens of 31 characters each, and 95 characters in
total. There is no quoting, so every parameter must be a single token. An
over-long line or token is an error rather than a silent truncation, which would
turn a typo into a different, valid command.

Every verb declares how many arguments it takes, and both bounds are checked
before the command runs. A trailing token is therefore rejected rather than
ignored: `level s1 100 junk` fails with a usage string, where an earlier version
of the console accepted it as `level s1 100`.

Results are short strings:

| Result | Meaning |
|---|---|
| `OK` | Command or helper sequence was queued successfully. |
| `pending` | An asynchronous command was queued and has not completed yet. |
| `err` | The DALI stack rejected the command or its execution later failed. |
| `queue full` | The scheduler queue was full; retry after bus traffic settles. |
| `no reply` | A query expected a reply but none arrived. |
| `<name>: N (0xHH)` | Query reply, decoded under the name of the command that asked. |
| `<name>: yes (0xFF)` | A yes/no query's reply. |
| `<name>: malformed reply` | Something answered, but not as the command's reply kind. |
| `0xHH flag,flag` | A status byte and the flags it sets (see `status`). |
| `TX OK` / `TX2 OK` | Raw frame sent once / twice without waiting for a reply. |
| `TX ERR N` / `TX2 ERR N` | Raw transmission failed with DALI error code `N`. |
| `RX N (0xHH)` | Raw frame sent with `wait` and received reply byte. |
| `RX timeout` | Raw frame sent with `wait`, but no reply arrived. |
| `usage: <verb> ...` | Wrong number of arguments, including a trailing token. |
| `scan active` | A bus verb was refused because a scan is running. |

Every console path reports scheduler admission failures immediately. An
asynchronous command publishes `pending` until its completion callback replaces
the result. Only the newest submitted command may update the result, so a delayed
callback from an older command cannot overwrite a newer result. `OK` means queued,
not that a no-reply command has already executed or been accepted by the target
device.

A reply is decoded by the same shared function the native CLI prints through
(`dali_cli_format_response`), so a yes/no query answers `yes` here rather than
`255`, and a fade byte arrives split into its two nibbles. Earlier console
versions published every reply as a bare `N (0xHH)`; an automation that parses
the result string needs updating for the named, typed form.

## Target Parameters

Normal gear commands use `<target>`:

| Form | Range | Meaning |
|---|---:|---|
| `s<N>` | `0-63` | Short address. |
| `<N>` | `0-63` | Short address, bare number. |
| `g<N>` | `0-15` | Group address. |
| `b` | n/a | Broadcast. |

`a<N>` is not one of them. It was the spelling before the console adopted the
shared tables, and it now fails with `bad target` — including inside an
otherwise valid line, so a stored HA script written against the old syntax stops
working silently rather than loudly.

Input-device commands take the address and the instance as two separate
arguments, matching the native CLI:

| Form | Range | Meaning |
|---|---:|---|
| `<addr> <instance>` | address `0-63`, instance `0-31` | Short-addressed DALI-2 input instance. |

The address accepts a bare number or the `s<N>` form. The earlier `a<N>:<I>`
colon form is gone.

`iquery`, `iconfig`, `devmem`, and `dtrcheck` require a short address. They do
not accept group or broadcast targets.

## Quick Examples

```text
query s0 actual
status s0
level g0 180
scene g0 3
off b
dt6 5 dimming-curve
dt6 5 select-curve 1
iquery 0 1 occ-hold-timer
iconfig 0 1 occ-set-hold-timer 20
memread 0 0 3 8
devmem read 0 2 4
dtrcheck 0 0 66
raw 01FE36 len=24 wait
raw2 01FE15 len=24
group forget 7
```

## Direct Gear Verbs

### `off <target>`

Sends DALI `OFF`.

Examples:

```text
off s0
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
min s3
```

### `level <target> <level>`

Sends a DAPC direct arc power command.

| Parameter | Range | Meaning |
|---|---:|---|
| `<level>` | `0-254` | Arc power level. `254` is maximum. |
| `mask` | n/a | Arc power MASK: leave the level unchanged. |

`mask` is a separate frame builder rather than level 255, so no level arithmetic
can reach 255 and silently stop meaning "set this level".

Examples:

```text
level g0 128
level s3 mask
```

### `mask <target>`

Arc power MASK as its own verb, identical to `level <target> mask`.

### Fade and step verbs

Each sends one Part 102 command and returns as soon as it is queued. None of
them updates the light entity: the gear's level moves, and Home Assistant
catches up on the next refresh pass, the same as after a wall switch.

| Verb | DALI command | Effect |
|---|---|---|
| `up <target>` | `UP` | One fade step up, using the configured fade rate. |
| `down <target>` | `DOWN` | One fade step down. |
| `step-up <target>` | `STEP UP` | One level up, without fading. |
| `step-down <target>` | `STEP DOWN` | One level down, without fading. |
| `step-off <target>` | `STEP DOWN AND OFF` | One level down, switching off at the minimum. |
| `on-step <target>` | `ON AND STEP UP` | Switch on if off, otherwise one level up. |
| `cont-up <target>` | `CONTINUOUS UP` | Fade toward maximum until stopped (62386-102:2022). |
| `cont-down <target>` | `CONTINUOUS DOWN` | Fade toward minimum until stopped. |
| `dapc-seq <target>` | `ENABLE DAPC SEQUENCE` | Open the DAPC sequence window. |
| `last <target>` | `GO TO LAST ACTIVE LEVEL` | Return to the level before the last OFF. |

`up`/`down` fade over the gear's fade time; `step-up`/`step-down` change the
level immediately. Gear that is off ignores `up` and `step-up` — `on-step` is
the one that turns it on.

Examples:

```text
step-up s3
cont-down g0
last s3
```

### `scene <target> <0-15>`

Sends `GO TO SCENE`. The scene's level is stored in the gear, by
`config-dtr0 <target> set-scene <level> <scene>`.

```text
scene g0 3
```

### `status <target>`

Sends `QUERY STATUS` and decodes the reply into the flags that are set:

```text
status s0        ->  status: 0x06 lamp-fail,arc-on
status s3        ->  status: 0x00 none
```

| Flag | Meaning |
|---|---|
| `ballast-fail` | Ballast failure. |
| `lamp-fail` | Lamp failure. |
| `arc-on` | Lamp arc power is on. |
| `limit-err` | Requested level was outside the min/max window. |
| `fading` | A fade is running. |
| `reset` | Gear is in its reset state. |
| `no-addr` | No short address programmed. |
| `power-fail` | Power failure since the last reset. |

`query <target> status` returns the same decoded line.

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

### `raw2 <hex> len=<16|24>`

Same as `raw`, but sends the frame twice inside the DALI send-twice window.
It takes no `wait`: a send-twice command is a configuration write, not a query.
Use this for commands that DALI requires to be transmitted twice.

Example:

```text
raw2 01FE15 len=24
```

### `dtr <0|1|2> <0-255>`

Loads one control-gear DTR register by broadcast.

| Parameter | Range | Meaning |
|---|---:|---|
| `<reg>` | `0-2` | `0` = DTR0, `1` = DTR1, `2` = DTR2. |
| `<value>` | `0-255` | Byte to load. |

A DTR is consumed by whichever command reads it next, so a value loaded from
here only survives if nothing else transmits in between — including this
component's own refresh queries. Prefer the verbs that carry their own load and
cannot be interrupted: `config-dtr0`, `iconfig`, `dt6`, and the DTR0 form of
`iquery`. Use `dtr` for the cases none of those covers, and `dtrcheck` when you
need to confirm a *control-device* DTR took a value.

```text
dtr 0 200
```

## Gear Query Verb

### `query <target> <query-name> [param]`

Sends a 16-bit control-gear query and waits for one reply byte.

Broadcast and group queries can cause multiple gear to reply at once. Prefer a
short address when you need a clean numeric result.

The full shared table is in `dali_command_reference.md`; the names seen most
often from Home Assistant are:

| Query name | Meaning |
|---|---|
| `actual` | Current arc power level. |
| `max-level` | Configured maximum level. |
| `min-level` | Configured minimum level. |
| `power-on` | Level used after power-on. |
| `failure-level` | Level used after system failure. |
| `status` | DALI status byte. |
| `version` | Version number. |
| `device-type` | Device type. |
| `lamp-on` | Lamp power-on flag. |
| `power-failure` | Power failure flag. |
| `groups-0-7` | Group membership bits for groups 0 through 7. |
| `groups-8-15` | Group membership bits for groups 8 through 15. |
| `physical-min` | Physical minimum level. |
| `operating-mode` | Operating mode byte. |
| `fade` | Combined fade time and fade rate byte. |
| `dtr0`, `dtr1`, `dtr2` | Current gear DTR values. |
| `scene-level <0-15>` | Level stored for one scene; takes a parameter. |

Adopting the shared table also brought in the rest of the Part 102 query set —
`present`, `lamp-failure`, `limit-error`, `reset-state`, `missing-address`,
`light-source`, `random-h/m/l`, `extended-version`, and others.

Examples:

```text
query s0 actual
query s0 fade
query s0 groups-0-7
query s0 scene-level 3
```

## Gear Config Verb

### `config <target> <config-name> [param]` and `config-dtr0 <target> <config-name> <dtr0> [param]`

Sends a 16-bit control-gear configuration command, using the DALI send-twice
path where the protocol requires it.

The two verbs split by where the command gets its value:

- `config` is for commands whose parameter is encoded in the opcode, or that
  take none at all.
- `config-dtr0` is for commands that read DTR0. It loads DTR0 and sends the
  command as one scheduler sequence, so no other locally scheduled frame can
  replace DTR0 in between.

Using the wrong one is refused with a message naming the other, so a DTR0
command can never be sent with an unset register.

For `add-group` and `remove-group`, the group `0-15` is required. Short and
group targets are accepted; broadcast targets are rejected because they make the
runtime group query cache ambiguous. After group-target edits, run a scan if the
source group has not already been scan-verified.

Commonly used config names:

| Config name | Verb | Value | Meaning |
|---|---|---|---|
| `reset` | `config` | none | Reset control gear variables. |
| `identify-device` | `config` | none | Send DALI identify-device command. |
| `save-persistent` | `config` | none | Ask gear to save persistent variables. |
| `add-group` | `config` | group `0-15` | Add target gear to a group. |
| `remove-group` | `config` | group `0-15` | Remove target gear from a group. |
| `set-scene` | `config-dtr0` | scene `0-15` | Store DTR0 as the level for one scene. |
| `remove-scene` | `config` | scene `0-15` | Clear one scene. |
| `set-max-dtr0` | `config-dtr0` | DTR0 `0-255` | Set maximum level from DTR0. |
| `set-min-dtr0` | `config-dtr0` | DTR0 `0-255` | Set minimum level from DTR0. |
| `set-power-on-dtr0` | `config-dtr0` | DTR0 `0-255` | Set power-on level from DTR0. |
| `set-failure-dtr0` | `config-dtr0` | DTR0 `0-255` | Set system-failure level from DTR0. |
| `set-fade-time-dtr0` | `config-dtr0` | DTR0 `0-255` | Set fade time from DTR0. |
| `set-fade-rate-dtr0` | `config-dtr0` | DTR0 `0-255` | Set fade rate from DTR0. |
| `set-operating-mode-dtr0` | `config-dtr0` | DTR0 `0-255` | Set operating mode from DTR0. |
| `set-short-address-dtr0` | `config-dtr0` | DTR0 `0-255` | Set the short address from DTR0. |

Examples:

```text
config-dtr0 s0 set-max-dtr0 200
config-dtr0 s0 set-fade-time-dtr0 4
config-dtr0 s0 set-scene 200 3
config s0 add-group 3
config g0 remove-group 3
config s0 save-persistent
```

## Special Command Verb

### `special <name> [param]`

Sends one special (broadcast) command frame. These are not addressed: every
device on the bus sees them.

The commissioning primitives are refused here with
`commissioning special; use the native CLI`:

| Refused | Why |
|---|---|
| `initialise` | Opens the 15-minute commissioning window on every device. |
| `randomize` | Regenerates random addresses. Cannot be undone. |
| `search-h`, `search-m`, `search-l` | Search-address registers; only meaningful inside a commissioning walk. |
| `program-short` | Writes a short address to whatever is currently selected. |
| `withdraw` | Removes the selected device from the search. |
| `write-memory`, `write-memory-nr` | Write wherever DTR1/DTR0 currently point. |

This integration exposes discovery, not a guarded commissioning workflow, and
those are the commands that can readdress a whole installation from one line
typed into a Home Assistant text box. The native CLI runs them sequenced and
checked inside `commission unaddressed`.

Available here:

| Name | Param | Meaning |
|---|---|---|
| `terminate` | none | End an initialise/commissioning window another tool opened. |
| `dtr0`, `dtr1`, `dtr2` | `0-255` | Load a DTR register (same frames as the `dtr` verb). |
| `ping` | none | DALI-2 presence ping. |
| `compare` | none | Whether any device is in the current search selection. |
| `verify-short` | `0-63` | Whether a device holds this short address. |
| `query-short` | none | The selected device's short address. |
| `enable-type` | `0-255` | ENABLE DEVICE TYPE for the next command. |

`terminate` is deliberately available: it is the one command that undoes an
initialise window, and withholding it would leave you holding the problem
without the remedy.

`enable-type` applies only to the very next command on the bus, which this
console cannot guarantee is yours — the `dt6` verb exists because it sends the
enable and the command as one inseparable sequence.

Examples:

```text
special terminate
special verify-short 3
special ping
```

## Device Type 6 Verb

### `dt6 <addr> <name> [dtr0]`

Sends one IEC 62386-207 (LED gear) command. The command table, its names, and
the DTR0 byte each one takes are the native CLI's, so `dt6 5 dimming-curve`
means the same thing on both surfaces.

Every `dt6` line goes out as one scheduler sequence: the DTR0 load if the
command takes one, `ENABLE DEVICE TYPE 6`, and the command itself. They cannot
be separated by other locally scheduled traffic — a DT6 command that arrived
without its enable would be interpreted by the gear under its default device
type, which is a different command entirely.

DT6 commands only mean anything on gear that reports device type 6. Check with
`query <addr> device-type` first.

Configuration commands (sent twice, no reply):

| Name | DTR0 | Meaning |
|---|---|---|
| `ref-power` | none | Start a reference system power measurement. |
| `enable-protector` | none | Enable the current protector. |
| `disable-protector` | none | Disable the current protector. |
| `select-curve` | `0` standard, `1` linear | Select the dimming curve. |
| `store-fast-fade` | fast fade time | Store DTR0 as the fast fade time. |

Queries (one reply, decoded):

| Name | Reply | Meaning |
|---|---|---|
| `gear-type` | bitset | Gear type bits. |
| `dimming-curve` | number | `0` standard (logarithmic), `1` linear. |
| `operating-modes` | bitset | Operating modes the gear supports. |
| `features` | bitset | Supported DT6 features. |
| `failure-status` | bitset | All eight failure flags as one byte. |
| `short-circuit` | yes/no | LED short circuit. |
| `open-circuit` | yes/no | LED open circuit. |
| `load-decrease` | yes/no | Load decrease detected. |
| `load-increase` | yes/no | Load increase detected. |
| `protector-active` | yes/no | Current protector is currently acting. |
| `thermal-shutdown` | yes/no | Thermal shutdown active. |
| `thermal-overload` | yes/no | Thermal overload with light-level reduction. |
| `reference-running` | yes/no | Reference measurement in progress. |
| `reference-failed` | yes/no | Last reference measurement failed. |
| `protector-enabled` | yes/no | Current protector is enabled. |
| `operating-mode` | number | Current operating mode. |
| `fast-fade` | number | Configured fast fade time. |
| `min-fast-fade` | number | Physical minimum fast fade time. |
| `version` | number | DT6 extended version number. |

`failure-status` returns the raw bitset. The per-flag decode is printed only by
the native CLI, where there is room for eight lines.

**`select-curve` changes how brightness maps to the bus.** The component drops
its cached level profile for that address and starts a refresh, because every
brightness it sends is computed from the curve — a stale one misreports and
miscommands in both directions. The static counterpart is `dimming_curve:` on a
light entity, for gear whose curve query is not trustworthy:

```yaml
light:
  - platform: dali
    dali_id: dali_bus
    name: "Office"
    target_type: short
    target_address: 5
    dimming_curve: linear   # auto (default) | standard | linear
```

Examples:

```text
dt6 5 dimming-curve
dt6 5 select-curve 1
dt6 5 failure-status
dt6 5 store-fast-fade 2
```

Device type 8 (colour) is implemented in the shared stack and reachable from the
native CLI, but is deliberately not on this surface until it has been run
against real DT8 gear.

## Input Instance Query Verb

### `iquery <addr> <instance> <query-name> [dtr0]`

Sends a 24-bit DALI-2 input-instance query and waits for one reply byte.
Run `iquery <addr> <instance> type` first and use Part 301, 303, or 304 names
only on an instance of type 1, 3, or 4 respectively.

A name whose value is selected by DTR0 — `instance-config` — takes the selector
as a fourth argument, and the load and the read go out as one sequence. A DTR0
written by a separate command can be replaced before the query that reads it
arrives, so the two forms are not interchangeable: the console rejects a
selector on a query that takes none, and refuses a DTR0 query without one.

Available `iquery` names:

| Query name | Part / opcode | Meaning |
|---|---:|---|
| `type` | 103 / `0x80` | Input instance type. |
| `resolution` | 103 / `0x81` | Input resolution. |
| `error` | 103 / `0x82` | Part/type-specific instance error byte. |
| `status` | 103 / `0x83` | Instance status byte. |
| `event-priority` | 103 / `0x84` | Configured event priority. |
| `enabled` | 103 / `0x86` | Whether the instance is enabled. |
| `primary-group` | 103 / `0x88` | Primary instance-group assignment. |
| `group1` | 103 / `0x89` | Additional instance-group assignment 1. |
| `group2` | 103 / `0x8A` | Additional instance-group assignment 2. |
| `event-scheme` | 103 / `0x8B` | Configured event source scheme. |
| `input-value` | 103 / `0x8C` | Current input value. |
| `input-value-latch` | 103 / `0x8D` | Latched input value. |
| `event-filter0` | 103 / `0x90` | Event-filter bits 0 through 7. |
| `event-filter1` | 103 / `0x91` | Event-filter bits 8 through 15. |
| `event-filter2` | 103 / `0x92` | Event-filter bits 16 through 23. |
| `pb-short-timer` | 301 / `0x0A` | Push-button short timer. |
| `pb-short-timer-min` | 301 / `0x0B` | Push-button physical short-timer minimum. |
| `pb-double-timer` | 301 / `0x0C` | Push-button double-press timer. |
| `pb-double-timer-min` | 301 / `0x0D` | Push-button physical double-timer minimum. |
| `pb-repeat-timer` | 301 / `0x0E` | Push-button long-press repeat timer. |
| `pb-stuck-timer` | 301 / `0x0F` | Push-button stuck timer. |
| `occ-deadtime` | 303 / `0x2C` | Occupancy deadtime. |
| `occ-hold-timer` | 303 / `0x2D` | Occupancy hold timer. |
| `occ-report-timer` | 303 / `0x2E` | Occupancy report timer. |
| `occ-capabilities` | 303 / `0x29` | Range/sensitivity capability bits. |
| `occ-detection-range` | 303 / `0x2A` | Configured detection range. |
| `occ-sensitivity` | 303 / `0x2B` | Configured detection sensitivity. |
| `occ-catching` | 303 / `0x2F` | Whether movement catching is active. |
| `light-hysteresis-min` | 304 / `0x3C` | Absolute minimum hysteresis-band height. |
| `light-deadtime` | 304 / `0x3D` | Light-sensor deadtime timer. |
| `light-report-timer` | 304 / `0x3E` | Light-sensor report timer. |
| `light-hysteresis` | 304 / `0x3F` | Light-sensor hysteresis percentage. |

The removed `hysteresis` and `deadtime-gen` names were not generic Part 103
queries: opcodes `0x82` and `0x83` are QUERY INSTANCE ERROR and QUERY INSTANCE
STATUS. Use the `light-*` names for Part 304 values.

Examples:

```text
iquery 0 1 type
iquery 0 1 input-value
iquery 0 1 occ-hold-timer
iquery 0 1 occ-capabilities
iquery 0 1 instance-config 2
```

## Input Instance Config Verb

### `iconfig <addr> <instance> <config-name> [v0] [v1] [v2]`

Sends a 24-bit DALI-2 input-instance configuration or control command. Commands
that consume DTR0 load it first and then use the send-twice path. The two Part
303 control instructions, `occ-catch-movement` and `occ-cancel-hold`, are sent once
and do not consume DTR0.

Warning: the opcode surface has been independently audited, but these writes have
not been round-trip validated on the project hardware. For field testing, read
first, write one parameter, and read it back before making another change. `OK`
means the sequence was queued; it does not prove that the device accepted it.

Available `iconfig` names:

| Config name | Part / opcode | Value | Meaning |
|---|---:|---|---|
| `enable` | 103 / `0x62` | none | Enable the selected instance. |
| `disable` | 103 / `0x63` | none | Disable the selected instance. |
| `set-event-priority` | 103 / `0x61` | DTR0 `2-5` | Set event priority. |
| `set-primary-group` | 103 / `0x64` | DTR0 `0-31` or `255` | Set or clear the primary instance group. |
| `set-group1` | 103 / `0x65` | DTR0 `0-31` or `255` | Set or clear instance group 1. |
| `set-group2` | 103 / `0x66` | DTR0 `0-31` or `255` | Set or clear instance group 2. |
| `set-event-scheme` | 103 / `0x67` | DTR0 `0-4` | Set the event source scheme. |
| `pb-set-short-timer` | 301 / `0x00` | DTR0 `10-255` | Set the push-button short timer; 20 ms units. |
| `pb-set-double-timer` | 301 / `0x01` | DTR0 `0` or `10-100` | Set the double-press timer; 20 ms units. |
| `pb-set-repeat-timer` | 301 / `0x02` | DTR0 `5-100` | Set the long-press repeat timer; 20 ms units. |
| `pb-set-stuck-timer` | 301 / `0x03` | DTR0 `5-255` | Set the stuck timer; 1 s units. |
| `occ-catch-movement` | 303 / `0x20` | none | Start movement catching; single send. |
| `occ-set-hold-timer` | 303 / `0x21` | DTR0 `0-254` | Set occupancy hold timer; `0` selects 1 s, otherwise 10 s units. |
| `occ-set-report-timer` | 303 / `0x22` | DTR0 `0-255` | Set occupancy report timer; 1 s units, `0` disables it. |
| `occ-set-deadtime` | 303 / `0x23` | DTR0 `0-255` | Set occupancy deadtime; 50 ms units, `0` disables it. |
| `occ-cancel-hold` | 303 / `0x24` | none | Cancel the hold timer; single send. |
| `occ-set-detection-range` | 303 / `0x25` | DTR0 `0-100` | Set detection range if supported. |
| `occ-set-sensitivity` | 303 / `0x26` | DTR0 `0-100` | Set detection sensitivity if supported. |
| `light-set-report-timer` | 304 / `0x30` | DTR0 `0-255` | Set light report timer; 1 s units, `0` disables it. |
| `light-set-hysteresis` | 304 / `0x31` | DTR0 `0-25` | Set light hysteresis percentage. |
| `light-set-deadtime` | 304 / `0x32` | DTR0 `0-255` | Set light deadtime; 50 ms units, `0` disables it. |
| `light-set-hysteresis-min` | 304 / `0x33` | DTR0 `0-255` | Set the absolute minimum hysteresis-band height. |

Before setting a Part 301 short or double timer, query `pb-short-timer-min` or
`pb-double-timer-min`. The console enforces the standard-wide floor, but a device
may reject a value below its own reported physical minimum.

Adopting the shared table also brought in the three multi-DTR commands that the
console previously did not expose. They take their values as separate arguments,
and the DTR loads travel with the command in one sequence:

| Config name | Part / opcode | Values | Meaning |
|---|---:|---|---|
| `set-event-filter` | 103 / `0x68` | `<v0> <v1> <v2>` | eventFilter bits 0-7, 8-15, 16-23. |
| `set-instance-type` | 103 / `0x69` | DTR0 `0-31` | Set the instance type. |
| `set-instance-config` | 103 / `0x6A` | `<index> <low> <high>` | DTR0 selects the index; DTR2:DTR1 hold the 16-bit value. |

Every value is range-checked against the applicable Part 103/3xx command
definition before anything is sent. An out-of-range byte is refused with the
accepted range rather than transmitted: some gear stores it, and the mistake then
shows up later as odd behaviour instead of as a rejected command.

The old generic `set-hysteresis`/`set-deadtime-gen` aliases and non-standard
Part 301/304 setters are intentionally removed.

Recommended test pattern:

```text
iquery 0 1 occ-hold-timer
iconfig 0 1 occ-set-hold-timer 20
iquery 0 1 occ-hold-timer
```

Example Part 301 query/write/read-back sequence:

```text
iquery 0 0 pb-short-timer
iconfig 0 0 pb-set-short-timer 25
iquery 0 0 pb-short-timer
```

## Vendor Verb

### `vendor lunatone <addr> <instance> <name>`

Sends one Lunatone-documented sensor instance query. These are 24-bit instance
frames, but they are not generic Part 103 commands — they only mean anything on
Lunatone hardware. They describe how to scale that instance's raw input value:

| Name | Meaning |
|---|---|
| `multiplicator` | Value multiplicator. |
| `divisor` | Value divisor. |
| `offset-msb`, `offset-lsb` | Offset, high and low byte. |
| `offset-mult`, `offset-div` | Offset multiplicator and divisor. |
| `unit` | Unit code. |

```text
vendor lunatone 0 0 unit
vendor lunatone 0 0 multiplicator
```

### `vendor steinel <instance> <raw>`

Converts a raw Steinel HF 360 II reading into its physical unit. This sends
nothing: it applies the same conversion the sensor platform applies, to a value
you already have. Because it touches no bus, it stays answerable during a scan.

| Instance | Reports |
|---:|---|
| `0` | Illuminance, in lux (raw scale 0.01). |
| `1` | Motion. |
| `2` | Temperature, in °C. |
| `3` | Relative humidity, in %. |

The conversions are `(raw - 50) / 10` for temperature and `raw / 2` for
humidity:

```text
vendor steinel 2 262     ->  temperature: 21.2 C
vendor steinel 3 110     ->  humidity: 55.0 %
```

## Memory Verbs

Two classes of device, two sets of opcodes, two verbs:

| Verb | Frames | Reads from |
|---|---|---|
| `memread` | 16-bit Part 102 | Control gear — drivers, ballasts |
| `devmem read` / `devmem write` | 24-bit Part 103 | Control devices — sensors, switches |

They are named apart rather than overloaded because picking the wrong one
addresses a different class of device entirely. `devmem` replaced the console's
earlier `memread`/`memwrite`, which were the control-device forms all along
despite their names.

Reading a Bank 0 identity block is not on this surface: it means walking the
bank and deciding what to ask next from what came back, which needs the blocking
transport the native CLI has. The scan already reports Bank 0 identity for every
device it finds. Use `meminfo` on the native CLI for a one-off.

### `memread <addr> <bank> <offset> [count]`

Reads consecutive bytes from a **control-gear** memory bank (IEC 62386-102).

| Parameter | Range | Meaning |
|---|---:|---|
| `<addr>` | `0-63` | Short address. |
| `<bank>` | `0-255` | Memory bank, loaded into DTR1. |
| `<offset>` | `0-255` | Offset, loaded into DTR0. |
| `[count]` | `1-14` | Bytes to read. Defaults to 1. |

DTR1, DTR0, and every read frame go out as one sequence. `READ MEMORY LOCATION`
increments the device's offset cursor, so a read that was interrupted and
retried would return the bytes after the ones asked for; keeping the sequence
intact is what prevents that. The result lists every byte in hex.

Bank 0 offset `0x03` is the GTIN, `0x0B` the eight-byte identification number.

```text
memread 0 0 3 8
memread 5 0 0
```

### Control-device memory

These use 24-bit DALI-2 control-device frames. They are useful for devices such
as the Steinel HF 360 II.

### `devmem read <addr> <bank> <offset> [count]`

Reads one or more consecutive bytes from a control-device memory bank.

| Parameter | Range | Meaning |
|---|---:|---|
| `<addr>` | `0-63` | Short address. |
| `<bank>` | `0-255` | Memory bank number loaded into control-device DTR1. |
| `<offset>` | `0-255` | Memory offset loaded into control-device DTR0. |
| `[count]` | `1-16` | Bytes to read. Defaults to 1. |

All bytes are read in one sequence, so the device's auto-incrementing memory
cursor cannot be moved by other traffic mid-read. The result lists every byte in
hex, so a multi-byte read is not reduced to its last value.

Examples:

```text
devmem read 0 2 4
devmem read 0 0 3 8
```

### `devmem write <addr> <bank> <offset> <value>`

Writes one byte to a control-device memory bank.

| Parameter | Range | Meaning |
|---|---:|---|
| `<addr>` | `0-63` | Short address. |
| `<bank>` | `1-255` | Writable memory bank number. Bank 0 is read-only and refused. |
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
devmem read 0 2 4
devmem write 0 2 4 255
devmem read 0 2 4
```

## Device-Level Diagnostic Verbs

The former `devquery a<N> <opcode>` verb is gone. It built a device-level frame
(address, `0xFE`, opcode) and waited for a reply, which `raw` already does:

```text
raw 01FE36 len=24 wait     # was: devquery a0 54 — query control-device DTR0
raw 01FE37 len=24 wait     # was: devquery a0 55 — query control-device DTR1
```

### `dtrcheck <addr> <reg> <value>`

Loads a control-device DTR and reads it straight back.

| Parameter | Range | Meaning |
|---|---:|---|
| `<addr>` | `0-63` | Short address. |
| `<reg>` | `0-2` | `0` = DTR0, `1` = DTR1, `2` = DTR2. |
| `<value>` | `0-255` | Byte to load and verify. |

A DTR load produces no backward frame, so nothing else confirms that a control
device took the value. Both frames go out as one sequence, so no other locally
scheduled write can land between them. Use it to separate "the DTR never took
the value" from "the command that consumes it was ignored".

Example:

```text
dtrcheck 0 0 66
```

The expected result is `read 66 (0x42)`.

## Local Diagnostic Verbs

These generate no bus traffic: they answer from local state, and so remain
available while a scan is running — which is when queue pressure and a stale
group cache are most worth inspecting. `vendor steinel` is the third verb in
this class; it is documented with the other vendor helpers above.

### `queue [reset]`

Reports scheduler queue admission state.

`reset` clears the high-water mark and the counters. High-water is rebased on the
current depth rather than zero, so the reading never understates live occupancy.

| Field | Meaning |
|---|---|
| `d=<n>/<cap>` | Entries queued now, and queue capacity. The entry currently executing has already been popped, so it is not counted. |
| `hw=<n>` | Largest depth reached. At capacity means admission came within one submission of failing. |
| `ok=<n>` | Submissions accepted. |
| `full=<n>` | Submissions refused because the queue was full. |
| `busy=<n>` | Submissions refused by a pending scheduler reset barrier. |

Example:

```text
queue
```

A typical idle result is `d=0/16 hw=3 ok=214 full=0 busy=0`.

A non-zero `full` or `busy` is dropped work, not deferred work: the scheduler
never retries a refused submission on the caller's behalf. Light entities and the
refresh pump retain their own state and retry, but a headless dispatch action
refused this way is discarded rather than replayed against a stale physical
context. The same counters are logged as warnings whenever they advance.

### `group forget <addr> [group]`

Retires a departed member from the group-membership cache — the table that picks
which short address a group light polls for its state.

| Parameter | Range | Meaning |
|---|---:|---|
| `<addr>` | `0-63` | Short address to retire. |
| `[group]` | `0-15` | One group. Omit to retire the address from every group. |

A bus scan deliberately keeps the memberships of gear it did not see, so that a
device which is merely offline or briefly unpowered is not dropped. The
consequence is that physically removed gear stays in the cache forever, still
eligible as a group's query target, and no scan can ever clear it. This is the
explicit way to say the gear is gone.

It sends nothing on the bus — the gear cannot answer — and only edits the local
cache, which is then persisted to flash. For gear that is still present, use
`config <target> remove-group <g>` instead: that reconfigures the device itself.

Examples:

```text
group forget 7          # remove address 7 from every group
group forget 7 3        # remove address 7 from group 3 only
```

Reports `not a member` when the address held no membership to remove.

## Practical Bus Test Recipes

Read a lamp level:

```text
query s0 actual
```

Check group membership:

```text
query s0 groups-0-7
query s0 groups-8-15
```

Set and verify a DALI-2 occupancy hold timer:

```text
iquery 0 1 occ-hold-timer
iconfig 0 1 occ-set-hold-timer 20
iquery 0 1 occ-hold-timer
```

Diagnose a driver that reports the wrong brightness:

```text
query s5 device-type       # DT6 gear answers 6
dt6 5 dimming-curve        # 0 standard, 1 linear
query s5 min-level
query s5 max-level
```

If the curve is not what the entity assumes, either correct the gear
(`dt6 5 select-curve 0`) or pin the entity with `dimming_curve:` in YAML. Both
paths drop the cached profile; the first also re-reads it.

Check an LED driver for faults:

```text
status s5                  # Part 102 status flags
dt6 5 failure-status       # DT6 fault bitset
dt6 5 thermal-overload
dt6 5 open-circuit
```

Read a driver's identity from control-gear Bank 0:

```text
memread 5 0 3 8
```

Check Steinel Bank 2 global sensitivity byte:

```text
devmem read 0 2 4
```

Verify control-device DTR writes:

```text
dtrcheck 0 0 66
dtrcheck 0 1 2
```

Retire gear that has been physically removed from the bus:

```text
group forget 7
```

Use raw frames when helper verbs are suspect:

```text
raw C13102 len=24
raw C13004 len=24
raw 01FE3C len=24 wait
```

