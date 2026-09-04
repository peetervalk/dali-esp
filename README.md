# dali-esp

DALI-2 lighting controller for ESP32, built for [ESPHome](https://esphome.io) and Home Assistant.

A reusable C protocol stack (`components/dali`) drives the bus — PHY, scheduler, transport, discovery, commissioning, DT6/DT8 control gear, and DALI-2 input devices — with an ESPHome external component (`esphome/components/dali`) on top. A native ESP-IDF diagnostic CLI (`main/`) provides serial workflows for common control, discovery, commissioning, capture, and bus debugging; its verb tables, argument parsers, and reply formatting are shared with the Home Assistant command console, so a command means the same thing on both surfaces.

## Features

- Lights by group, short address, or broadcast, with query-based state readback; short-address entities query themselves by default and groups use a representative member (`QUERY ACTUAL LEVEL`)
- Brightness maps through the DALI arc-power curve rather than a linear percentage. Each target's curve and MIN/MAX LEVEL window are queried from the gear and cached; a group entity uses the union of its members' windows. `dimming_curve`, `min_level`, and `max_level` override the queried values per light.
- Multi-frame operations (DTR loads, ENABLE DEVICE TYPE, memory reads, send-twice config) run as atomic scheduler transactions, so locally scheduled traffic cannot interleave between the steps
- Bus scan and discovery from Home Assistant; scan-verified group membership is persisted to flash and survives reboots; control-gear commissioning through the native serial CLI or the TCP diagnostic shell with `allow_commissioning: true`
- DALI-2 input devices: occupancy, lux, temperature, humidity — authoritative polling, with matching Device/Instance events requesting an immediate poll (`poll_on_event`)
- Passive observation of existing pushbutton couplers (`headless_dispatch`): couplers keep commanding the lamps, ESP32 keeps HA state in sync
- Diagnostic shell over TCP (`shell:`): the full native CLI — discover, identify, commissioning, live trace, rolling capture, JSON export — from a terminal, with no serial cable. The same shell, running the same code, as the serial console on a native ESP-IDF build
- Free-text DALI command console in HA: queries, config, memory bank read/write, raw 16/24-bit frames
- Protocol core is plain C with no ESPHome dependency; protocol and cross-task
  helpers are covered by 31 host test suites, and CI additionally builds the
  ESPHome component, the native ESP-IDF firmware, and a release tag as a
  consumer would fetch it

## Hardware

| | |
|---|---|
| MCU | ESP32 (classic), tested on ESP32-WROVER-E / ESP32-DevKitC-VE. Other variants are untested |
| Cores | Two recommended; a single-core part is handled but has never been built or run here |
| RAM | ~134 KiB of static DRAM at link time, 65 KiB of it the diagnostic shell. No PSRAM required |
| Flash | 4 MB. ESPHome's default ESP32 layout is two 1.75 MB app slots plus a 448 KiB NVS partition, and NVS is where a saved backup lives |
| DALI interface | MikroE [DALI 2 Click](https://www.mikroe.com/dali-2-click) |
| Wiring | TX → GPIO18, RX → GPIO19 |

Notes: GPIO16/17 are unavailable on WROVER-E — the PSRAM die uses them, and driving them corrupts PSRAM or the DALI line. This is a module-level wiring caveat, not a schema error: they are ordinary pins on WROOM, and the S3 puts PSRAM elsewhere, so `tx_pin`/`rx_pin` accept them and it is on you to avoid them on a WROVER. The pin schema does check what the chip can actually do, and rejects an input-only pin (GPIO34-39 on the classic ESP32) named as `tx_pin`. The controller has no proven collision-detection/arbitration strategy. Existing DALI-1 pushbutton couplers in direct-control mode coexist on the installed buses, but simultaneous transmissions remain a risk; see `headless_dispatch`.

### Cores

The DALI worker and the scan task both block on bus round-trips — a full scan for tens of seconds — so on a dual-core part they are pinned to Core 1 and the ESPHome main loop keeps Core 0 to itself. Two cores is what the firmware is tested on.

A single-core target is handled rather than supported: `dali_core_affinity.h` requests no affinity when the part reports one core, because `xTaskCreatePinnedToCore()` fails outright against a core that does not exist. Nothing here has been built or run on one, and a scan there would share its core with Wi-Fi and the API for the whole walk.

### Memory

Both columns are the same tree (`dev`, 2026-09-04) built for the classic ESP32 with esp-idf: [dali_test.yaml](dali_test.yaml) — the CI config, which enables everything the component has — and the same config with the `shell:` block removed.

| Segment | With `shell:` | Without | Region |
|---|---|---|---|
| Static DRAM (`.data` + `.bss` + `.noinit`) | 133.9 KiB (76%) | 69.1 KiB (39%) | 176.5 KiB (`dram0_0_seg`) |
| IRAM (`.text` + `.vectors`) | 76.7 KiB (60%) | 76.7 KiB (60%) | 128 KiB (`iram0_0_seg`, default) |
| App image | 1,053,739 B (57%) | 973,083 B (53%) | 1.75 MB partition |

What is left of the static window — 42.6 KiB with the shell, 107.4 KiB without — joins the ~128 KiB D/IRAM pool as heap, less whatever Wi-Fi and the API hold at runtime. Every buffer in the protocol stack is fixed-size, so none of this grows with the number of fixtures on the bus, and none of it shrinks on a small one either. On a real board the number to watch is free heap, which ESPHome's `debug:` component reports; the link-time figure only says where you start.

**The diagnostic shell is the largest single item, by a distance.** Its session caches — the input-instance cache, two capture rings, two inventories, the held backup — are 62 KiB of static `.bss` between them. `dali_shell.c` is compiled unconditionally (`proto_dali_shell.c` is a build shim), but nothing outside the shell's own TCP front end references it, so with no `shell:` block the linker drops all of it: **64.8 KiB of RAM and 78.8 KiB of flash back**. Once a bus is commissioned, that is the cheapest headroom on offer.

**`sram1_as_iram: true` is inert at this size** IRAM sits at 76.7 KiB of the default 128 KiB — the same with or without the shell, whose code is all in flash — so there is no overflow for it to relieve. An image whose code really does land above `0x400A0000` needs a bootloader that knows the region, so a device OTA'd from a bootloader older than the option can fail to boot.

### Persistent state

Two things survive a reboot, both through ESPHome's preferences API — which on ESP32 means the NVS partition the default layout already provides. No extra partition, no filesystem, nothing to add to the board:

| What | Size | Written when |
|---|---|---|
| Scan-verified group membership | 144 B | a scan, or an add-group/remove-group console command, changes it |
| Address backup (`backup save`) | up to 2448 B | `backup save`, or a completed `backup import` |

Both are staged in RAM by the task that produced them and written by the main loop, because the preferences API is Core 0 only. NVS is flushed on ESPHome's `flash_write_interval` — 60 s by default — or at a clean shutdown, so a `backup save` seconds before a power cut is not on flash yet. After a reboot, `backup status` reporting `loaded from storage` is the confirmation that one made it. Reboots and OTA updates keep it; erasing the chip or moving the partition layout does not.

The native ESP-IDF serial firmware passes no persistence hooks at all, so a backup taken there lives until reboot and `backup export` is how it leaves the device. The shell says which case you are in; see [commissioning_readme.md](commissioning_readme.md).

## Getting started

1. Flash [dali-starter.yaml](dali-starter.yaml) (`esphome run dali-starter.yaml`).
2. Commission or inspect the bus:
   - **From a terminal.** Run [tools/dali-shell](tools/dali-shell) from any host
     that can reach the device — standard library only, nothing to install, and
     it drops straight into the HA "Advanced SSH & Web Terminal" add-on. Copy it
     to `/config` there and run it with the interpreter, since copying over
     Samba or the file editor drops the execute bit:

     ```sh
     python3 /config/dali-shell                 # interactive session
     python3 /config/dali-shell --list          # nodes answering on this network
     python3 /config/dali-shell discover        # one command, then exit
     python3 /config/dali-shell export inventory > /config/dali_inventory.json
     python3 /config/dali-shell export config > /config/dali_block.yaml
     ```

     With no `--host` it finds the device over mDNS and connects, asking only if
     more than one answers. `discover` maps short addresses, device types and
     groups; `identify <addr>` blinks one fixture; `export inventory` writes the
     result as JSON. `help` lists every verb. With no client to hand,
     `nc <address> 2323` is a complete if unfriendly substitute.

     The commissioning entry point is `commission unaddressed [first] [max]`.
     Over TCP it is refused unless the firmware sets
     `shell: { allow_commissioning: true }`; the native serial shell permits it.
     Timestamped reply-activity handling and the safety `TERMINATE` cleanup path
     are host-tested, not real-bus proof of multi-device commissioning. Use one
     unaddressed control gear at a time for now.

     `backup save` records which physical unit — by its Bank 0 identification
     number, which no addressing operation changes — holds which short address,
     and `restore plan` / `restore apply` put them back afterwards using plain
     addressed commands and no `INITIALISE` window. Take one before anything
     that re-addresses in bulk. `backup export` prints it as the `backup import`
     script that reads it back, which is how a backup is kept off a device with
     no persistent store. Host-tested; no bus has run a restore.

     `export config` answers the other half: it prints the `dali:` block, and
     the `light:` and `sensor:` entries naming it, as YAML you can paste back.
     It is reconstructed from the running firmware — the source YAML never
     reaches the ESP32 — so the pins, entities, and dispatch rules it prints
     are the ones in force rather than the ones in a file that may have moved
     on. Run `discover` first and gear the bus reported that no entity drives
     is appended commented out, ready to uncomment; so is a draft `sensor:`
     entry for every input instance nothing reads, carrying the address,
     instance, and value width the scan read off the device.

     It is a `dali:` block, not a backup. Everything ESPHome owns is invisible
     to it — the node's own blocks, an entity's unit, device class, id,
     `internal`, filters and automations, and the `button:`, `number:` and
     `text:` platforms — so treat it as a diff against your source rather than
     a file to restore from. The emitted header says the same.
   - **From Home Assistant.** **Scan DALI Bus**, **Find Couplers**, and
     **Identify** do the same walk from a phone. A scan publishes ready-to-paste
     light YAML only when group discovery is complete; otherwise it retains the
     previous map and asks you to retry.

   The two are separate implementations of the same walk, so running both is
   itself a diagnostic — if they disagree, the disagreement is the finding.
3. Write your own config from the example below and the scan output.

### Example configuration

A bus with two lamp groups, one individually addressed lamp, and a DALI-2 multi-sensor (e.g. Steinel HF 360 II) at short address 0:

The example pins `v1.3.0`, the last tagged release. `dev` carries newer work;
compile and test it separately before pointing an installation at it.

```yaml
esp32:
  variant: esp32   # classic ESP32, e.g. WROVER-E
  framework:
    type: esp-idf

external_components:
  - source:
      type: git
      url: https://github.com/peetervalk/dali-esp.git
      ref: v1.3.0
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
  shell:                   # diagnostic shell over TCP; omit the block entirely
    port: 2323             # and the shell is not compiled in at all
    idle_timeout: 10min    # reclaims the session from a terminal that dropped
    # The port is unauthenticated — the same posture as OTA and the web server,
    # but a lower bar than physical access to a UART — so the commissioning
    # verbs are refused unless this opts in. Leave it false on anything left
    # flashed on a shared network: one typed line can readdress a whole bus,
    # and RANDOMISE cannot be undone.
    allow_commissioning: false

light:
  - platform: dali
    dali_id: dali_bus
    name: "Living Room"
    target_type: group        # short | group | broadcast
    target_address: 0
                              # No query_address: a group entity needs one
                              # member's short address to poll for state, and
                              # the component asks the bus for one rather than
                              # being told. Set it only to pin a member.
  - platform: dali
    dali_id: dali_bus
    name: "Hallway"
    target_type: group
    target_address: 1

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
- [commissioning_readme.md](commissioning_readme.md) — commissioning workflow: flash, walk the bus, export a config
- [dali_commands.md](dali_commands.md) — every verb and named command table, for the shell and the HA console
- [dali_protocol.md](dali_protocol.md) — frame layouts, opcode tables by IEC part, event decoding
- [dali_capability_matrix.md](dali_capability_matrix.md) — per-capability status: shared API, native CLI verb, host vector, real-bus result, ESPHome surface
- [current_status.md](current_status.md) — project state, known limitations, roadmap
- [project_log.md](project_log.md) — verification history, investigations, and the unreleased-change list behind those claims
- [steinel_bank2_reference.md](steinel_bank2_reference.md) — Steinel HF 360 II memory bank tuning

## Scope and limitations

- Control gear: DT6 (LED) and DT8 (colour) are implemented. Other device types (DT0 fluorescent, DT1 emergency, …) are not.
- One DALI bus per controller. Direct-control couplers are additional transmitters;
  simultaneous traffic is not collision-safe.
- Commissioning preserves frame-like, undecodable reply-window activity for
  COMPARE and attempts an abort-bypassing safety `TERMINATE` if the workflow is
  cancelled. Those paths are host-tested only: real-bus multi-device
  commissioning remains unverified, so the supported operating procedure is one
  unaddressed gear at a time. A run brackets itself with broadcast Part 103
  START/STOP QUIESCENT MODE so control devices cannot transmit into a COMPARE
  reply window, but that too is host-tested only and cannot reach a device that
  missed the broadcast. Two gear that draw the same random address are detected
  at VERIFY, de-addressed, and left for a second run to place -- host-tested,
  same caveat. Cross-part addressing interference is guarded in one direction —
  a run brackets itself with Part 103 TERMINATE so a control device cannot sit in
  its own addressing state and answer COMPARE as gear — but control-device
  commissioning itself, and multi-master arbitration, are not implemented.
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
