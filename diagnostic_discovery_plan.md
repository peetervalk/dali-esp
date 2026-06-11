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

- Full-bus readdressing of already addressed devices.
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

Implemented native CLI commands:

```text
bus check
capture start|stop|clear|status|export
scan
level <addr|sN|gN|b> <0-254>
off <addr|sN|gN|b>
up <addr|sN|gN|b>
down <addr|sN|gN|b>
step-up <addr|sN|gN|b>
step-down <addr|sN|gN|b>
step-off <addr|sN|gN|b>
on-step <addr|sN|gN|b>
dapc-seq <addr|sN|gN|b>
last <addr|sN|gN|b>
scene <addr|sN|gN|b> <0-15>
max <addr|sN|gN|b>
min <addr|sN|gN|b>
status <addr>
query <addr>     # compatibility alias for status
query <addr|sN|gN|b> <query-name> [param]
query-list
config <addr|sN|gN|b> <config-name> [param]
config-list
discover
inventory
commission unaddressed [first-addr] [max-devices]
instances <addr>
sensor poll <addr> [instance]
smoke <addr>
events
find switches [seconds]
export inventory
identify <addr>
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
```

## Discovery Steps

1. Scan short addresses 0..63 with one scheduler transaction at a time.
2. Mark responders as present; mark timeouts as absent/unknown.
3. Query status for each responder.
4. Query basic capability information using the generic control-gear query path:
   - version number
   - device type
   - actual level for control gear
   - group membership
   - DALI-2 input-device metadata
5. Build an in-memory `DaliDiscoveryInventory`.
6. Print inventory in human-readable form.
7. Export inventory in a machine-readable form for final ESPHome configuration.

Suggested first hardware pass:

```text
bus check
capture clear
capture start
smoke <known_addr>
instances <sensor_addr>
sensor poll <sensor_addr>
find switches 30
capture stop
export inventory
```

## Physical Identification

For control gear:

- `identify <addr>` should visibly change the lamp, for example alternating
  `RECALL MAX LEVEL` and `RECALL MIN LEVEL` or another safe test pattern.
- The user can then assign a friendly name outside the protocol stack.

For switch/input couplers:

- `find switches` enters a timed training mode.
- For DALI-2 couplers, the user physically double-presses each switch when the
  device emits standard input-event frames.
- For legacy/DALI-1-style couplers, the user triggers the configured action; the
  firmware records the observed 16-bit target/action frame and trigger order.
- DALI-1-style 16-bit frames generally do not carry source device or instance
  identity, so export must treat them as raw target/action mappings.
- Double press is preferred where available as an intentional selection gesture
  to avoid accidental event capture.

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
        { "instance": 0, "type": 4, "role": "light", "source": "standard" },
        { "instance": 1, "type": 3, "role": "occupancy", "source": "standard" },
        { "instance": 2, "type": 0, "role": "temperature", "source": "vendor_profile" },
        { "instance": 3, "type": 0, "role": "humidity", "source": "vendor_profile" }
      ]
    },
    {
      "address": 12,
      "kind": "control_gear",
      "groups": [0, 2]
    }
  ],
  "switches": [
    { "order": 1, "frame_kind": "input-24bit", "address": 8, "instance": 0, "event": "double-press" },
    { "order": 2, "frame_kind": "legacy-16bit", "raw": "0x8B10", "action": "go-to-scene" }
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
- [x] Reusable `dali_discovery` module for scheduler-agnostic scan,
      inventory, and generic instance discovery.
- [x] Raw DALI-2 input value polling through `sensor poll <addr> [instance]`.
- [x] Raw 24-bit DALI-2 event parser, 16-bit legacy controller-frame parser,
      and fixed diagnostic event queue.
- [x] `find switches [seconds]` training from parsed DALI-2 double-press events
      and legacy 16-bit action frames.
- [x] Richer JSON `export inventory` with cached input instances and learned
      switch mappings.
- [x] Rolling diagnostic capture log with JSON export.
- [x] `bus check` health snapshot.
- [x] Query-only `smoke <addr>` pass for one-address bring-up.
- [x] Latest raw sensor values cached into JSON export.
- [ ] Steinel profile polling. Profile helpers exist; raw value polling is
      implemented, but real-bus profile application is pending.
- [ ] Validate event frame decode and double-press event code against Lunatone
      captures / real hardware.
- [ ] Export final ESPHome YAML snippets.
