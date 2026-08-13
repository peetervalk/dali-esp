# dali-esp

DALI-2 lighting controller for ESP32, built for [ESPHome](https://esphome.io) and Home Assistant.

A reusable C protocol stack (`components/dali`) drives the bus — PHY, scheduler, transport, discovery, commissioning, DT6/DT8 control gear, and DALI-2 input devices — with an ESPHome external component (`esphome/components/dali`) on top. A native ESP-IDF diagnostic CLI (`main/`) provides serial workflows for common control, discovery, commissioning, capture, and bus debugging; its verb tables, argument parsers, and reply formatting are shared with the Home Assistant command console, so a command means the same thing on both surfaces.

## Features

- Lights by group, short address, or broadcast, with query-based state readback; short-address entities query themselves by default and groups use a representative member (`QUERY ACTUAL LEVEL`)
- Brightness maps through the DALI arc-power curve rather than a linear percentage. Each target's curve and MIN/MAX LEVEL window are queried from the gear and cached; a group entity uses the union of its members' windows. `dimming_curve`, `min_level`, and `max_level` override the queried values per light.
- Multi-frame operations (DTR loads, ENABLE DEVICE TYPE, memory reads, send-twice config) run as atomic scheduler transactions, so locally scheduled traffic cannot interleave between the steps
- Bus scan and discovery from Home Assistant; scan-verified group membership is persisted to flash and survives reboots; control-gear commissioning through the native CLI
- DALI-2 input devices: occupancy, lux, temperature, humidity — authoritative polling, with matching Device/Instance events requesting an immediate poll (`poll_on_event`)
- Passive observation of existing pushbutton couplers (`headless_dispatch`): couplers keep commanding the lamps, ESP32 keeps HA state in sync
- Diagnostic shell over TCP (`shell:`): the full native CLI — discover, identify, commissioning, live trace, rolling capture, JSON export — from a terminal, with no serial cable. The same shell, running the same code, as the serial console on a native ESP-IDF build. Newer than `v1.0.4`; see the note under the example configuration
- Free-text DALI command console in HA: queries, config, memory bank read/write, raw 16/24-bit frames
- Protocol core is plain C with no ESPHome dependency; protocol and cross-task
  helpers are covered by 26 host test suites, and CI additionally builds the
  ESPHome component, the native ESP-IDF firmware, and a release tag as a
  consumer would fetch it

## Hardware

| | |
|---|---|
| MCU | ESP32 (tested: ESP32-WROVER-E / ESP32-DevKitC-VE) |
| DALI interface | MikroE [DALI 2 Click](https://www.mikroe.com/dali-2-click) |
| Wiring | TX → GPIO18, RX → GPIO19 |

Notes: GPIO16/17 are unavailable on WROVER-E (used by PSRAM). The controller has no proven collision-detection/arbitration strategy. Existing DALI-1 pushbutton couplers in direct-control mode coexist on the installed buses, but simultaneous transmissions remain a risk; see `headless_dispatch`.

## Getting started

1. Flash [dali_diag.yaml](dali_diag.yaml) (`esphome run dali_diag.yaml`).
2. Commission the bus, either way round:
   - **From a terminal.** Run [tools/dali-shell](tools/dali-shell) from any host
     that can reach the device — standard library only, nothing to install, and
     it drops straight into the HA "Advanced SSH & Web Terminal" add-on.
     `discover` maps short addresses, device types and groups; `identify <addr>`
     blinks one fixture; `export inventory > /config/dali_inventory.json` writes
     the result as JSON. `help` lists every verb. With no client to hand,
     `nc dali-diag.local 2323` is a complete if unfriendly substitute.
   - **From Home Assistant.** **Scan DALI Bus**, **Find Couplers**, and
     **Identify** do the same walk from a phone. A scan publishes ready-to-paste
     light YAML only when group discovery is complete; otherwise it retains the
     previous map and asks you to retry.

   The two are separate implementations of the same walk, so running both is
   itself a diagnostic — if they disagree, the disagreement is the finding.
3. Write your own config from the example below and the scan output.

### Example configuration

A bus with two lamp groups, one individually addressed lamp, and a DALI-2 multi-sensor (e.g. Steinel HF 360 II) at short address 0:

The example pins `v1.0.4`, the last tagged release. `dev` carries newer work;
compile and test it separately before pointing an installation at it.

The diagnostic shell is part of that newer work and is **not in `v1.0.4`**. To
use it, point `ref:` at a branch rather than that tag, then add:

```yaml
dali:
  # ...
  shell:
    port: 2323
    idle_timeout: 10min
    # The port is unauthenticated — the same posture as OTA and the web server,
    # but a lower bar than physical access to a UART — so the commissioning
    # verbs are refused unless this opts in. Leave it false on anything left
    # flashed on a shared network: one typed line can readdress a whole bus,
    # and RANDOMISE cannot be undone.
    allow_commissioning: false
```

Omit the block entirely and the shell is not compiled in at all.

```yaml
esp32:
  variant: esp32   # classic ESP32, e.g. WROVER-E
  framework:
    type: esp-idf

external_components:
  - source:
      type: git
      url: https://github.com/peetervalk/dali-esp.git
      ref: v1.0.4
    components: [dali]

dali:
  id: dali_bus
  tx_pin: 18
  rx_pin: 19
  command_result:        # replies from the command console
    name: "DALI Command Result"
  bus_fault:             # bus-stuck / wiring fault indicator
    name: "Bus Fault"
  headless_dispatch:
    # Existing wall-switch coupler commands group 0 directly; observe it
    # so HA state stays in sync without retransmitting.
    - { frame_kind: legacy_16bit, address_kind: group, address: 0, action: observe, output_type: group, output_address: 0 }

light:
  - platform: dali
    dali_id: dali_bus
    name: "Living Room"
    target_type: group        # short | group | broadcast
    target_address: 0
    query_address: 2          # cold-start seed: any group member; only a
                              # complete scan replaces verified membership
  - platform: dali
    dali_id: dali_bus
    name: "Hallway"
    target_type: group
    target_address: 1
    query_address: 5

  - platform: dali
    dali_id: dali_bus
    name: "Desk Lamp"
    target_type: short
    target_address: 8
    member_groups: [1]        # short-address entity also follows group 1
                              # dispatch results
    # Level window and curve are queried from the gear. Override only when the
    # gear reports something the fixture cannot honour:
    # min_level: 10           # 1..254 arc power
    # max_level: 254
    # dimming_curve: auto     # auto (query the gear) | standard | linear

sensor:
  - platform: dali
    dali_id: dali_bus
    name: "Hallway Lux"
    address: 0                # input device short address
    instance: 0               # instance number on that device
    value_bytes: 2
    scale: 0.01
    poll_interval: 30
    poll_on_event: false      # a lux instance reports continuously; polling on
                              # every event pulls the whole bus down to its rate
    unit_of_measurement: "lx"
    device_class: illuminance

  - platform: dali
    dali_id: dali_bus
    name: "Hallway Occupancy"
    address: 0
    instance: 1
    poll_interval: 5          # Part 303/type 3 raw state: 0 / 85 / 170 / 255
                              # poll_on_event defaults to true — occupancy
                              # should follow a change without waiting

# Optional: free-text DALI console for diagnostics from HA
text:
  - platform: dali
    dali_id: dali_bus
    name: "DALI Command"
    mode: text
```

## Documentation

- [tools/dali-shell](tools/dali-shell) — terminal client for the diagnostic shell; `help` on a connected device is the authoritative verb list
- [esphome_verb_readme.md](esphome_verb_readme.md) — command console verbs and syntax
- [dali_command_reference.md](dali_command_reference.md) — DALI command/protocol catalog
- [dali_capability_matrix.md](dali_capability_matrix.md) — per-capability status: shared API, native CLI verb, host vector, real-bus result, ESPHome surface
- [current_status.md](current_status.md) — project state, known limitations, roadmap
- [steinel_bank2_reference.md](steinel_bank2_reference.md) — Steinel HF 360 II memory bank tuning

## Scope and limitations

- Control gear: DT6 (LED) and DT8 (colour) are implemented. Other device types (DT0 fluorescent, DT1 emergency, …) are not.
- One DALI bus per controller. Direct-control couplers are additional transmitters;
  simultaneous traffic is not collision-safe.
- Implemented does not mean verified on hardware. Several paths — the DT6/DT8
  command sets, memory writes, input-device configuration — have host vectors but
  no recorded real-bus result; [dali_capability_matrix.md](dali_capability_matrix.md)
  states which is which per capability.
- This is an independent project. It implements and is tested against useful parts of IEC 62386, but it is **not** a DALI Alliance certified product. DALI and DALI-2 are trademarks of the Digital Illumination Interface Alliance.

## Development

Host tests (no hardware needed) — 26 suites over the C protocol stack and the
cross-task helpers the ESPHome layer depends on:

```sh
cmake -B test/build -S test
cmake --build test/build
ctest --test-dir test/build --output-on-failure
```

To prove a change to the ESPHome layer actually builds, compile
[dali_test.yaml](dali_test.yaml). It resolves `external_components` to
`type: local`, so it builds the working tree by construction, and it declares
every platform and every optional block on purpose — an option no tracked config
names is an option nothing compiles. Extend it in the same change that extends
the schema.

```sh
esphome config  dali_test.yaml    # schema only; does not run to_code()
esphome compile dali_test.yaml    # the working tree, every platform
```

Native diagnostic firmware: standard ESP-IDF flow (`idf.py build flash monitor`).

CI runs on pushes to `main` and `dev`, and on pull requests into `main`:

| Workflow | What it checks |
|---|---|
| `host-tests.yml` | The host suites build and pass |
| `esphome-build.yml` | Source/shim/CMakeLists manifests agree; every tracked `dali*.yaml` validates against the Python schema; those that build the in-repo component compile fully |
| `idf-build.yml` | The native firmware builds for esp32 against pinned ESP-IDF 6.0.1, from `sdkconfig.defaults` alone |
| `release-packaging.yml` | A consumer can build the component from a published tag with no checkout of this repo (runs on `v*` tags) |

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
