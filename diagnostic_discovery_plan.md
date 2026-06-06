# Diagnostic Discovery Plan

Diagnostic discovery is a user-visible bring-up workflow, but it should run on
top of the same proven DALI stack used by native diagnostics. It discovers bus
facts; it does not decide room names or final Home Assistant semantics.

## Goals

- Find addressed DALI devices on the bus.
- Identify basic role and capabilities.
- Poll known DALI-2 input-device instances.
- Help the user locate lamps and switches physically.
- Export enough mapping information for final ESPHome YAML.

## Non-Goals For First Version

- Automatic unaddressed-device commissioning.
- Automatic room/name inference.
- Full DALI-2 value/profile auto-discovery.
- Firmware update / DFU for DALI devices.
- Dynamic Home Assistant entity creation.

## Required RX Model

Discovery depends on a clean RX split:

```text
Solicited reply -> scheduler transaction completion
Unsolicited event -> event queue / discovery listener
Ignored stale frame -> diagnostic counter
```

Unsolicited input-device events must never complete an unrelated request/reply
transaction.

## CLI Workflow

Initial native CLI commands:

```text
scan
status <addr>
query <addr>     # compatibility alias for status
discover
inventory
instances <addr>
identify <addr>
sensor poll <addr>
find switches
export inventory
```

Example:

```text
> discover
Scanning short addresses 0-63...
05: present, status=0x80
12: present, status=0x00
14: present, status=0x00

> identify 12
Blinking addr 12 between min and max for 10 seconds.

> find switches
Listening for 90 seconds. Double press each switch now.
1: addr=8 instance=0 event=double_press
2: addr=8 instance=1 event=double_press
```

## Discovery Steps

1. Scan short addresses 0..63 with one scheduler transaction at a time.
2. Mark responders as present; mark timeouts as absent/unknown.
3. Query status for each responder.
4. Query basic capability information when command support is ready:
   - version number
   - device type
   - actual level for control gear
   - group membership
   - DALI-2 input-device metadata
5. Build an in-memory `DaliInventory`.
6. Print inventory in human-readable form.
7. Export inventory in a machine-readable form for final ESPHome configuration.

## Physical Identification

For control gear:

- `identify <addr>` should visibly change the lamp, for example alternating
  `RECALL MAX LEVEL` and `RECALL MIN LEVEL` or another safe test pattern.
- The user can then assign a friendly name outside the protocol stack.

For switch/input couplers:

- `find switches` enters a timed training mode.
- The user physically double-presses each switch.
- The firmware records address, instance, event type, and trigger order.
- Double press is used as an intentional selection gesture to avoid accidental
  event capture.

## Inventory Shape

Draft export shape:

```json
{
  "devices": [
    {
      "address": 5,
      "kind": "input_device",
      "profile": "steinel_hf360_2",
      "instances": [
        { "instance": 0, "type": 4, "suggested_entity": "illuminance" },
        { "instance": 1, "type": 3, "suggested_entity": "motion" },
        { "instance": 2, "type": 0, "suggested_entity": "temperature" },
        { "instance": 3, "type": 0, "suggested_entity": "humidity" }
      ]
    },
    {
      "address": 12,
      "kind": "control_gear",
      "groups": [0, 2]
    }
  ],
  "switches": [
    { "order": 1, "address": 8, "instance": 0, "event": "double_press" }
  ]
}
```

## First Implementation Order

- [x] Safe scheduler RX gating.
- [x] Last-frame storage and trace output.
- [x] `discover` as enriched `scan`.
- [x] `inventory` print command.
- [x] `identify <addr>` for lamps.
- [x] Generic `instances <addr>` for DALI-2 input-device count/type discovery.
- [ ] Steinel profile polling. Profile helpers exist; real-bus polling is
      pending.
- [ ] Unsolicited event queue.
- [ ] `find switches`.
- [ ] Export for ESPHome YAML.
