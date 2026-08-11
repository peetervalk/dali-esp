# DALI Capability Matrix

**Last reviewed:** 2026-08-11

This file answers one question per row: for a given DALI capability, what
exists in the reusable C stack, whether the native CLI exposes it, whether an
independent host vector covers it, whether it has been exercised on a real bus,
and whether ESPHome exposes it.

It is a status record, not a conformance claim. "Host vector" means a test in
`test/` asserts frame layout or behaviour against values written from the
standard rather than read back from the implementation; where a suite mostly
repeats implementation constants the cell says so. "Real bus" means the path has
been run against physical gear and the result recorded, not that it is certified.

## Legend

| Mark | Meaning |
|---|---|
| yes | implemented and covered |
| partial | implemented, but with a stated limitation |
| no | not implemented |
| n/a | not applicable at this layer |

Real-bus results predate the 2026-08-10 static audit unless stated. Nothing in
the 2026-08-11 work has been flashed to hardware.

## Control gear — IEC 62386-102

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| DAPC level | `dali_control_build_dapc` | `level` | yes | yes | yes (light) |
| Arc power MASK | `dali_control_build_dapc_mask` | `level <t> mask`, `mask` | yes | no | no |
| OFF | `dali_control_build_off` | `off` | yes | yes | yes |
| UP / DOWN | `dali_control_build_up/down` | `up`, `down` | yes | yes | no |
| STEP UP / STEP DOWN | `dali_control_build_step_up/step_down` | `step-up`, `step-down` | yes | yes | no |
| STEP DOWN AND OFF | `dali_control_build_step_down_and_off` | `step-off` | yes | partial | no |
| ON AND STEP UP | `dali_control_build_on_and_step_up` | `on-step` | yes | partial | no |
| CONTINUOUS UP / DOWN | `dali_control_build_continuous_up/down` | `cont-up`, `cont-down` | yes | no | no |
| ENABLE DAPC SEQUENCE | `dali_control_build_enable_dapc_sequence` | `dapc-seq` | yes | no | no |
| GO TO LAST ACTIVE LEVEL | `dali_control_build_go_to_last_active_level` | `last` | yes | no | no |
| RECALL MAX / MIN | `dali_control_build_recall_max/min` | `max`, `min` | yes | yes | yes (button) |
| GO TO SCENE | `dali_control_build_go_to_scene` | `scene` | yes | no | no |
| Addressed queries (34 names) | `dali_control_build_query` | `query`, `status` | yes | yes | yes (console) |
| Configuration commands (19 names) | `dali_control_build_config` | `config` | yes | partial | yes (console) |
| DTR0-consuming configuration | `dali_control_build_config` + sequence | `config-dtr0` | yes | no | no |
| DTR0/1/2 load | `dali_control_build_dtr` | `dtr` | yes | yes | yes (console) |
| Special/broadcast commands (18 names) | `dali_build_special` | `special` | yes | partial | no |
| Arbitrary frame | n/a | `raw` | yes (parse only) | yes | yes (`raw`) |
| Arbitrary frame, send-twice | scheduler `send_twice` | `raw2` | yes (parse only) | no | yes (`raw2`) |

Real-bus coverage for queries and DAPC comes from the two site deployments and
recorded diagnostic sessions; the remaining "partial" cells mean some names in
the group have been exercised and others have not.

## Discovery, commissioning, and inventory

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| Short-address scan | `dali_discovery_scan` | `scan`, `discover` | yes | yes | yes |
| Device-type enumeration | `dali_discovery_build_device_types_sequence` | via `discover` | yes | no | yes |
| Group membership query | `dali_discovery_build_groups_sequence` | via `discover` | yes | yes | yes |
| Bank 0 identity read | `dali_memory_read_bank0_identity` | `meminfo` | yes | partial | via scan |
| Inventory export | `dali_discovery_inventory_*` | `inventory`, `export inventory` | partial | yes | yes (YAML lines) |
| Commission unaddressed | `dali_commissioning_commission_unaddressed` | `commission unaddressed` | yes | partial | no |
| Identify blink | n/a | `identify` | no | yes | yes |
| Smoke check | n/a | `smoke` | no | yes | no |

Commissioning is dependable only with a single unaddressed device on the bus;
the COMPARE collision inversion recorded in `current_status.md` is unfixed.

## Memory banks

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| Control-gear block read | `dali_memory_read_bytes` | `memread` | yes | partial | no |
| Control-gear Bank 0 identity | `dali_memory_read_bank0_identity` | `meminfo` | yes | partial | via scan |
| Control-device block read | `dali_memory_build_control_device_read_sequence` | `devmem read` | yes | no | yes (`memread`) |
| Control-device byte write | `dali_memory_build_control_device_write_sequence` | `devmem write` | yes | no | yes (`memwrite`) |
| Write read-back verification | no | no | no | no | no |

The ESPHome console's `memread`/`memwrite` are the *control-device* forms. The
native CLI names them `devmem read`/`devmem write` and reserves `memread` for
the Part 102 control-gear form, because the two use different DTR and memory
opcodes and picking the wrong one addresses a different device class.

No write path reads its value back. Treat every memory write as unverified until
a `devmem read` confirms it.

## Device type 6 — IEC 62386-207

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| ENABLE + command grouping | `dali_dt6_build_command_sequence` | all `dt6` verbs | yes | no | no |
| Configuration commands (5) | `dali_dt6_*` builders | `dt6 <addr> <name>` | yes | no | no |
| Queries (19) | `dali_dt6_query_*` | `dt6 <addr> <name>` | yes | no | no |
| Failure-status decode | `dali_dt6_parse_failure_status` | via `dt6 failure-status` | yes | no | no |

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

Mapping DT8 to Home Assistant traits is deliberately held until the native verbs
have been run against real DT8 gear.

## Control devices — IEC 62386-103 and Parts 301/303/304

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| Instance discovery | `dali_discovery_query_input_device` | `instances` | yes | yes | yes |
| Generic instance queries (15) | `dali_input_build_query_*` | `iquery` | yes | partial | yes (`iquery`) |
| DTR0-selected instance query | `dali_input_build_config_sequence` | `iquery ... instance-config` | yes | no | no |
| Generic configuration (10) | `dali_input_build_set_*` | `iconfig` | yes | no | yes (`iconfig`) |
| Part 301 push-button timers | `dali_input_pb_*` | `iquery pb-*`, `iconfig pb-set-*` | yes | no | yes (`pb-*`) |
| Part 303 occupancy | `dali_input_occ_*` | `iquery occ-*`, `iconfig occ-*` | yes | partial | yes |
| Part 304 light sensor | `dali_input_light_*` | `iquery light-*`, `iconfig light-*` | yes | partial | yes (`light-*`) |
| Multi-byte input polling | `dali_input_poll_build_value_sequence` | `sensor poll` | yes | yes | yes |
| Event decode | `dali_event_*` | `events`, `capture`, `find switches` | yes | yes | yes |
| Event dispatch rules | `dali_dispatch_*` | n/a | yes | yes | yes |

Configuration writes are experimental everywhere. The native CLI says so on
every `iconfig` success line: the result means transmitted, not applied. Read
the value back with `iquery` before relying on it.

## Vendor helpers

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| Lunatone sensor queries (7) | `dali_lunatone_build_instance_command` | `vendor lunatone` | yes | no | no |
| Steinel HF 360 II instance profile | `dali_steinel_hf360_instance_lookup` | `vendor steinel` | yes | yes | yes (YAML) |
| Steinel value conversions | `dali_steinel_temperature_*`, `_humidity_*` | `vendor steinel` | yes | yes | yes |

## CLI infrastructure

| Capability | Shared API | Native CLI verb | Host vector | Real bus | ESPHome |
|---|---|---|---|---|---|
| Tokenising and trailing-token rejection | `dali_cli_tokenize`, `dali_cli_resolve[_in]` | every verb | yes | no | yes |
| Verb table / help parity | `dali_cli_command_*`, `dali_cli_print_help` | `help` | yes | n/a | n/a |
| Argument validation | `dali_cli_parse_*` | every verb | yes | no | yes |
| Named table listing | `dali_cli_print_table` | `list`, `*-list` | yes | n/a | n/a |
| Response formatting | `dali_cli_print_response` | every query verb | yes | yes | separate path (one HA text state) |
| Scheduler queue diagnostics | `dali_sched_queue_stats` | `stats`, `queue` | yes | no | yes (`queue`) |
| PHY/RX counters | `g_dali_stats` | `stats`, `bus check`, `rxdebug` | partial | yes | partial |
| Frame capture | n/a | `capture` | no | yes | yes (bus monitor) |

The ESPHome console now uses `dali_cli` for tokenising, argument parsing, arity
checking, and the named command tables, so a verb and a command name mean the
same thing on both surfaces and trailing tokens are rejected. It supplies its
own verb table to `dali_cli_resolve_in()`: the subset that suits a Home
Assistant text entity, with no scan, capture, or inventory verbs. The renames
this brought are listed in `dali_command_reference.md`. None of the migrated
console paths has been exercised on a real bus.

## Known gaps this matrix is tracking

- No verb has real-bus verification for DT6, DT8, memory writes, or input-device
  configuration. Those rows are the reason the CLI exists; running them is the
  next step, not more code.
- `identify`, `smoke`, `capture`, and the inventory JSON export have no host
  vectors. They are composed from covered primitives, but their own output
  formats are unasserted.
- The ESPHome console and the native CLI have separate parsers and separate verb
  names for control-device memory. Converging them would remove a class of
  divergence but is not scheduled.
- Nothing here claims DALI Alliance certification or complete IEC 62386
  coverage.
