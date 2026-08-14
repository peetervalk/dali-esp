# DALI Command Reference

Every verb, its arguments, and its named command tables, for both surfaces that
accept typed commands.

**Last reviewed:** 2026-08-14

Frame layouts and opcodes are in `dali_protocol.md`. Per-capability status —
shared API, host vector, real-bus result, exposure — is in
`dali_capability_matrix.md`. On a running device, `help`, `list <table>`, and
`schema` print the tables the parser actually dispatches on, so they cannot drift
from what the firmware accepts; this file can.

## The Two Surfaces

| Surface | Reached by | Verb set |
|---|---|---|
| Diagnostic shell | `tools/dali-shell`, or `nc <host> 2323`, with a `shell:` block in the YAML | Full |
| Serial CLI | UART0 on a native ESP-IDF build | Full — same code as the shell |
| Command console | ESPHome `text:` entity, usually "DALI Command" | Subset |

The shell and the serial CLI are one implementation reached by two transports:
the verb, its argument checking, its blocking transport, and its output are the
same code (`components/dali/dali_shell.c`).

The console is a different execution model, not a different language. It shares
the tokeniser, argument parsers, named tables, and reply decoding
(`components/dali/dali_cli.c`), so a verb, an argument form, and a command name
mean the same thing on both. What it cannot do is stream or block: a result is
one Home Assistant text state, and every console verb is one enqueue and one
completion because Core 0's loop may not block. That is the whole basis of the
split — see the availability column below, and
`dali_capability_matrix.md` for the reasoning per verb.

Console results land in the `command_result` text sensor, usually "DALI Command
Result":

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

## Syntax

```text
<verb> [parameters...]
```

Space-separated and lowercase. At most 8 tokens of 31 characters each. The whole
line is capped at 79 characters on the shell and 95 on the console. There is no
quoting, so every parameter is a single token. An over-long line or token is an
error rather than a silent truncation — a clipped line could otherwise become a
different, valid command.

Numbers may be decimal or `0x`-prefixed. A leading zero is decimal, not octal.

Every verb declares how many arguments it takes, and both bounds are checked
before the handler runs. A trailing token is rejected rather than ignored:
`level s1 100 junk` fails with a usage string.

### Targets

| Form | Range | Meaning |
|---|---:|---|
| `s<N>` | `0-63` | Short address |
| `<N>` | `0-63` | Short address, bare number |
| `g<N>` | `0-15` | Group |
| `b` | n/a | Broadcast |

`a<N>` is **not** accepted and fails as `bad target`. It was the console's
spelling before the shared tables; there is no alias, so a stored Home Assistant
script written against it stops working.

Queries should normally target one short address. Group and broadcast queries can
collide when several devices answer.

Input-device verbs take no target. They take the short address and the instance
as two separate arguments — `iquery 0 1 input-value`, not `iquery a0:1
input-value`. `iquery`, `iconfig`, `devmem`, and `dtrcheck` require a short
address and reject group and broadcast forms.

### Console results

| Result | Meaning |
|---|---|
| `OK` | Command or helper sequence was queued |
| `pending` | Asynchronous command queued, not yet complete |
| `err` | The stack rejected the command, or execution later failed |
| `queue full` | Scheduler queue was full; retry once traffic settles |
| `no reply` | A query expected a reply and none arrived |
| `<name>: N (0xHH)` | Query reply, decoded under the name that asked |
| `<name>: yes (0xFF)` | A yes/no query's reply |
| `<name>: malformed reply` | Something answered, but not as that command's reply kind |
| `0xHH flag,flag` | A status byte and the flags it sets |
| `TX OK` / `TX2 OK` | Raw frame sent once / twice, no reply awaited |
| `TX ERR N` / `TX2 ERR N` | Raw transmission failed with DALI error `N` |
| `RX N (0xHH)` | Raw frame sent with `wait`, reply byte received |
| `RX timeout` | Raw frame sent with `wait`, no reply |
| `usage: <verb> ...` | Wrong argument count, including a trailing token |
| `scan active` | A bus verb was refused while a scan runs |

`OK` means queued — not that the command executed, and not that the target
accepted it. Only the newest submitted command may update the result, so a late
callback from an older command cannot overwrite a newer result.

Replies are decoded by the shared `dali_cli_format_response()`, so a yes/no query
answers `yes` rather than `255` and a fade byte arrives split into its two
nibbles. Anything in Home Assistant parsing `command_result` must expect the
named form.

## Verb Index

| Verb | Shell / CLI | Console |
|---|:---:|:---:|
| `off` `max` `min` `level` `mask` | yes | yes |
| `up` `down` `step-up` `step-down` `step-off` `on-step` | yes | yes |
| `cont-up` `cont-down` `dapc-seq` `last` `scene` | yes | yes |
| `status` `query` | yes | yes |
| `config` `config-dtr0` | yes | yes |
| `special` | yes | yes, minus commissioning primitives |
| `dt6` | yes | yes |
| `dt8` | yes | no — held until real DT8 gear is available |
| `iquery` `iconfig` `vendor` | yes | yes |
| `raw` `raw2` `dtr` | yes | yes |
| `memread` `devmem` `dtrcheck` | yes | yes |
| `meminfo` | yes | no — walks a bank, needs a blocking transport |
| `queue` | yes | yes |
| `group forget` | no | yes — the cache it edits is the component's |
| `scan` `discover` `inventory` `export` `identify` | yes | no — buttons and text sensors instead |
| `commission` `instances` `sensor poll` `smoke` | yes | no |
| `events` `find switches` | yes | no |
| `help` `list` `schema` `query-list` `special-list` `config-list` | yes | no |
| `stats` `bus check` `capture` `trace` `read` `rxdebug` `reset` | yes | no |

## Gear Control

```text
off <target>
max <target>
min <target>
level <target> <0-254|mask>
mask <target>
up <target>
down <target>
step-up <target>
step-down <target>
step-off <target>
on-step <target>
cont-up <target>
cont-down <target>
last <target>
dapc-seq <target>
scene <target> <0-15>
```

| Verb | DALI command | Effect |
|---|---|---|
| `off` | OFF | Switch off |
| `max` / `min` | RECALL MAX / MIN LEVEL | Recall the configured window ends |
| `level` | DAPC | Direct arc power; `254` is maximum |
| `mask` | DAPC 255 | Leave the level unchanged |
| `up` / `down` | UP / DOWN | One fade step, over the gear's fade time |
| `step-up` / `step-down` | STEP UP / STEP DOWN | One level, no fade |
| `step-off` | STEP DOWN AND OFF | One level down, off at the minimum |
| `on-step` | ON AND STEP UP | Switch on if off, otherwise one level up |
| `cont-up` / `cont-down` | CONTINUOUS UP / DOWN | Fade toward max/min until stopped |
| `last` | GO TO LAST ACTIVE LEVEL | Return to the level before the last OFF |
| `dapc-seq` | ENABLE DAPC SEQUENCE | Open the DAPC sequence window |
| `scene` | GO TO SCENE | Recall a scene stored in the gear |

`mask` is a separate frame builder rather than level 255, so no level arithmetic
can reach 255 and silently stop meaning "set this level". Gear that is off
ignores `up` and `step-up`; `on-step` is the one that turns it on.

A scene's level is stored in the gear by
`config-dtr0 <target> set-scene <level> <scene>`.

None of these updates the light entity. The gear's level moves and Home Assistant
catches up on the next refresh pass, the same as after a wall switch.

```text
level g0 128
level s3 mask
step-up s3
cont-down g0
scene g0 3
off b
```

## Queries

```text
status <target>
query <target> [query-name] [param]
```

`status` sends QUERY STATUS and decodes the flags by name; `query <target>
status` returns the same line. On the shell, `query <target>` with no name at all
is shorthand for the same thing; the console requires a name.

```text
status s0        ->  status: 0x06 lamp-fail,arc-on
status s3        ->  status: 0x00 none
```

The 34 shared query names (`list query`):

| Name | Underlying command |
|---|---|
| `status` | QUERY STATUS |
| `present` | QUERY CONTROL GEAR PRESENT |
| `lamp-failure` | QUERY LAMP FAILURE |
| `lamp-on` | QUERY LAMP POWER ON |
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
| `extended-fade` | QUERY EXTENDED FADE TIME |
| `manufacturer-mode` | QUERY MANUFACTURER SPECIFIC MODE |
| `gear-failure` | QUERY CONTROL GEAR FAILURE |
| `next-device-type` | QUERY NEXT DEVICE TYPE |
| `scene-level <0-15>` | QUERY SCENE LEVEL — takes a parameter |
| `groups-0-7`, `groups-8-15` | QUERY GROUPS |
| `random-h`, `random-m`, `random-l` | QUERY RANDOM ADDRESS |
| `memory` | READ MEMORY LOCATION |
| `extended-version` | QUERY EXTENDED VERSION NUMBER |

```text
query s0 actual
query s0 fade
query s0 groups-0-7
query s0 scene-level 3
```

## Configuration

```text
config <target> <config-name> [param]
config-dtr0 <target> <config-name> <dtr0> [param]
```

The two verbs split by where the command gets its value. `config` is for commands
whose parameter is in the opcode, or that take none. `config-dtr0` is for
commands that read DTR0: it loads DTR0 and sends the command as one scheduler
sequence, so no other locally scheduled frame can replace DTR0 in between. Using
the wrong one is refused with a message naming the other, so a DTR0 command can
never be sent against an unset register.

The 19 shared config names (`list config`):

| Name | Verb | Param | Meaning |
|---|---|---|---|
| `reset` | `config` | none | Reset control gear variables |
| `store-actual-dtr0` | `config` | none | Store the actual level in DTR0 |
| `save-persistent` | `config` | none | Save persistent variables |
| `identify-device` | `config` | none | IDENTIFY DEVICE |
| `enable-write-memory` | `config` | none | Open the memory write gate |
| `add-group` | `config` | `0-15` | Add gear to a group |
| `remove-group` | `config` | `0-15` | Remove gear from a group |
| `remove-scene` | `config` | `0-15` | Clear one scene |
| `set-scene` | `config-dtr0` | `0-15` | Store DTR0 as one scene's level |
| `set-max-dtr0` | `config-dtr0` | — | Set maximum level from DTR0 |
| `set-min-dtr0` | `config-dtr0` | — | Set minimum level from DTR0 |
| `set-power-on-dtr0` | `config-dtr0` | — | Set power-on level from DTR0 |
| `set-failure-dtr0` | `config-dtr0` | — | Set system-failure level from DTR0 |
| `set-fade-time-dtr0` | `config-dtr0` | — | Set fade time from DTR0 |
| `set-fade-rate-dtr0` | `config-dtr0` | — | Set fade rate from DTR0 |
| `set-extended-fade-dtr0` | `config-dtr0` | — | Set extended fade time from DTR0 |
| `set-operating-mode-dtr0` | `config-dtr0` | — | Set operating mode from DTR0 |
| `set-short-address-dtr0` | `config-dtr0` | — | Set the short address from DTR0 |
| `reset-memory-dtr0` | `config-dtr0` | — | Reset the memory bank named by DTR0 |

`add-group` and `remove-group` require the group value. Short and group targets
are accepted; broadcast is rejected, because it makes the runtime group query
cache ambiguous. After a group-target edit, run a scan if that group has not
already been scan-verified.

```text
config-dtr0 s0 set-max-dtr0 200
config-dtr0 s0 set-fade-time-dtr0 4
config-dtr0 s0 set-scene 200 3
config s0 add-group 3
config s0 save-persistent
```

## Special Commands

```text
special <name> [param]
```

Not addressed: every device on the bus sees them.

| Name | Param | Meaning |
|---|---|---|
| `terminate` | none | End an initialise window another tool opened |
| `dtr0`, `dtr1`, `dtr2` | `0-255` | Load a DTR register |
| `ping` | none | DALI-2 presence ping |
| `compare` | none | Whether any device is in the current search selection |
| `verify-short` | `0-63` | Whether a device holds this short address |
| `query-short` | none | The selected device's short address |
| `enable-type` | `0-255` | ENABLE DEVICE TYPE for the next command |

The console refuses the nine commissioning primitives that
`dali_cli_special_is_commissioning()` marks — `initialise`, `randomize`,
`search-h/m/l`, `program-short`, `withdraw`, `write-memory`, and
`write-memory-nr` — with `commissioning special; use the native CLI`. Those are
the commands that can readdress a whole installation from one line typed into a
text box, and RANDOMISE cannot be undone. The shell runs them sequenced and
checked inside `commission`, subject to `allow_commissioning`.

`terminate` stays available on purpose: it is what closes a window another tool
opened, and withholding it would leave you holding the problem without the
remedy.

`enable-type` applies only to the very next command on the bus, which neither
surface can guarantee is yours. That is why `dt6` and `dt8` exist as verbs.

## Device Type 6 — LED gear

```text
dt6 <addr> <name> [dtr0]
```

One IEC 62386-207 command per line, sent as one scheduler sequence: the DTR0 load
if the command takes one, ENABLE DEVICE TYPE 6, and the command itself. A DT6
command that arrived without its enable would be read by the gear under its
default device type — a different command entirely.

DT6 commands only mean anything on gear that reports device type 6. Check with
`query <addr> device-type` first.

Configuration (sent twice, no reply):

| Name | DTR0 | Meaning |
|---|---|---|
| `ref-power` | none | Start a reference system power measurement |
| `enable-protector` | none | Enable the current protector |
| `disable-protector` | none | Disable the current protector |
| `select-curve` | `0` standard, `1` linear | Select the dimming curve |
| `store-fast-fade` | fast fade time | Store DTR0 as the fast fade time |

Queries (one decoded reply):

| Name | Reply | Meaning |
|---|---|---|
| `gear-type` | bitset | Gear type bits |
| `dimming-curve` | number | `0` standard (logarithmic), `1` linear |
| `operating-modes` | bitset | Supported operating modes |
| `features` | bitset | Supported DT6 features |
| `failure-status` | bitset | All eight failure flags as one byte |
| `short-circuit` | yes/no | LED short circuit |
| `open-circuit` | yes/no | LED open circuit |
| `load-decrease` | yes/no | Load decrease detected |
| `load-increase` | yes/no | Load increase detected |
| `protector-active` | yes/no | Current protector is acting |
| `protector-enabled` | yes/no | Current protector is enabled |
| `thermal-shutdown` | yes/no | Thermal shutdown active |
| `thermal-overload` | yes/no | Thermal overload with light-level reduction |
| `reference-running` | yes/no | Reference measurement in progress |
| `reference-failed` | yes/no | Last reference measurement failed |
| `operating-mode` | number | Current operating mode |
| `fast-fade` | number | Configured fast fade time |
| `min-fast-fade` | number | Physical minimum fast fade time |
| `version` | number | DT6 extended version number |

`failure-status` returns the raw bitset on the console; the per-flag decode is
printed only by the shell, where there is room for eight lines.

**`select-curve` changes how brightness maps to the bus.** The light layer
computes every brightness from the curve through `dali_dim_curve`, which
implements the standard logarithmic curve only. The component therefore drops its
cached level profile for that address and starts a refresh — a stale profile
misreports and miscommands in both directions. The static counterpart, for gear
whose curve query is not trustworthy:

```yaml
light:
  - platform: dali
    dali_id: dali_bus
    name: "Office"
    target_type: short
    target_address: 5
    dimming_curve: linear   # auto (default) | standard | linear
```

```text
dt6 5 dimming-curve
dt6 5 select-curve 1
dt6 5 failure-status
dt6 5 store-fast-fade 2
```

## Device Type 8 — colour

Shell and serial CLI only, until it has been run against real DT8 gear.

```text
dt8 <addr> <name> [v0] [v1] [v2]
dt8 <addr> colour <selector>
```

| Group | Names |
|---|---|
| Temporary set | `set-x`, `set-y`, `set-tc` (DTR0 low, DTR1 high), `set-primary`, `set-rgb`, `set-waf`, `set-rgbwaf-control` |
| Step | `x-step-up`, `x-step-down`, `y-step-up`, `y-step-down`, `tc-cooler`, `tc-warmer` |
| Apply | `activate`, `copy-report` |
| Store | `store-ty-primary`, `store-xy-primary`, `store-tc-limit`, `store-features`, `assign-colour`, `auto-calibration` |
| Query | `features`, `colour-status`, `colour-type-features`, `rgbwaf-control`, `assigned-colour`, `version` |
| 16-bit read | `colour <selector>` |

Selectors (`list selectors`): `x`, `y`, `tc`, `primary0`..`primary5`, `red`,
`green`, `blue`, `white`, `amber`, `free`.

`dt8 <addr> colour tc` performs the four-step 16-bit colour value read and prints
Kelvin as well as mirek.

## Input Devices — Part 103

```text
iquery <addr> <instance> <name> [dtr0]
iconfig <addr> <instance> <name> [v0] [v1] [v2]
```

Run `iquery <addr> <instance> type` first, and use Part 301, 303, or 304 names
only on an instance of type 1, 3, or 4 respectively.

`instance-config` takes its DTR0 selector as a fourth argument, and the load and
the read go out as one sequence. A DTR0 written by a separate command can be
replaced before the query that reads it arrives, so the two forms are not
interchangeable: a selector on a query that takes none is rejected, and a DTR0
query without one is refused.

### `iquery` names

| Name | Part / opcode | Meaning |
|---|---:|---|
| `type` | 103 / `0x80` | Instance type |
| `resolution` | 103 / `0x81` | Bit resolution |
| `error` | 103 / `0x82` | Instance error byte |
| `status` | 103 / `0x83` | Instance status byte |
| `event-priority` | 103 / `0x84` | Configured event priority |
| `enabled` | 103 / `0x86` | Whether the instance is enabled |
| `primary-group` | 103 / `0x88` | Primary instance group |
| `group1`, `group2` | 103 / `0x89`, `0x8A` | Additional instance groups |
| `event-scheme` | 103 / `0x8B` | Event source scheme |
| `input-value` | 103 / `0x8C` | Current input value |
| `input-value-latch` | 103 / `0x8D` | Latched input value |
| `event-filter0/1/2` | 103 / `0x90`-`0x92` | Event-filter bits 0-7, 8-15, 16-23 |
| `instance-config` | 103 / `0x93` | Takes a DTR0 configuration index |
| `available-types` | 103 / `0x94` | Available instance types |
| `pb-short-timer` | 301 / `0x0A` | Short timer |
| `pb-short-timer-min` | 301 / `0x0B` | Physical short-timer minimum |
| `pb-double-timer` | 301 / `0x0C` | Double-press timer |
| `pb-double-timer-min` | 301 / `0x0D` | Physical double-timer minimum |
| `pb-repeat-timer` | 301 / `0x0E` | Long-press repeat timer |
| `pb-stuck-timer` | 301 / `0x0F` | Stuck timer |
| `occ-capabilities` | 303 / `0x29` | Range/sensitivity capability bits |
| `occ-detection-range` | 303 / `0x2A` | Configured detection range |
| `occ-sensitivity` | 303 / `0x2B` | Configured sensitivity |
| `occ-deadtime` | 303 / `0x2C` | Deadtime |
| `occ-hold-timer` | 303 / `0x2D` | Hold timer |
| `occ-report-timer` | 303 / `0x2E` | Report timer |
| `occ-catching` | 303 / `0x2F` | Whether movement catching is active |
| `light-hysteresis-min` | 304 / `0x3C` | Absolute minimum hysteresis band |
| `light-deadtime` | 304 / `0x3D` | Deadtime timer |
| `light-report-timer` | 304 / `0x3E` | Report timer |
| `light-hysteresis` | 304 / `0x3F` | Hysteresis percentage |

The removed `hysteresis` and `deadtime-gen` names were never generic Part 103
queries; `0x82` and `0x83` are QUERY INSTANCE ERROR and QUERY INSTANCE STATUS.

### `iconfig` names

> **Hardware validation incomplete.** The opcode mapping has been independently
> audited, but these writes have not been round-trip validated on the project
> hardware. Read with `iquery`, change one parameter, read it back. `OK` means
> queued — it does not prove the device accepted the value.

Commands that consume DTR0 load it first and then use the send-twice path.

| Name | Part / opcode | Value | Meaning |
|---|---:|---|---|
| `enable` | 103 / `0x62` | none | Enable the instance |
| `disable` | 103 / `0x63` | none | Disable the instance |
| `set-event-priority` | 103 / `0x61` | `2-5` | Event priority |
| `set-primary-group` | 103 / `0x64` | `0-31` or `255` | Set or clear the primary group |
| `set-group1` | 103 / `0x65` | `0-31` or `255` | Set or clear group 1 |
| `set-group2` | 103 / `0x66` | `0-31` or `255` | Set or clear group 2 |
| `set-event-scheme` | 103 / `0x67` | `0-4` | Event source scheme |
| `set-event-filter` | 103 / `0x68` | `<v0> <v1> <v2>` | eventFilter bits 0-7, 8-15, 16-23 |
| `set-instance-type` | 103 / `0x69` | `0-31` | Instance type |
| `set-instance-config` | 103 / `0x6A` | `<index> <low> <high>` | DTR0 selects, DTR2:DTR1 hold the value |
| `pb-set-short-timer` | 301 / `0x00` | `10-255` | 20 ms units |
| `pb-set-double-timer` | 301 / `0x01` | `0` or `10-100` | 20 ms units; `0` disables |
| `pb-set-repeat-timer` | 301 / `0x02` | `5-100` | 20 ms units |
| `pb-set-stuck-timer` | 301 / `0x03` | `5-255` | 1 s units |
| `occ-catch-movement` | 303 / `0x20` | none | Start movement catching; single send |
| `occ-cancel-hold` | 303 / `0x24` | none | Cancel the hold timer; single send |
| `occ-set-hold-timer` | 303 / `0x21` | `0-254` | 10 s units; `0` selects 1 s |
| `occ-set-report-timer` | 303 / `0x22` | `0-255` | 1 s units; `0` disables |
| `occ-set-deadtime` | 303 / `0x23` | `0-255` | 50 ms units; `0` disables |
| `occ-set-detection-range` | 303 / `0x25` | `0-100` | If supported |
| `occ-set-sensitivity` | 303 / `0x26` | `0-100` | If supported |
| `light-set-report-timer` | 304 / `0x30` | `0-255` | 1 s units; `0` disables |
| `light-set-hysteresis` | 304 / `0x31` | `0-25` | Percentage |
| `light-set-deadtime` | 304 / `0x32` | `0-255` | 50 ms units; `0` disables |
| `light-set-hysteresis-min` | 304 / `0x33` | `0-255` | Absolute minimum band height |

Every value is range-checked against the applicable command definition before
anything is sent. An out-of-range byte is refused with the accepted range rather
than transmitted: some gear stores it, and the mistake then surfaces later as odd
behaviour instead of as a rejected command.

Before setting a Part 301 short or double timer, query `pb-short-timer-min` or
`pb-double-timer-min`. The standard-wide floor is enforced here, but a device may
reject a value below its own physical minimum.

```text
iquery 0 1 type
iquery 0 1 occ-hold-timer
iconfig 0 1 occ-set-hold-timer 20
iquery 0 1 occ-hold-timer
```

## Vendor Helpers

```text
vendor lunatone <addr> <instance> <name>
vendor steinel <instance> <raw>
```

Lunatone sensor instance queries describe how to scale that instance's raw input
value. They are 24-bit instance frames but only mean anything on Lunatone
hardware: `multiplicator`, `divisor`, `offset-msb`, `offset-lsb`, `offset-mult`,
`offset-div`, `unit`.

`vendor steinel` transmits nothing. It applies the same conversion the sensor
platform applies to a raw Steinel HF 360 II reading you already have, which is
why it stays answerable during a scan.

| Instance | Reports | Conversion |
|---:|---|---|
| `0` | Illuminance, lux | raw scale 0.01 |
| `1` | Motion | — |
| `2` | Temperature, °C | `(raw - 50) / 10` |
| `3` | Relative humidity, % | `raw / 2` |

```text
vendor steinel 2 262     ->  temperature: 21.2 C
vendor steinel 3 110     ->  humidity: 55.0 %
```

## Memory

Two classes of device, two sets of opcodes, two verbs:

| Verb | Frames | Reads from |
|---|---|---|
| `memread`, `meminfo` | 16-bit Part 102 | Control gear — drivers, ballasts |
| `devmem read` / `devmem write` | 24-bit Part 103 | Control devices — sensors, switches |

They are named apart rather than overloaded because picking the wrong one
addresses a different class of device entirely. 

```text
memread <addr> <bank> <offset> [count]      # count 1-14, default 1
meminfo <addr>                              # Bank 0 identity; shell only
devmem read <addr> <bank> <offset> [count]  # count 1-16, default 1
devmem write <addr> <bank> <offset> <value> # bank 1-255; bank 0 is refused
dtrcheck <addr> <0|1|2> <0-255>
```

For `memread`, DTR1, DTR0, and every read frame go out as one sequence. READ
MEMORY LOCATION increments the device's offset cursor, so an interrupted and
retried read would return the bytes after the ones asked for; keeping the
sequence intact is what prevents that. The result lists every byte in hex, so a
multi-byte read is not reduced to its last value. The same holds for
`devmem read`.

`devmem write` unlocks the bank by writing `0x55` to lock byte offset `0x02`,
then writes the requested byte. Its seven logical steps are one queue entry and
run contiguously relative to other traffic from this controller. The no-reply
write does not acknowledge or verify that the device committed the byte, and an
external bus master is not excluded. **No write path reads its value back.**
Treat every memory write as unverified until a following read confirms it.

`meminfo` is the one memory verb the console lacks: reading a Bank 0 identity
means walking the bank and deciding what to ask next from what came back, which
needs the blocking transport. The scan already reports Bank 0 identity per
device.

`dtrcheck` loads a **control-device** DTR and reads it straight back in one
sequence. A DTR load produces no backward frame, so nothing else confirms the
device took the value; this separates "the DTR never took the value" from "the
command that consumes it was ignored".

```text
memread 5 0 3 8            # gear Bank 0: GTIN at 0x03, ident number at 0x0B
devmem read 0 2 4
devmem write 0 2 4 255
devmem read 0 2 4
dtrcheck 0 0 66            # expect: read 66 (0x42)
```

## Raw Frames

```text
raw <hex> len=<16|24> [wait]
raw2 <hex> len=<16|24>
dtr <0|1|2> <0-255>
```

`raw` sends one arbitrary forward frame; `wait` waits for an 8-bit backward
frame. `raw2` sends the frame twice through the scheduler's send-twice path,
which is the only way to meet the 100 ms window — two manually typed `raw`
commands cannot, so a send-twice command entered that way is not the command the
standard describes. `raw2` takes no `wait`: a send-twice command is a
configuration write, not a query. A value that does not fit the stated width is
rejected rather than transmitted as a differently framed command.

`dtr` loads one **control-gear** DTR by broadcast. A DTR is consumed by whichever
command reads it next, so a value loaded this way survives only if nothing else
transmits in between — including this component's own refresh queries. Prefer the
verbs that carry their own load and cannot be interrupted: `config-dtr0`,
`iconfig`, `dt6`, and the DTR0 form of `iquery`.


## Discovery and Commissioning

Shell and serial CLI only. Home Assistant reaches the same walk through the
**Scan DALI Bus**, **Find Couplers**, and **Identify** buttons.

```text
scan                                    # short addresses, brief output
discover                                # short addresses, device types, groups
inventory                               # reprint the last discover, no bus traffic
export inventory                        # the same result as JSON
export config                           # discovered devices as a YAML dali: block
identify <addr>                         # blink one fixture
find switches [seconds]                 # listen for events and map switches
events                                  # drain queued Part 103 events
instances <addr>                        # what a control device offers
sensor poll <addr> [instance]           # read an input instance's value
smoke <addr>                            # read/write/read-back check
commission unaddressed [first] [max]    # assign short addresses
```

Commissioning is dependable only with a single unaddressed device on the bus: the
COMPARE collision inversion recorded in `current_status.md` is unfixed. Over TCP,
`commission` and the nine commissioning specials are refused unless the YAML sets
`allow_commissioning: true`, because the port is unauthenticated. See
`commissioning_readme.md` for the workflow.

A long verb prints as it goes and holds the bus for as long as it runs.
Disconnecting drops it: the device notices between bus steps and stops.

## Diagnostics

```text
help                       # the whole verb table
list <table>               # query special config dt6 dt8 selectors iquery iconfig vendor
schema                     # every command table as JSON — this is what drives tab completion
query-list | special-list | config-list
stats                      # PHY, RX, and scheduler counters
queue [reset]              # scheduler queue admission
bus check                  # RX level, scheduler state, fault counters
capture start|stop|clear|status|export
trace on|off               # per-frame trace logging
read                       # the last received frame
rxdebug                    # last malformed RX timing snapshot
reset                      # reset PHY, scheduler, and diagnostic state
```

`help`, `list`, and `schema` are generated from the table the parser dispatches
on, so they cannot drift from what the CLI accepts.

### `queue [reset]` — both surfaces

| Field | Meaning |
|---|---|
| `d=<n>/<cap>` | Entries queued now, and capacity. The executing entry has been popped and is not counted |
| `hw=<n>` | Largest depth reached. At capacity means admission came within one submission of failing |
| `ok=<n>` | Submissions accepted |
| `full=<n>` | Submissions refused because the queue was full |
| `busy=<n>` | Submissions refused by a pending scheduler reset barrier |

A typical idle result is `d=0/16 hw=3 ok=214 full=0 busy=0`. `reset` clears the
high-water mark and counters, rebasing high-water on current depth rather than
zero so the reading never understates live occupancy.

A non-zero `full` or `busy` is dropped work, not deferred work: the scheduler
never retries a refused submission for the caller. Light entities and the refresh
pump keep their own state and retry; a headless dispatch action refused this way
is discarded rather than replayed against a stale physical context. The same
counters are logged as warnings whenever they advance.

### `group forget <addr> [group]` — console only

Retires a departed member from the group-membership cache — the table that picks
which short address a group light polls for its state. Omit the group to retire
the address from every group.

A bus scan deliberately keeps the memberships of gear it did not see, so that a
device which is merely offline is not dropped. The consequence is that physically
removed gear stays in the cache forever, still eligible as a group's query
target, and no scan can clear it. This is the explicit way to say the gear is
gone. It sends nothing — the gear cannot answer — and only edits the cache, which
is then persisted to flash. Reports `not a member` when there was nothing to
remove.

For gear that is still present, use `config <target> remove-group <g>` instead:
that reconfigures the device itself.

```text
group forget 7          # remove address 7 from every group
group forget 7 3        # remove address 7 from group 3 only
```

### Verbs answerable during a scan

`queue`, `group`, and `vendor steinel` generate no bus traffic, so they are the
only console verbs accepted while a scan is running — which is when queue
pressure and a stale group cache are most worth inspecting.

## Recipes

Diagnose a driver that reports the wrong brightness:

```text
query s5 device-type       # DT6 gear answers 6
dt6 5 dimming-curve        # 0 standard, 1 linear
query s5 min-level
query s5 max-level
```

If the curve is not what the entity assumes, either correct the gear
(`dt6 5 select-curve 0`) or pin the entity with `dimming_curve:`. Both drop the
cached profile; the first also re-reads it.

Check an LED driver for faults:

```text
status s5                  # Part 102 status flags
dt6 5 failure-status       # DT6 fault bitset
dt6 5 thermal-overload
dt6 5 open-circuit
```

Check group membership:

```text
query s0 groups-0-7
query s0 groups-8-15
```

Set and verify an occupancy hold timer:

```text
iquery 0 1 occ-hold-timer
iconfig 0 1 occ-set-hold-timer 20
iquery 0 1 occ-hold-timer
```

Verify control-device DTR writes before trusting a memory read:

```text
dtrcheck 0 0 66
dtrcheck 0 1 2
```

Use raw frames when helper verbs are suspect:

```text
raw C13102 len=24
raw C13004 len=24
raw 01FE3C len=24 wait
```


