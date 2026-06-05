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
   - [ ] Find switches.
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
- [ ] `find switches`.
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

  switches:
    - id: hallway_left
      device_address: 8
      instance: 0
      event: double_press
```

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
