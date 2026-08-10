# dali-esp

DALI-2 lighting controller for ESP32, built for [ESPHome](https://esphome.io) and Home Assistant.

A reusable C protocol stack (`components/dali`) drives the bus — PHY, scheduler, discovery, commissioning, DT6/DT8 control gear, and DALI-2 input devices — with an ESPHome external component (`esphome/components/dali`) on top. A native ESP-IDF diagnostic CLI (`main/`) provides serial workflows for common control, discovery, commissioning, capture, and bus debugging.

## Features

- Lights by group, short address, or broadcast, with query-based state readback when a short-address query target is available (`QUERY ACTUAL LEVEL`)
- Bus scan and discovery from Home Assistant; control-gear commissioning through the native CLI
- DALI-2 input devices: occupancy, lux, temperature, humidity — authoritative polling, with matching Device/Instance events requesting an immediate poll
- Passive observation of existing pushbutton couplers (`headless_dispatch`): couplers keep commanding the lamps, ESP32 keeps HA state in sync
- Free-text DALI command console in HA: queries, config, memory bank read/write, raw 16/24-bit frames
- Protocol core is plain C with no ESPHome dependency, covered by 19 host test suites (run in CI)

## Hardware

| | |
|---|---|
| MCU | ESP32 (tested: ESP32-WROVER-E / ESP32-DevKitC-VE) |
| DALI interface | MikroE [DALI 2 Click](https://www.mikroe.com/dali-2-click) |
| Wiring | TX → GPIO18, RX → GPIO19 |

Notes: GPIO16/17 are unavailable on WROVER-E (used by PSRAM). The controller has no proven collision-detection/arbitration strategy. Existing DALI-1 pushbutton couplers in direct-control mode coexist on the installed buses, but simultaneous transmissions remain a risk; see `headless_dispatch`.

## Getting started

1. Flash [dali_diag.yaml](dali_diag.yaml) (`esphome run dali_diag.yaml`). Use **Scan DALI Bus**, **Find Couplers**, and **Identify** from HA to map out addresses and groups — the scan streams ready-to-paste light YAML to the log.
2. Write your own config from the example below and the scan output.

### Example configuration

A bus with two lamp groups, one individually addressed lamp, and a DALI-2 multi-sensor (e.g. Steinel HF 360 II) at short address 0:

The example intentionally pins the known-working `v1.0.1` deployment baseline.
Development-branch changes should be compiled and tested separately before use on
an installation.

```yaml
esp32:
  variant: esp32   # classic ESP32, e.g. WROVER-E
  framework:
    type: esp-idf

external_components:
  - source:
      type: git
      url: https://github.com/peetervalk/dali-esp.git
      ref: v1.0.1
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
    query_address: 2          # cold-start seed: any group member; a scan
                              # replaces it with verified membership
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

sensor:
  - platform: dali
    dali_id: dali_bus
    name: "Hallway Lux"
    address: 0                # input device short address
    instance: 0               # instance number on that device
    value_bytes: 2
    scale: 0.01
    poll_interval: 30
    unit_of_measurement: "lx"
    device_class: illuminance

  - platform: dali
    dali_id: dali_bus
    name: "Hallway Occupancy"
    address: 0
    instance: 1
    poll_interval: 5          # DT303 raw state: 0 / 85 / 170 / 255

# Optional: free-text DALI console for diagnostics from HA
text:
  - platform: dali
    dali_id: dali_bus
    name: "DALI Command"
    mode: text
```

## Documentation

- [esphome_verb_readme.md](esphome_verb_readme.md) — command console verbs and syntax
- [dali_command_reference.md](dali_command_reference.md) — DALI command/protocol catalog
- [current_status.md](current_status.md) — project state, known limitations, roadmap
- [steinel_bank2_reference.md](steinel_bank2_reference.md) — Steinel HF 360 II memory bank tuning

## Scope and limitations

- Control gear: DT6 (LED) and DT8 (colour) are implemented. Other device types (DT0 fluorescent, DT1 emergency, …) are not.
- One DALI bus per controller. Direct-control couplers are additional transmitters;
  simultaneous traffic is not collision-safe.
- This is an independent project. It implements and is tested against useful parts of IEC 62386, but it is **not** a DALI Alliance certified product. DALI and DALI-2 are trademarks of the Digital Illumination Interface Alliance.

## Development

Host tests (no hardware needed):

```sh
cmake -B test/build -S test
cmake --build test/build
ctest --test-dir test/build --output-on-failure
```

Native diagnostic firmware: standard ESP-IDF flow (`idf.py build flash monitor`).

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
