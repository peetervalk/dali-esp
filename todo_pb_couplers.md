# TODO: DALI-1 Pushbutton Coupler Support

## What we know

- Bus has two Lunatone/Tridonic DALI MC PB couplers, 7 physical switches total
- Couplers are configured with BF6 function (legacy DALI-1, not DALI-2 Part 103)
- Each button sends one of two legacy 16-bit frames targeting a DALI group address:
  - `recall-max` (0x05) = ON button
  - `off` (0x00) = OFF button
- Groups in use: 0, 2, 3, 4, 5, 6, 7

## Discovery

- `find switches` passive listen works: `found switches 14, dropped 0`
- 7 zones × 2 actions = 14 unique frames detected correctly
- Cannot query couplers directly (no short address, no DALI-2 input device support)
- Lunatone uses proprietary eDALI protocol to interrogate couplers — not replicable

## What Lunatone does (from bus trace)

1. INITIALIZE + START QUIESCENT MODE (DALI-2 broadcast) — DALI-1 couplers likely ignore this
2. Broadcast DALI-2 device command 0xCC with DTR0/DTR1 — probably memory writes to DALI-2 devices
3. eDALI vendor frames (`05 01 XX`) to read coupler config directly — proprietary, not standard

## Proposed improvement: zone-based grouping

Currently `find switches` maps 14 entries (one per unique frame).
Should group by `(kind, address)` into 7 zones, each holding its action frames:

```
Zone 1: group 6 → actions: [recall-max (0x8D05), off (0x8D00)]
Zone 2: group 7 → actions: [recall-max (0x8F05), off (0x8F00)]
...
```

Struct: replace flat `DiagSwitchMapping[]` with `DiagSwitchZone[]` (zone key = kind+address,
up to 4 action frames per zone). JSON export nests actions under zones.
See conversation from 2026-06-21 for full sketch.

## Open questions

- For HA: one entity per zone (7) or one per action (14)?
- How to handle button holds, double-press, or scene recalls from the same coupler?
- QUIESCENT MODE during `scan` — worth sending even if DALI-1 devices ignore it?
