# DALI Capability Matrix

**Last reviewed:** 2026-08-25

One question per row: for a given DALI capability, what exists in the reusable C
stack, whether the CLI exposes it, whether an independent host vector covers it,
whether it has been exercised on a real bus, and whether ESPHome exposes it.

This is a status record, not a conformance claim. What each verb does and how to
call it is in `dali_commands.md`; frames and opcodes are in `dali_protocol.md`.

**Host vector** means a test in `test/` asserts frame layout or behaviour against
values written from the standard rather than read back from the implementation;
where a suite mostly repeats implementation constants, the cell says so.
**Real bus** means the path has been run against physical gear and the result
recorded — not that it is certified.

| Mark | Meaning |
|---|---|
| yes | implemented and covered |
| partial | implemented, with a stated limitation |
| no | not implemented |
| n/a | not applicable at this layer |

Real-bus results predate the 2026-08-10 static audit unless stated, with one
exception: `v1.1.1` was flashed to both sites and run on the installed buses on
2026-08-14, which covers the shell rows below. The 2026-08-11 and 2026-08-12
verb-parity work has still not been exercised verb by verb on a bus.

The ESPHome column names the surface: `console` is the `text:` command entity,
`light`/`button`/`sensor` are entities, `scan` is the discovery workflow, and
`shell` is the TCP diagnostic shell. Neither "console" nor "shell" implies
real-bus verification.

`shell` differs in kind from the others. The rest of the column describes code
written for that surface; `shell` means the surface runs the native CLI's own
implementation, so parity there is structural rather than something to re-check
verb by verb.

## Control gear — IEC 62386-102

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| DAPC level | `dali_control_build_dapc` | `level` | yes | yes | yes (light, console) |
| Arc power MASK | `dali_control_build_dapc_mask` | `level <t> mask`, `mask` | yes | no | yes (console) |
| OFF | `dali_control_build_off` | `off` | yes | yes | yes |
| UP / DOWN | `dali_control_build_up/down` | `up`, `down` | yes | yes | yes (console) |
| STEP UP / STEP DOWN | `dali_control_build_step_up/step_down` | `step-up`, `step-down` | yes | yes | yes (console) |
| STEP DOWN AND OFF | `dali_control_build_step_down_and_off` | `step-off` | yes | partial | yes (console) |
| ON AND STEP UP | `dali_control_build_on_and_step_up` | `on-step` | yes | partial | yes (console) |
| CONTINUOUS UP / DOWN | `dali_control_build_continuous_up/down` | `cont-up`, `cont-down` | yes | no | yes (console) |
| ENABLE DAPC SEQUENCE | `dali_control_build_enable_dapc_sequence` | `dapc-seq` | yes | no | yes (console) |
| GO TO LAST ACTIVE LEVEL | `dali_control_build_go_to_last_active_level` | `last` | yes | no | yes (console) |
| RECALL MAX / MIN | `dali_control_build_recall_max/min` | `max`, `min` | yes | yes | yes (button, console) |
| GO TO SCENE | `dali_control_build_go_to_scene` | `scene` | yes | no | yes (console) |
| Addressed queries (34 names) | `dali_control_build_query` | `query`, `status` | yes | yes | yes (console) |
| Configuration commands (19 names) | `dali_control_build_config` | `config` | yes | partial | yes (console) |
| DTR0-consuming configuration | `dali_control_build_config` + sequence | `config-dtr0` | yes | no | yes (console, minus `set-short-address-dtr0`) |
| DTR0/1/2 load | `dali_control_build_dtr` | `dtr` | yes | yes | yes (console) |
| Special/broadcast commands (18 names) | `dali_build_special` | `special` | yes | partial | partial (console) |
| Arbitrary frame | n/a | `raw` | yes (parse only) | yes | yes (`raw`) |
| Arbitrary frame, send-twice | scheduler `send_twice` | `raw2` | yes (parse only) | no | yes (`raw2`) |

Real-bus coverage for queries and DAPC comes from the two site deployments and
recorded diagnostic sessions; a "partial" cell means some names in the group have
been exercised and others have not.

The console's `special` is partial by design: it refuses the nine commissioning
primitives. A level-changing console verb transmits and returns without updating
the light entity; the exception is `dt6 select-curve`, which invalidates the
cached level profile and triggers a refresh.

## Discovery, commissioning, and inventory

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| Short-address scan | `dali_discovery_scan` | `scan`, `discover` | yes | yes | yes (scan, shell) |
| Device-type enumeration | `dali_discovery_build_device_types_sequence` | via `discover` | yes | no | yes (scan, shell) |
| Group membership query | `dali_discovery_build_groups_sequence` | via `discover` | yes | yes | yes (scan, shell) |
| Bank 0 identity read | `dali_memory_read_bank0_identity` | `meminfo` | yes | partial | via scan; `meminfo` via shell |
| Inventory export | `dali_discovery_inventory_*` | `inventory`, `export inventory` | partial | yes | yes (YAML lines, shell) |
| Configuration export | n/a — reads live entity state | `export config` | no | yes | shell only; the native build has no YAML to describe |
| Commission unaddressed | `dali_commissioning_commission_unaddressed` | `commission unaddressed` | yes | partial | shell, opt-in |
| Identify blink | n/a | `identify` | no | yes | yes (button, shell) |
| Smoke check | n/a | `smoke` | no | yes | shell |

The host suite now carries timestamped RX observations from the PHY into the
scheduler. Frame-like undecodable activity in an active reply window becomes
`DALI_ERR_RX_ACTIVITY`; COMPARE alone interprets it as YES, while ordinary
queries retain the ambiguity as an error. The short-address scan is the one
caller that does neither: it records the address as `has_undecodable_activity`,
counts it, keeps walking, and holds the address out of the commissioning free
pool without listing it as a device. The same host coverage asserts that a
cancelled commissioning workflow uses a safety transport to attempt TERMINATE
despite the latched front-end abort, and reports a failed cleanup separately.

That is host evidence, not a real-bus multi-device result. Commissioning remains
limited operationally to one unaddressed control gear at a time until the
multi-device path is exercised on hardware. A run now brackets itself with
broadcast START/STOP QUIESCENT MODE — host-tested for ordering, settle,
release-on-every-exit, and the two failure modes, but never run on a bus, and it
cannot reach a device that does not receive the broadcast. Equal-random-address
recovery and arbitration against another bus master remain open.

The shell rows are the same code the native CLI runs, reached through
`esphome/components/dali/dali_shell_tcp.cpp`. Its commissioning entry point is
`commission unaddressed [first] [max]`; that verb and the nine commissioning
primitives are refused over TCP unless the YAML sets `allow_commissioning: true`,
because the port is unauthenticated.

The shell was flashed and run against a real bus on 2026-08-14 in `v1.1.1`:
discover, identify, live trace, rolling capture, and JSON export were exercised
over the TCP front end. The "Real bus" column above is still recorded from the
native CLI, which is the same implementation reached by a different transport.

## Memory banks

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| Control-gear block read | `dali_memory_build_read_sequence` | `memread` | yes | partial | yes (console) |
| Control-gear Bank 0 identity | `dali_memory_read_bank0_identity` | `meminfo` | yes | partial | via scan |
| Control-device block read | `dali_memory_build_control_device_read_sequence` | `devmem read` | yes | no | yes (console) |
| Control-device byte write | `dali_memory_build_control_device_write_sequence` | `devmem write` | yes | no | yes (console) |
| Write read-back verification | no | no | no | no | no |

No write path reads its value back. Treat every memory write as unverified until
a `devmem read` confirms it.

## Device type 6 — IEC 62386-207

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| ENABLE + command grouping | `dali_dt6_build_command_sequence` | all `dt6` verbs | yes | no | yes (console) |
| Configuration commands (5) | `dali_dt6_*` builders | `dt6 <addr> <name>` | yes | no | yes (console) |
| Queries (19) | `dali_dt6_query_*` | `dt6 <addr> <name>` | yes | no | yes (console) |
| Failure-status decode | `dali_dt6_parse_failure_status` | via `dt6 failure-status` | yes | no | no |
| Discovered dimming curve | `dali_discovery_*`, `dali_dim_curve` | via `discover` | yes | yes | yes (light) |
| Per-entity curve override | `dali_level_profile_validate` | n/a | yes | no | yes (`dimming_curve:`) |

All 24 DT6 names are reachable from the console, and every one goes out through
`dali_dt6_build_command_sequence()`, so ENABLE DEVICE TYPE 6, any DTR0 load, and
the command cannot be separated by other locally scheduled traffic.

## Device type 8 — IEC 62386-209

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| ENABLE + command grouping | `dali_dt8_build_command_sequence` | all `dt8` verbs | yes | no | no |
| Temporary XY / Tc / RGBWAF set | `dali_dt8_set_temporary_*` | `dt8 <addr> set-*` | yes | no | no |
| Activate / copy report | `dali_dt8_activate`, `dali_dt8_copy_report_to_temporary` | `dt8 <addr> activate` | yes | no | no |
| Store commands (6) | `dali_dt8_store_*` | `dt8 <addr> store-*` | yes | no | no |
| Queries (6) | `dali_dt8_query_*` | `dt8 <addr> <name>` | yes | no | no |
| 16-bit colour value read | `dali_dt8_build_colour_value_sequence` | `dt8 <addr> colour <sel>` | yes | no | no |
| Mirek/Kelvin conversion | `dali_dt8_kelvin_to_mirek`, `dali_dt8_mirek_to_kelvin` | via `dt8 colour tc` | yes | n/a | no |
| HA colour-temperature / XY / RGB traits | no | n/a | no | no | no |

DT8 is held back from the ESPHome surface entirely — verbs as well as traits —
until the native verbs have been run against real DT8 gear. There is none on
either site, so exposing it would be publishing an untested surface, not closing
a gap.

## Control devices — IEC 62386-103 and Parts 301/303/304

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| Instance discovery | `dali_discovery_query_input_device` | `instances` | yes | yes | yes |
| Generic instance queries | `dali_input_build_query_*` | `iquery` | yes | partial | yes (`iquery`) |
| DTR0-selected instance query | `dali_input_build_config_sequence` | `iquery ... instance-config` | yes | no | yes (`iquery <a> <i> <n> <dtr0>`) |
| Generic configuration (10) | `dali_input_build_set_*` | `iconfig` | yes | no | yes (`iconfig`) |
| Part 301 push-button timers | `dali_input_pb_*` | `iquery pb-*`, `iconfig pb-set-*` | yes | no | yes (`pb-*`) |
| Part 303 occupancy | `dali_input_occ_*` | `iquery occ-*`, `iconfig occ-*` | yes | partial | yes |
| Part 304 light sensor | `dali_input_light_*` | `iquery light-*`, `iconfig light-*` | yes | partial | yes (`light-*`) |
| Multi-byte input polling | `dali_input_poll_build_value_sequence` | `sensor poll` | yes | yes | yes |
| Event decode | `dali_event_*` | `events`, `capture`, `find switches` | yes | yes | yes |
| Event dispatch rules | `dali_dispatch_*` | n/a | yes | yes | yes |
| Quiescent mode | `dali_input_build_quiescent_mode[_broadcast]` | `quiescent on\|off <addr\|all>` | yes | no | yes (console) |
| Commissioning quiescence bracket | `DaliCommissioningOptions.quiesce_control_devices` | automatic in `commission` | yes | no | n/a |
| Device broadcast (0xFF) | `dali_build_device_broadcast_command` | via `quiescent ... all` | yes | no | yes (console) |

Configuration writes are experimental everywhere. The native CLI says so on every
`iconfig` success line: the result means transmitted, not applied.

## Vendor helpers

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| Lunatone sensor queries (7) | `dali_lunatone_build_instance_command` | `vendor lunatone` | yes | no | yes (console) |
| Steinel HF 360 II instance profile | `dali_steinel_hf360_instance_lookup` | `vendor steinel` | yes | yes | yes (YAML, console) |
| Steinel value conversions | `dali_steinel_temperature_*`, `_humidity_*` | `vendor steinel` | yes | yes | yes |

## CLI infrastructure

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| Tokenizing and trailing-token rejection | `dali_cli_tokenize`, `dali_cli_resolve[_in]` | every verb | yes | no | yes |
| Verb table / help parity | `dali_cli_command_*`, `dali_cli_print_help` | `help` | yes | n/a | n/a |
| Argument validation | `dali_cli_parse_*` | every verb | yes | no | yes |
| Named table listing | `dali_cli_print_table` | `list`, `*-list` | yes | n/a | n/a |
| Command tables as JSON | `dali_cli_print_schema` | `schema` | yes | n/a | n/a |
| Response formatting | `dali_cli_print_response` | every query verb | yes | yes | yes (`dali_cli_format_response`) |
| Single-line reply decode | `dali_cli_format_response`, `_format_status` | via `dali_cli_print_response` | yes | no | yes (every query verb) |
| Scheduler queue diagnostics | `dali_sched_queue_stats` | `stats`, `queue` | yes | no | yes (`queue`) |
| PHY/RX counters | `g_dali_stats` | `stats`, `bus check`, `rxdebug` | partial | yes | partial |
| Frame capture | n/a | `capture` | no | yes | yes (bus monitor) |

The console supplies its own verb table to `dali_cli_resolve_in()` — the subset
that suits a Home Assistant text entity — but shares the tokenizer, argument
parsers, arity checking, named tables, and reply decoding. None of the migrated
console paths has been exercised on a real bus.

## Verb parity between the two surfaces

The console implements every native verb whose answer fits one Home Assistant
text state and whose execution fits one enqueue and one completion. What remains
native-only, and why:

| Native verb | Why not on the console |
|---|---|
| `help`, `list`, `schema`, `query-list`, `special-list`, `config-list` | The answer is a block of lines |
| `stats`, `bus check`, `rxdebug`, `read`, `trace`, `reset` | Same, and the counters are already on diagnostic sensors |
| `capture` | Rolling buffer with a terminal-shaped export; the bus monitor covers the live view |
| `scan`, `discover`, `inventory`, `export inventory`, `identify` | Exposed as buttons and text sensors instead |
| `export config` | A whole config block; the scan's `yaml_result` sensor carries the group map the console can fit |
| `commission unaddressed` | No guarded workflow here; `special` refuses its primitives for the same reason |
| `config <t> set-short-address-dtr0` | Re-addresses gear from one typed line, the same reason `special program-short` is refused |
| `meminfo`, `instances`, `sensor poll` | Each walks a device and decides the next query from the last reply, which needs a blocking transport. Covered by the scan and the sensor platform |
| `smoke` | Composed of `devmem` write/read; run the parts |
| `dt8` | Held until real DT8 gear is available to test against |

`group forget` runs the other way: it is console-only, because the cache it edits
belongs to the ESPHome component rather than to the protocol stack.

The caches that cache is part of are now fed by both surfaces. A shell session's
`discover` and `commission` publish their inventory through
`DaliShellHooks::inventory_changed`, and a `config` verb reports what it sent
through `DaliShellHooks::config_applied`, so the group-membership table and the
level-profile cache no longer depend on which surface an operator happened to
use. Host- and compile-verified only; no bus has run it.

## Known gaps this matrix is tracking

- Most verbs do not have real-bus verification for DT6, DT8, memory writes, or input-device
  configuration. Those rows are the reason the CLI exists.
- `identify`, `smoke`, `capture`, and the inventory JSON export have no host
  vectors. They are composed from covered primitives, but their own output
  formats are unasserted.
- `export config` has none either, and is the one most worth having: its output
  is only useful if it validates, which a host vector could assert against the
  ESPHome schema rather than leaving to a paste-and-see.
- The console's handlers themselves are unasserted. Their parsing, argument
  validation, and reply decoding are shared code with host vectors, but the
  dispatch in `dali_component.cpp` between them is ESPHome/FreeRTOS-bound and
  reachable only on the device.
- Quiescent mode has frame-level host vectors but no bus result. Commissioning
  releases what it started; the standalone verb does not, so a device left
  quiescent by hand stays silent until `quiescent off`, which is
  indistinguishable from a dead sensor.
- Nothing here claims DALI Alliance certification or complete IEC 62386 coverage.
