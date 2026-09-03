# DALI Command Reference

Every verb, its arguments, and its named command tables, for both surfaces that
accept typed commands.

**Last reviewed:** 2026-09-03

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
the tokenizer, argument parsers, named tables, and reply decoding
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
`level a1 100 junk` fails with a usage string.

### Targets

| Form | Range | Meaning |
|---|---:|---|
| `a<N>` | `0-63` | Short address |
| `<N>` | `0-63` | Short address, bare number |
| `g<N>` | `0-15` | Group |
| `b` | n/a | Broadcast |

`s<N>` is **not** accepted and fails as `bad target`. It was the spelling v1.2.0
shipped; there is no alias, so a stored Home Assistant script written against it
stops working.

Queries should normally target one short address. Group and broadcast queries can
collide when several devices answer.

Input-device verbs take no target. They take the short address and the instance
as two separate arguments — `iquery 0 1 input-value`, not `iquery a0:1
input-value`. `iquery`, `iconfig`, `devmem`, and `dtrcheck` require a short
address and reject group and broadcast forms. `quiescent` is the exception: it
is device-level rather than instance-level, so it takes no instance, and its
target is a short address or the literal `all`.

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
| `config` `config-dtr0` | yes | yes, minus `set-short-address-dtr0` |
| `address` | yes | no — a multi-frame workflow that claims the bus, like `scan` |
| `special` | yes | yes, minus commissioning primitives |
| `dt6` | yes | yes |
| `dt8` | yes | no — held until real DT8 gear is available |
| `iquery` `iconfig` `vendor` | yes | yes |
| `raw` `raw2` `dtr` | yes | yes |
| `memread` `devmem` `dtrcheck` | yes | yes |
| `quiescent` | yes | yes |
| `meminfo` | yes | no — walks a bank, needs a blocking transport |
| `queue` | yes | yes |
| `group forget` | no | yes — the cache it edits is the component's |
| `scan` `discover` `inventory` `export` `identify` | yes | no — buttons and text sensors instead |
| `commission` `instances` `sensor poll` `smoke` | yes | no |
| `backup` `restore` (incl. `restore groups`) | yes | no — all answer in a block of lines |
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
level a3 mask
step-up a3
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
status a0        ->  status: 0x06 lamp-fail,arc-on
status a3        ->  status: 0x00 none
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
query a0 actual
query a0 fade
query a0 groups-0-7
query a0 scene-level 3
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
| `set-short-address-dtr0` | `config-dtr0` | — | Set the short address from DTR0, **encoded** — see [Encoded short addresses](#encoded-short-addresses) |
| `reset-memory-dtr0` | `config-dtr0` | — | Reset the memory bank named by DTR0 |

`add-group` and `remove-group` require the group value. Short and group targets
are accepted. Broadcast is refused on both surfaces with
`no broadcast group config`, because the runtime group query cache cannot
represent the result and the undo is not symmetric: a broadcast REMOVE empties
the group, including the members that were there first. The refusal comes from
`dali_cli_config_rejects_broadcast()` rather than from either front end, so the
console and the shell cannot disagree about it. `raw2` still sends the frame
when that is genuinely what you want.

Either surface applies what the edit invalidated: the runtime group-membership
table, any cached level profile, and a light refresh. After a **group-target**
edit, run a scan if that group has not already been scan-verified — the
affected set is not computable from an unverified source group. A short address
that moved needs a scan for the same kind of reason: the new address is not
knowable from the command, so caches keyed by the old one are dropped and a
warning is logged rather than a new address guessed.

```text
config-dtr0 a0 set-max-dtr0 200
config-dtr0 a0 set-fade-time-dtr0 4
config-dtr0 a0 set-scene 200 3
config a0 add-group 3
config a0 save-persistent
config-dtr0 a0 set-short-address-dtr0 27   # a0 -> a13   ((13<<1)|1 = 27)
```

`set-short-address-dtr0` is the one config name that changes which address a
device answers to, and its DTR0 value is the encoded form, not the address. It
is gated exactly as the commissioning specials are: a shell session refuses it
without `allow_commissioning: true`, and the console refuses it outright with
`commissioning config; use the native CLI`. Both spellings are covered — with
DTR0 already holding `0xFF`, plain `config <t> set-short-address-dtr0`
de-addresses exactly as the `config-dtr0` form does, so gating only one of them
would be a hole rather than a difference.

## Address and Group Membership

```text
address <aN> set <aM>
address <aN> add <gN>
address <aN> remove <gN>
```

The checked way to change what one piece of gear answers to. DALI addresses gear
three ways — one short address, up to sixteen group addresses, and broadcast —
and this verb changes the first two for a single unit. Every argument is written
the way a target is, so the prefix says which kind of address is meant and
`address a5 add a13` is refused rather than guessed at.

It stands to `config` and `config-dtr0` as `commission` stands to the addressing
specials: same frames, plus the checks that make them safe to type. The raw
spellings are unchanged and still available.

| Subverb | Sends | Checks |
|---|---|---|
| `set <aM>` | DTR0 + SET SHORT ADDRESS, one sequence | destination is empty and source answers, **before**; destination answers and source is silent, **after** |
| `add <gN>` | ADD TO GROUP | reads both group bytes back and prints the resulting membership |
| `remove <gN>` | REMOVE FROM GROUP | the same read-back |

```text
> address a5 set a13
address: a5 -> a13 (DTR0=27)
address: a13 confirmed, a5 silent

> address a5 add g3
address: a5 is in g1 g3
```

The subject must be a single short address. Every arm reads its result back off
the bus, and a group or broadcast subject has no single answer to read — the
collision that produces is indistinguishable from silence. Multi-unit group
edits stay on `config g<N> add-group`, where a scan is needed afterwards anyway.

A destination that cannot be shown to be free stops the move. Undecodable
activity in the reply window (`DALI_ERR_RX_ACTIVITY`) is what two units sharing
an address sound like, so it is reported as "cannot tell" rather than read as
free:

```text
> address a5 set a13
address: a13 already answers; refusing to move a5 onto it
> address a5 set a13
address: cannot tell whether a13 is free (rx-activity); nothing sent
```

Because a confirmed move names both ends, the integration can follow it. A
re-address through `address` moves the group-membership bookkeeping with the
gear and costs no rescan; `config-dtr0 set-short-address-dtr0` carries its
destination out of band, so it still drops the caches and warns. What cannot
follow is an entity configured in YAML against the old address — that is logged,
not guessed at.

`set` is gated exactly as `config <t> set-short-address-dtr0` is: refused
without `allow_commissioning: true`. `add` and `remove` are gated on neither
spelling. The whole verb is absent from the console, like every other verb that
claims the bus for a multi-frame workflow.

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
| `verify-short` | `0-255` | Whether the selected device holds this **encoded** short address |
| `query-short` | none | The selected device's short address |
| `enable-type` | `0-255` | ENABLE DEVICE TYPE for the next command |

The console refuses the nine commissioning primitives that
`dali_cli_special_is_commissioning()` marks — `initialise`, `randomise`,
`search-h/m/l`, `program-short`, `withdraw`, `write-memory`, and
`write-memory-nr` — with `commissioning special; use the native CLI`. Those are
the commands that can readdress a whole installation from one line typed into a
text box, and RANDOMISE cannot be undone. The shell runs them sequenced and
checked inside `commission`, subject to `allow_commissioning`.

The same rule reaches one name in the config table:
`set-short-address-dtr0` re-addresses gear as effectively as `program-short`
does, so `dali_cli_config_is_commissioning()` marks it and both surfaces treat
it the same way. See [Configuration](#configuration).

`terminate` stays available on purpose: it is what closes a window another tool
opened, and withholding it would leave you holding the problem without the
remedy.

`verify-short` and `query-short` answer about the device *selected* inside an
initialisation window — `verify-short` about whatever `program-short` last
wrote, which is how `commission` uses it. Neither is a way to ask whether a
short address is free on a working bus; `scan` and `status a<N>` are.

`enable-type` applies only to the very next command on the bus, which neither
surface can guarantee is yours. That is why `dt6` and `dt8` exist as verbs.

### Encoded short addresses

A short address carried as a command's **data byte** is encoded
`(address << 1) | 1`; `0xFF` means no short address. That applies to
`program-short`, `verify-short`, the reply from `query-short`, the address
form of `initialise`, and the DTR0 value for `set-short-address-dtr0`. It does
not apply to the `a<N>` target form, which is an address byte and is written
as the plain number.

| Short address | Encoded byte |
|---:|---|
| `a0` | `0x01` (1) |
| `a1` | `0x03` (3) |
| `a5` | `0x0B` (11) |
| `a10` | `0x15` (21) |
| `a63` | `0x7F` (127) |
| none | `0xFF` (255) |

Nothing converts for you, and nothing can reject the mistake:
`special program-short 5` is a well-formed frame that programs short address
**2**. An even value has bit 0 clear and is not a valid short address at all.
`initialise`, `program-short` and `verify-short` therefore say what their
parameter means before they send it, which is what catches a value typed as if
it were the address:

```text
> special program-short 27
special: 27 is the encoded form of a13

> special program-short 5
special: 5 is not a valid encoded short address
special: a5 encodes as 11
```

The frame goes out either way — sending exactly what you typed is what `special`
is for — so the echo tells you what you just did rather than preventing it. The
value is that you find out on the line you typed instead of at the next `scan`.
`address a<N> set a<M>` is the spelling that checks first and refuses.

`initialise` takes `0` for all control gear, `255` for gear with no short
address, or an encoded address to open the window for one device. Its `0` is the
costly one to misread — typed as though it meant a0 it opens the addressing
window on the whole bus — so the echo names the selection rather than decoding a
number:

```text
> special initialise 0
special: 0 opens the window for every control gear on the bus, not a0 -- a0 is 1

> special initialise 27
special: 27 opens the window for a13 only

> special initialise 6
special: 6 selects nothing -- 0 is every gear, 255 is unaddressed gear, anything else is an encoded short address
special: a6 encodes as 13
```

An even parameter other than `0` selects no gear at all: the window opens for
nobody, `compare` answers nothing, and the walk that follows looks like an empty
bus rather than a typo.

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
quiescent on|off <addr|all>             # silence control-device events
smoke <addr>                            # read/write/read-back check
commission unaddressed [first] [max]    # assign short addresses
```

`commission` only ever addresses gear that has none, so it is the verb for new
gear and never the verb for a change. Re-grouping, re-addressing, and retiring
a fixture are `config` and `config-dtr0` commands — see the recipes below and
the reconfiguration section of `commissioning_readme.md`.

`quiescent` sends the Part 103 device-level `START`/`STOP QUIESCENT MODE`
(opcodes `0x1D`/`0x1E`, instance byte `0xFE`, send-twice). `all` is address byte
`0xFF` — every control device at once, which is the form worth having, since the
point is a quiet bus rather than one quiet sensor. Control gear is untouched:
lights keep responding while sensors and wall switches go silent.

Two things an operator has to hold in their head, because nothing enforces them:
a quiesced device reports no events, so anything driven by one stops updating
until `quiescent off` releases it; and nothing in this project tracks the state
or releases it on exit, so a forgotten `quiescent on all` leaves the installation
looking broken. Whether the standard also ends the state on its own timer is not
established here — treat `off` as the only thing that reliably releases it.
Host-tested; no bus has run it.

Every operator-driven walk now takes this bracket on its own behalf, so an
operator rarely needs the verb for a scan: `scan`, `discover`, both
commissioning pre-scans, the commissioning post-scan, `backup save` and the
`restore` refresh all broadcast `START QUIESCENT MODE`, settle, walk, and
release on the way out — including when the walk was cancelled or failed.
Sensors are therefore silent for the length of a walk, which is minutes on a
full bus, and any automation driven by them stops updating for that time. If a
release fails the shell says so and `quiescent off all` is the fix. Two things
stay outside it: `find switches`, whose whole purpose is listening for events,
and the integration's own periodic scan, which runs with nobody present to
accept a silent installation.

Commissioning remains hardware-dependable only with a single unaddressed device
on the bus. The receive path now attributes observations to a precise
TX-end-relative reply window — opening at 5.5 ms for undecodable activity, which
is the case `COMPARE` turns on, and closing at 27 ms — and distinguishes three
cases during `COMPARE`:

- silence is NO;
- qualified, response-like malformed activity is `DALI_ERR_RX_ACTIVITY`, which
  `COMPARE` alone treats as YES;
- ambiguous malformed activity or RX overflow is an error and aborts the run.

This fixes the software-side collision inversion recorded in `project_log.md`,
but overlapping replies and the activity qualifier have host coverage only; they
have not been validated as physical-bus collision detection. Do not rely on
multi-device commissioning until that hardware validation is complete. A run now brackets itself with broadcast
`START`/`STOP QUIESCENT MODE`, so control devices are silent for its duration
and cannot put an event frame into a COMPARE reply window. The release is
unconditional, so a run also releases a quiescence started by hand with
`quiescent on all`; if the release fails, the shell says so and `quiescent off
all` is the fix.

A Part 103 `TERMINATE` is bracketed with it — before `INITIALISE`, again
immediately after, and in the cleanup unwind. It covers the half quiescence does
not: quiescent mode stops a control device transmitting on its own initiative,
but does not stop one entering its own addressing state when it observes the
Part 102 `INITIALISE`, nor answering a `COMPARE` it was addressed with. Nothing
acknowledges it, so the shell prints a line only when it could not be sent.
There is no verb for it; the Part 102 `special terminate` is unrelated and
addresses gear.

Over TCP, `commission` and the nine commissioning specials are refused unless the
YAML sets `allow_commissioning: true`, because the port is unauthenticated. See
`commissioning_readme.md` for the workflow.

A long verb prints as it goes and holds the bus for as long as it runs.
Cancellation or a TCP disconnect is noticed between ordinary bus steps. Once the
opening atomic sequence has been handed to the transport, however, `INITIALISE`
may already have reached the bus. Cleanup therefore bypasses the cancellation
gate and attempts a final Part 102 `TERMINATE` before returning. The original
operation error remains primary; if the cleanup transmission also fails, the
shell reports that separately and warns that the initialisation state is unknown.
`TERMINATE` is cancellation-safe in the sense that it is still attempted, not
that delivery can be guaranteed after a bus or transport failure. The Part 103
`TERMINATE` and the quiescence release run through the same unwind and carry the
same meaning.

A run that assigned anything, hit a duplicate, or failed after reaching
`INITIALISE` then re-scans and checks itself; `commission devices` does the same
in the control-device address space. `post-scan confirmed N of M assignment(s)`
is the line to read. An assigned address that comes back `contested` means two
units hold it, and an address reported `occupied, unrecorded` was written to by a
run that ended before it recorded the assignment — it is commissioned, and the
run's own list does not say so. See `commissioning_readme.md` for that output and
for what an equal random address looks like while the run is still going.

## Backup and Restore

Shell and serial CLI only; neither verb is on the **DALI Command** text entity,
because the answer to both is a block of lines rather than one text state.

```text
backup save                             # scan the bus and record it
backup status                           # what is held, entry by entry
backup export                           # print it as the import script
backup import begin|<hex>...|end|abort  # read one back in
restore plan                            # what it would take to match the backup
restore apply                           # do it
restore groups                          # the same for gear group membership
restore groups apply                    # do it
```

`backup save` scans both address spaces and records, per short address, the
8-byte **identification number** at Bank 0 offset `0x0B` for the unit holding it
— plus its GTIN and, for gear, its group mask. That number is the anchor: it is
the one property of a unit that no addressing operation changes, which is what
makes a snapshot taken before a commissioning run enough to undo one. It is not
the 24-bit random address RANDOMISE generates, which is temporary. An address whose
identity could not be read is still recorded, and `backup save` names it on the
spot, because a fixture that cannot be put back is something to learn before the
restore rather than during it.

`restore plan` re-scans, matches each backup entry to the unit now holding its
identification number, and prints the moves that would put every one back. It is
read-only and needs no policy. `restore apply` executes them, and is gated
exactly as `commission` is: a shell session refuses it without
`allow_commissioning: true`.

A move is a plain addressed `SET SHORT ADDRESS DTR0` — DTR0 and the command in
one contiguous sequence, in whichever address space the entry came from.
**`restore` opens no `INITIALISE` window.** Nothing it sends can leave the bus
in a state that needs terminating, so it is safe to run on a live installation
and safe to interrupt: `apply` stops at the first failed move, and re-running
`restore plan` against the bus as it now stands is the recovery. A plan the
planner marks `incomplete` is refused rather than partially applied.

Cycles are handled. Two units that need to swap addresses cannot both move
directly, so the plan stages one through a free address and places it on a later
step; a cycle with no free address to stage through fails closed rather than
overwriting. Conflicts — a unit absent from the backup, a recorded unit missing
from the bus, an unreadable identity, two units sharing an identification number
— are reported and never moved, and blocking cascades to anything queued behind
the blocked unit.

### `restore groups` — a different repair

Group membership lives in each gear's own non-volatile memory, keyed to the gear
and not to the address it answers on. A commissioning walk changes short
addresses and nothing else, **so after a re-address the groups are already
right** and `restore groups` will report nothing to do. It exists for the case
where the membership itself was destroyed: a `config <target> reset`, a driver
that lost its memory, or a group-addressed edit that emptied more than it meant
to.

Two things follow from that, and both are why it is a separate verb rather than
part of `restore apply`:

- **It does not depend on the addresses having been restored, or on their ever
  being restored.** Each gear is matched by identification number and the edits
  are addressed to wherever it answers *now*. Running it on a freshly scrambled
  bus is correct; so is running it on a restored one. When the two differ the
  plan prints both, as `a7 (backup a3)`.
- **It is destructive in a way an address move is not.** A move is undone by
  moving back; a removed group membership is only recovered from a record of
  what it was. A backup taken before a deliberate regrouping will undo that
  regrouping — which is why the plan prints the full mask on both sides per
  fixture rather than a count of edits, and why `restore apply` must never reach
  it.

```text
restore groups: 4 matched, 2 already correct, 2 change(s)
  1. a5 now none -> g1 g3
  2. a9 (backup a2) now g0 g1 -> g1 g4
restore groups: run 'restore groups apply' to execute
```

`apply` sends plain addressed `ADD TO GROUP` / `REMOVE FROM GROUP` for the bits
that differ, additions first so a fixture is never momentarily in no group at
all, then reads `QUERY GROUPS 0-7` / `8-15` back once per gear. The read-back is
not optional: group commands are unacknowledged, so a driver that took the edits
and one that ignored them are indistinguishable until something asks. A gear
whose mask does not match afterwards is reported as `MISMATCH` and counted
separately from a transport error.

It refuses to guess in two cases, both reported and neither written:

| Reported | Meaning |
|---|---|
| `no group data in backup` | the backup has the gear but never read its membership. Treating that silence as "no groups" would issue `REMOVE FROM GROUP` for every group it is in now |
| `groups unreadable` | the gear was identified but its `QUERY GROUPS` did not answer. Writing the recorded mask blind would add the right groups without removing the wrong ones |

The remaining conflict kinds are the address planner's and mean the same things.
**Control gear only** — IEC 62386-103 control devices have their own group
scheme, which the scan does not read and this does not touch. Scenes are not
captured at all.

### Keeping a backup off the device

Where the backup lives depends on the front end. The ESPHome shell persists it
to flash, so it survives a reboot and `backup status` says whether what is held
came from storage or from a `backup save` this session. **The native serial CLI
has no persistent store**: there, a saved backup lives until reboot, and
`backup export` is the only way to keep one.

`backup export` prints the snapshot as the `backup import` script that
reproduces it:

```text
backup: 46 byte(s); the lines below re-import it
backup import begin
backup import 44424B31010200000000000000000A 1B2C3D4E5F60718293A4B5C6D7E8F9
backup import 0A1B2C3D4E5F60718293A4B5C6D7E8 F9
backup import end
```

`44424B31` is the format magic, `01` the version and `02` the entry count; the
rest is entry data, printed 15 bytes to a token and two tokens to a line.

Redirect it to a file, paste the file back. The chunking is not decorative: a
full snapshot is 4880 hex characters against an 80-character line limit, so the
blob cannot arrive in one piece however it is spelled, and printing it as one
long line would leave the operator to re-chunk it by hand.

`import` is a short mode. `begin` opens it, each `backup import <hex> <hex>`
line appends, `end` decodes and installs, `abort` discards. While it is open,
`backup save`, `backup status`, `backup export` and both `restore` verbs refuse
— they share the staging buffer, and a `backup save` typed in the middle of an
82-line paste would otherwise destroy it silently. A chunk that does not parse
discards the whole import rather than being skipped, because a blob missing a
line in the middle can still decode into a plausible-looking snapshot that moves
fixtures to the wrong addresses.

Nothing an import can contain damages the backup already held: the blob is
validated in full — magic, version, entry count, exact length, and every entry's
address space and short address — before the first byte is written.

**No part of this has met a bus.** Both planners, the snapshot format, and the
blob's rejection paths have host vectors; the moves and the group edits have
been transmitted nowhere. See `commissioning_readme.md` for the workflow this
belongs to.

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

### Reading a `capture export`

The ring holds 128 records. A `discover` generates roughly 1900, so a full scan
can never be captured — only its tail, with `dropped` naming what was lost.
**Capture narrowly**: `capture start`, one command, `capture export`. Six
records that answer the question beat 128 that happen to be the end of
something else.

| Field | Meaning |
|---|---|
| `kind` | `tx` or `rx` |
| `timestamp_us` | Free-running microsecond clock; only differences are meaningful |
| `raw`, `raw_bits` | The frame and its width — 16 or 24 forward, 8 backward |
| `since_tx_us` | On an `rx` record: the backward frame's **last edge**, measured from the preceding TX **bus release** |

`since_tx_us` is the field worth being careful with, because neither end of it
is what a first reading assumes. It is anchored to the end of the forward
frame, not its start, and it runs to the end of the reply, not its beginning.
A backward frame is nine bit periods — about 7.5 ms — so the settling time the
standard talks about is `since_tx_us` minus 7500.

IEC 62386-101 gives that settling time as 5.5 to 10.5 ms, nominal 7 ms, so
healthy gear lands at `since_tx_us` of roughly 13000-18000. The scheduler
attributes anything from `DALI_REPLY_WINDOW_OPEN_US` (5.5 ms) through
`DALI_REPLY_WINDOW_CLOSE_US` (27 ms) after release; a reply outside that is
counted in `rx_reply_early` or `rx_reply_late` — the two halves of the old
`rx_ignored_outside_reply`, which is now their sum together with five other
classes — and reported by `discover` as N early / N late replies outside the
window. A timestamp is what separates the two, so a reply arriving through the
frame-only RX entry point is counted `rx_ignored_unclassified` rather than
guessed at.

Empty addresses produce no observations at all — a capture across a stretch of
unpopulated addresses shows TX with no RX — so a non-zero count on an otherwise
quiet bus is not noise. It is gear whose replies are landing outside the window,
one count per attempt including retries, and the verb that asked reports
`timeout` while a correctly decoded reply sits in the buffer.

Gear replying *early* is the case to watch for, because the arithmetic hides it.
The open edge is tested against the observation's **first** edge, so subtract the
7500 before comparing against any window figure. Two edges apply, and which one
depends on whether the observation decoded:

| Observation | Open edge | Rejected below `since_tx_us` |
|---|---:|---:|
| Decoded 8-bit backward frame | `DALI_REPLY_WINDOW_OPEN_DECODED_US` (2 ms, the RX self-echo suppression) | ~9500 |
| Undecodable activity | `DALI_REPLY_WINDOW_OPEN_US` (5.5 ms, the standard's minimum) | ~13000 |

The split is deliberate. A decoded backward frame arriving while a query is
outstanding is unambiguously the reply, so only the PHY's own suppression floor
applies. Undecodable activity keeps the standard's edge, because that is the
path COMPARE reads as YES and where a wrong call invents gear.

A device that answers some runs and not others, with `since_tx_us` clustered
either side of one of those thresholds, is sitting on the window open rather
than failing intermittently, and retries will not rescue it — the gear is not
random, it is on a threshold.

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
query a5 device-type       # DT6 gear answers 6
dt6 5 dimming-curve        # 0 standard, 1 linear
query a5 min-level
query a5 max-level
```

If the curve is not what the entity assumes, either correct the gear
(`dt6 5 select-curve 0`) or pin the entity with `dimming_curve:`. Both drop the
cached profile; the first also re-reads it.

Check an LED driver for faults:

```text
status a5                  # Part 102 status flags
dt6 5 failure-status       # DT6 fault bitset
dt6 5 thermal-overload
dt6 5 open-circuit
```

Check group membership:

```text
query a0 groups-0-7
query a0 groups-8-15
```

Move a fixture from one room's group to another's — add before removing, so it
is never briefly in neither:

```text
query a5 groups-0-7        # before
config a5 add-group 3
config a5 remove-group 1
config a5 save-persistent
query a5 groups-0-7        # after
max g3                     # it should light with the rest of group 3
```

Move a whole group's worth of gear in two commands:

```text
config g1 add-group 3      # every current member of group 1 also joins 3
config g1 remove-group 1   # ...and then leaves 1
discover                   # the controller cannot compute this one
```

Give a fixture a different short address, or none at all. No initialise window
is involved; `SET SHORT ADDRESS` is an ordinary addressed config command:

```text
scan                                       # confirm the destination is free
config-dtr0 a5 set-short-address-dtr0 27   # a5 -> a13   ((13<<1)|1 = 27)
config a13 save-persistent
scan
identify 13
```

```text
config-dtr0 a5 set-short-address-dtr0 255  # a5 -> unaddressed; commission
                                           # unaddressed can reassign it
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


