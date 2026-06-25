# ESPHome Release And End-User Discovery Plan

ESPHome is the end-user integration path, not the low-level stack debugging
environment. Native ESP-IDF diagnostics must prove the DALI stack first.

## Release Strategy

Ship two user-facing firmware modes:

1. **Diagnostic/discovery firmware**
   - Prebuilt and flashable through ESPHome Web / ESP Web Tools.
   - Provides Wi-Fi setup, local web UI, logs, OTA, bus discovery, switch
     training, and inventory export.
   - Helps users learn their bus addresses, groups, instances, and physical
     switch mapping.

2. **Final ESPHome entity firmware**
   - Built from explicit YAML/config created from discovery results.
   - Exposes Home Assistant lights, sensors, binary sensors, and buttons.
   - Uses `DaliControl` and protocol APIs; does not build raw frames in entity
     code.

## End-User Workflow

1. User connects the ESP32 gateway over USB.
2. User flashes diagnostic/discovery firmware with ESPHome Web / ESP Web Tools.
3. User configures Wi-Fi if needed.
4. User opens the local web UI.
5. User runs:
   - [ ] Bus check.
   - [ ] Address scan.
   - [ ] Device discovery.
   - [ ] Lamp identify.
   - [ ] Sensor poll.
   - [ ] Find couplers.
6. User exports or copies the discovered inventory.
7. User fills final ESPHome YAML with friendly names and chosen semantics.
8. User flashes final firmware by OTA or browser upload.

## Diagnostic Firmware UI

Minimum controls:

- [ ] Bus status.
- [ ] `scan` / `discover`.
- [ ] `inventory`.
- [ ] `identify <addr>`.
- [ ] `sensor poll <addr>`.
- [ ] `find couplers`.
- [ ] Advanced raw command panel.
- [ ] Export inventory JSON/YAML snippet.
- [ ] OTA upload for final firmware.

## Final ESPHome Mapping

Final entity config should be explicit. Discovery suggests values; the user
chooses names and semantics.

Example mapping idea:

```yaml
dali:
  lights:
    - id: kitchen_ceiling
      target:
        type: group
        address: 0
    - id: hall_spot
      target:
        type: short
        address: 12

  sensors:
    - id: hall_presence
      device_address: 5
      profile: steinel_hf360_2
      instance: 1

  couplers:
    - id: hallway_left
      device_address: 8
      instance: 0
      event: double_press
```

## ESPHome Integration Design Notes

Notes from reviewing jorticus/esphome-dali and general ESPHome patterns.
Capture these before starting so we don't rediscover them the hard way.

### Entity types needed

| HA domain | DALI concept | Notes |
|---|---|---|
| `light` | Control gear (DT6, DT8) | Brightness + colour temp for DT8 Tc gear |
| `sensor` | Input device instance values | Brightness lux, temperature, humidity |
| `binary_sensor` | Occupancy/motion instance | DT303 presence output |
| `button` | Scene recall | "Recall scene N" on a DALI target |
| `output` | Raw DALI level (broadcast) | Simpler than a full light for broadcast-only |

### Dynamic entity creation

For the discovery firmware, auto-create entities from scan results rather than
requiring fully static YAML. Approach: run `dali_discovery_scan` at boot with
`discovery: true`, then register one `light` per found control gear device.

This is the same pattern jorticus uses and is the right UX — users shouldn't
have to know short addresses in advance.

### Addressing in YAML

Expose the full `DaliTarget` model in entity config:

```yaml
target:
  type: short   # or group, broadcast
  address: 12
```

Groups and broadcast must be first-class options, not special-cased values.
Capability queries (device type, DT8 features) are only possible with short
addresses — document this clearly.

### Commissioning flag

Expose `initialize_addresses: true` as a one-shot YAML option that calls
`dali_commissioning_run()` at first boot (or when no assigned devices are
found). This avoids requiring users to run the serial `commission` command.

### Colour temperature slider range

For DT8 Tc devices, expose the physical limits from discovery (QueryColourValue
Tc_min/Tc_max) as the slider bounds. Also accept user-override in YAML:

```yaml
cold_white_color_temperature: 4000K
warm_white_color_temperature: 2700K
```

Without this, the Home Assistant UI defaults to a nonsense 153–500 Mirek range
that doesn't match the gear's actual capability.

### Restore-on-boot mode

ESPHome light entities need an explicit restore policy or they come up in unknown
state. Default to `RESTORE_DEFAULT_ON` for control gear lights. Add a YAML
override for installations that require lights to stay off after a power cycle
(e.g. safety-critical environments).

### Startup bus-write storm / BUS_STUCK during headless test

Observed on 2026-06-24 after flashing `dali_1k.yaml`: connecting the ESPHome
headless firmware turned all lights off, while physical switches then worked and
were logged correctly. On later flashes/reboots, startup still turned all groups
off and then turned one random/different group back on. The serial log also
showed a burst of:

```text
DALI-SCHED: phy_tx failed: 2
```

Error 2 is `DALI_ERR_BUS_STUCK` (PHY idle wait timed out; bus appeared held
active/low). Known/suspected combined cause:

- Generated ESPHome code defaults every DALI light to `restore_mode: ALWAYS_OFF`.
  `DaliLightOutput::write_state()` currently treats that initial/default off
  state as a real command, so it enqueues OFF for every configured group.
- The later random group-on was not explained by `ALWAYS_OFF`; suspect a startup
  TX/RX interaction, stale queued unsolicited event, or a valid BF6 coupler frame
  being received during/after the OFF storm and then mirrored by the old
  direct-BF6 headless mapping.
- That startup command burst may collide with bus activity from the DALI-1 BF6
  couplers or with the bus settling as the Click board is connected/powered.
- If it reproduces even without entity startup writes, also re-check ESP-side
  idle level/polarity at the MikroE Click logic pins and bus idle detection.

Prospective mitigation:

- Bundle the headless reliability fixes together rather than chasing symptoms one
  at a time:
  - [x] Suppress the first ESPHome light write after boot/registration. This
    should make boot passive and remove the deterministic all-groups-off
    behavior.
  - [x] Prefer boot state discovery (`QUERY_ACTUAL_LEVEL` via `query_address`)
    over commanding restore state onto the bus. For group entities, add a
    representative short-address `query_address` in the final YAML where useful.
  - [x] Replace direct-BF6 `MIRROR` with `OBSERVE`: in direct BF6 mode the
    coupler already commanded the lamps, so the ESP32 updates HA state without
    re-issuing the same DALI command. Keep active `MIRROR` translation for
    phantom-address modes.
  - [ ] Add a short headless startup gate so unsolicited events seen during PHY/API
    bring-up can update diagnostics/state but cannot immediately cause translated
    output unless intentionally allowed.
  - [ ] Rate-limit or coalesce initial DALI entity writes so a boot never creates a
    multi-group TX storm.
  - [ ] Add clearer logs around ignored initial writes, headless dispatch actions,
    and `DALI_ERR_BUS_STUCK` with sampled RX idle level / recent TX context.
- [x] Clean up brightness-only ESPHome state publishing so unused RGB/white/colour
  temperature fields are explicitly zeroed. HA already advertises
  `supported_color_modes: brightness`, so this is log/API hygiene rather than
  a functional colour-capability fix.

Follow-up observation after the headless fix commit: RGB state noise is gone and
on/off BF6 frames update/log correctly, but flashing still turns all lights off.
Dimming did not update or log because the couplers appear to send DAPC-level
frames during dimming; `OBSERVE` originally rejected selector=0 DAPC frames.

Follow-up mitigation:

- [x] Treat direct BF6 DAPC frames as observed brightness levels without TX.
- [x] Let phantom-address `MIRROR` translate DAPC levels too, for future phantom
  dimming support.
- [x] Add explicit debug logs for unsolicited RX, dispatch results, startup write
  suppression, and firmware-originated light writes.
- [x] Set the TX GPIO idle level before switching it to output and enable the
  internal pulldown during firmware init, reducing app-init TX glitches.
- [ ] Retest boot after flashing. If lights still go off without any
  `dali.light: tx off` / `tx level` log, suspect TX pin reset/floating behavior
  on the MikroE Click path rather than ESPHome restore writes. Mitigations to
  evaluate: add/verify an external pulldown so ESP32 reset/flashing cannot drive
  the Click TX input active.

### Scene recall button entity

We query and store scene levels during discovery — wire that up as ESPHome
`button` entities ("Recall scene 3 on group 0"). This is a real use case for
zone-level preset control (e.g. "presentation mode", "evening dim") that plain
brightness sliders don't cover.

### ESPHome version compatibility

ESPHome breaks external component APIs regularly between major releases. The
jorticus project needed a fix for ESPHome 2026.4 and ESPHome 2026.6 was also
a significant change. Mitigate this by:
- Keeping the ESPHome layer as thin as possible (entity wrappers only, no
  protocol logic).
- Pinning the minimum compatible ESPHome version in `manifest.json`.
- Not starting full ESPHome integration until the native firmware is hardware-
  proven — the protocol layer cannot be re-tested if ESPHome breakage occupies
  the debug cycle.

### What NOT to put in ESPHome entities

- Frame building or raw DALI opcodes — use the `DaliControl` API.
- Timing constants or retry logic — belongs in the scheduler.
- Any awareness of DT6/DT8 opcode specifics.
- Discovery logic — run that at boot and hand results to the entity layer.

## Boundaries

- Do not require the end user to install ESP-IDF.
- Do not use ESPHome to debug PHY timing or scheduler correctness.
- Do not infer room names from DALI bus data.
- Do not put DALI protocol logic in ESPHome entities.
- Do not start full ESPHome integration until native firmware works reliably on
  real hardware.

## Implementation Order

1. Keep native ESP-IDF diagnostics as the reference implementation.
2. Reuse the native discovery/inventory APIs in `components/dali`; keep them
   ESPHome-independent.
3. Add a simple diagnostic web/UI surface in the ESPHome-flashable firmware.
4. Add inventory export.
5. Wire final static entity mapping through the existing `dali_mapping` helpers.
6. Migrate the component packaging from in-tree custom component to external
   component only after the flow is stable.
