# DONE: DALI-2 Input Device ESPHome Integration

**Implemented 2026-06-26.** See `current_status.md` session notes. Summary:
polling approach used for all 4 Steinel instances (event-driven abandoned — hold
timer too long to observe transitions, lux events use different scale). Sensor
platform at `esphome/components/dali/sensor/`. Occupancy tunable from HA via
text command console (`iconfig a0:1 set-hold-timer <N>`).

---

Original design notes preserved below for reference.

# ORIGINAL: DALI-2 Input Device ESPHome Integration

## Context

DALI-2 input devices (sensors, push buttons) send unsolicited 24-bit frames on the bus.
The ESP32 already receives and parses these via `dali_event.c`. The dispatch layer
already buffers them through `DaliInputEventQueue`. The integration gap is routing
parsed events to ESPHome `binary_sensor` / `sensor` entities so HA can act on them.

This file covers design decisions made in conversation on 2026-06-24. Implementation
deferred until Steinel HF 360 II hardware is tested.

---

## Steinel HF 360 II — known instance layout

EAN `4007841064280`, DALI Alliance ID `3742`.
Parts: IEC 62386-101, -103, -303, -304.

| Instance | Type | Standard | Meaning |
|---:|---:|---|---|
| 0 | 4 | DT304 | Light sensor (lux) |
| 1 | 3 | DT303 | Occupancy (motion) |
| 2 | 0 | generic | Temperature (`T_C = value × 0.1 − 5`) |
| 3 | 0 | generic | Humidity (`H% = value × 0.5`) |

Event reporting vs polling:
- **Occupancy (instance 1)**: sends event frames on transition (occupied / free).
  No polling needed. Configure `SET_HOLD_TIMER` via `dali_input_config` to set the
  unoccupied delay.
- **Light level (instance 0)**: configure `SET_REPORT_TIMER` and `SET_HYSTERESIS`
  so the device self-reports on significant change. Polling is a fallback.
- **Temperature / humidity (instances 2, 3)**: generic instances; polling via
  `QUERY_INPUT_VALUE` is likely necessary unless the device self-reports.

---

## Architecture

### New ESPHome entity types needed

Two new component types under `esphome/components/dali/`:

**`DaliOccupancySensor`** (`binary_sensor`):
- Registered at `{short_address, instance}` — matches incoming 24-bit frames
- `True` on occupied event code, `False` on free/unoccupied event code
- DT303 event codes to confirm from hardware (likely 0x01 = occupied, 0x00 = free)

**`DaliInputSensor`** (`sensor`):
- For light level, temperature, humidity
- Registered at `{short_address, instance}`
- Value from 24-bit event frame or from `QUERY_INPUT_VALUE` polling
- Apply scale factor in entity config (`scale: 0.1`, `offset: -5.0`) for temp/humidity

### YAML shape (draft)

```yaml
binary_sensor:
  - platform: dali
    dali_id: dali_bus
    address: 5          # Steinel short address
    instance: 1         # occupancy instance
    name: "Zone 1 Occupancy"

sensor:
  - platform: dali
    dali_id: dali_bus
    address: 5
    instance: 0         # light level
    name: "Zone 1 Lux"
    poll_interval: 60   # seconds; 0 = event-driven only

  - platform: dali
    dali_id: dali_bus
    address: 5
    instance: 2         # temperature
    name: "Zone 1 Temperature"
    scale: 0.1
    offset: -5.0
    poll_interval: 120
```

### Input device registry in DaliComponent

Parallel to the light registry (target → DaliLightOutput*), add:

```
{short_address, instance} → DaliInputEntityBase*
```

`DaliInputEntityBase` is a small abstract class with `on_event(uint8_t event_code)`
and (for sensors) `on_value(uint16_t raw_value)`. Both `DaliOccupancySensor` and
`DaliInputSensor` inherit from it.

Routing: after the control dispatch, the component checks if the 24-bit frame's
`{address, instance}` matches an entry in the input registry and calls `on_event()`.

Cross-core handoff: same dirty-flag pattern as light snooping — Core 1 sets an
atomic event code; Core 0 `loop()` reads it and calls `publish_state()`.

### HA automation flow

```
Steinel 24-bit event frame
  → ESP32 unsolicited-RX callback
  → dali_event_parse_frame()
  → headless dispatch (optional: issue lighting command directly)
  → input device registry → binary_sensor / sensor entity
    → publish_state() → HA sees occupancy = on
      → HA automation triggers
        → light.turn_on / light.turn_off via ESPHome API
          → write_state() → DALI control command
```

HA automations use standard entity types — no DALI awareness needed in the
automation YAML itself.

### On-device automation (no HA round-trip)

For latency-critical response (occupancy → lights on in <100 ms):

Add a second dispatch table type for input device events that maps
`{address, instance, event_code}` → `DaliDispatchEntry`. This runs entirely on
Core 1 without touching HA. The HA path (binary_sensor publish) still fires for
logging and state visibility.

Requires: dispatch table extended with 24-bit input event entries.
`DaliDispatchKey.frame_kind = DALI_EVENT_FRAME_INPUT_24BIT` already supported.

---

## Open questions (decide when hardware is in hand)

- Exact DT303 event codes for occupied / unoccupied on Steinel HF 360 II
  (verify against live bus trace — spec says 0x01/0x00 but vendor may differ).
- Does the Steinel self-report light level, temperature, humidity or must they be
  polled? Check `QUERY_INPUT_VALUE` response and `SET_REPORT_TIMER` behaviour.
- Instance 2/3 (temp/humidity): are they generic (type 0) or type-specific?
  Query `QUERY_INSTANCE_TYPE` on hardware to confirm.
- Quiescent mode interaction: does the Steinel stop reporting when quiescent mode
  is active? Test before relying on event-driven approach.
- Multi-instance event collisions: if motion + light level change simultaneously,
  do frames collide? DALI-2 Part 103 addresses this via priority/scheme; check
  device configuration.

---

## Implementation prerequisites

Before implementing input device entities:
1. Bus snooping + light state sync must be working (cross-core dirty-flag
   infrastructure is shared).
2. Steinel hardware connected to a bus where the existing master is removed.
3. Live bus trace with DALI Cockpit to confirm event codes and frame timing.
4. `dali_input_config` commands tested: `SET_HOLD_TIMER` for occupancy,
   `SET_REPORT_TIMER` / `SET_HYSTERESIS` for light level.
