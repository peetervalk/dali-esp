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

## Quiescent mode

The Tridonic DALI MC datasheet lists compliance as "IEC 60929 (DALI V0) and IEC 62386
(DALI V1)". QUIESCENT MODE is a DALI-2 Part 103 (input device) command — these devices
are not Part 103 compliant on paper. The Lunatone software sends it, but likely targets
DALI-2 devices elsewhere on the same bus, not the MC couplers specifically.
Worth sending anyway (a 24-bit frame is ignored by DALI-1 gear, harmless). Test
experimentally: send QUIESCENT MODE, press a button, check if frames still appear.

## Potential option: ESPHome integration via non-actionable phantom frames

"MC" = Multi-Controller — these couplers send DALI commands directly to the bus, making
them autonomous controllers. To integrate cleanly with ESPHome (where the ESP32 decides
what to do on a button press), the couplers need to send frames that are IDENTIFIABLE
but NOT ACTIONABLE. This would in essence make them not a controller — buttons would
have no effect if the ESP32 is offline.

### Approach A — GO TO SCENE on MASK scenes

DALI devices default to MASK (0xFF) for unprogrammed scene slots. RECALL SCENE N where
all fixtures have scene N = MASK → devices receive the frame, do nothing, ESP32 gets
a clean trigger.

- Assign each button a unique scene number (e.g. ON buttons → scenes 8–14,
  or encode zone via group address + scene number)
- First verify: `query s0 scene-level 8` through `scene-level 15` on several fixtures
  to confirm those slots are MASK
- 16 scenes × possible group targeting gives enough encoding space for 14 buttons

### Approach B — Commands to unused short addresses (recommended)

Scan confirmed addresses 0–15 occupied, 16–63 free. Assign each zone a phantom address:

```
Zone (group 6):  RECALL MAX → short addr 16   (ON button)
                 OFF        → short addr 16   (OFF button)
Zone (group 7):  RECALL MAX → short addr 17
                 OFF        → short addr 17
... (7 zones → short addrs 16–22)
```

No fixtures at those addresses → completely inert on the bus. ESP32 decodes: address =
zone, command = action. Unambiguous, no scene audit needed.

### Implementation

Both approaches require reprogramming the couplers via **Macro 8** (Tridonic DALI MC:
user-defined COT file, up to 10 commands per button) using masterCONFIGURATOR software.

### Trade-off and failure modes

With current BF6: the bus works with zero masters — lamps respond to buttons even if
the ESP32 is unplugged. Phantom frames break this.

Three scenarios to decide on before committing:

| Scenario | Current BF6 | Phantom frames |
|---|---|---|
| ESP32 up, ESPHome connected | Lights respond immediately; ESPHome informed after the fact | ESPHome owns the action; clean |
| ESP32 up, ESPHome disconnected | Lights still respond (coupler is master) | **Nothing happens** unless ESP32 firmware applies fallback translation |
| ESP32 dead, bus PSU live | Lights respond (coupler is master) | **Nothing happens** — no translation possible |

**Firmware failsafe for scenario 2:** ✓ **Dispatch infrastructure implemented
(2026-06-24).** `dali_dispatch` + `dali_headless.cpp` provide the local mapping
table. In BF6 mode the dispatch runs unconditionally (no HA dependency). For
phantom-frame mode, only the mapping table in `dali_headless.cpp` needs updating —
the dispatch engine already supports `MIRROR` on short addresses. Connection-loss
detection is not explicitly implemented because BF6 dispatch does not require it;
add it when switching to phantom frames if needed.

**Scenario 3 is an accepted risk for this installation.** The DALI-1 PB couplers are
aging hardware well past their early-life period and are statistically more likely to
fail than a new ESP32 devkit. In a single-owner home installation where the only person
changing the system is the owner, scenario 3 (ESP32 dead) is an acceptable dependency.

> **Caveat for anyone reusing this codebase in a different context:** if the installation
> requires physical switches to work without any DALI master present (e.g. rental
> property, commercial space, or safety-critical lighting), phantom frames are the wrong
> approach. Keep the couplers in BF6 or equivalent direct-control mode and use the ESP32
> purely as an observer/recorder.

### Power-on level and system failure level

Currently lamps go to MAX after a power cut (POWER ON LEVEL = MASK → device default).
First button press after power cut with BF6 may send `recall-max` (no visible change)
or `off` — this is the expected DALI-1 toggle behavior.

With phantom frames, the power-on level decision becomes independent:
- Set `POWER ON LEVEL = 0` → lamps off after power cut → need ESP32 to turn on
- Set `POWER ON LEVEL = 254` → lamps at max → buttons only work if ESP32 is up (scenario 3)
- Keeping POWER ON LEVEL at MASK/max + firmware failsafe (scenario 2 fix) is probably
  the most robust combination.

**Diag shell already supports:** `config b set-power-on-dtr0 <0-255>` (broadcast to all gear).
**Gap:** `DALI_CMD_SET_SYSTEM_FAILURE_LEVEL_DTR0` exists in the protocol but is not
exposed in the diag config spec table — add it alongside set-power-on-dtr0.

## Open questions

- For HA: one entity per zone (7) or one per action (14)?
- How to handle button holds, double-press, or scene recalls from the same coupler?
- Confirm masterCONFIGURATOR COT file format for Macro 8 before reprogramming
- Decide on phantom-frame approach before reprogramming (can't easily undo without
  the software and cable on-site again)
- Add set-system-failure-dtr0 to diag config spec table (small gap, one line)
- ~~Implement ESP32 firmware failsafe~~ — **done**: `dali_dispatch` + `dali_headless.cpp`
  provide the local mapping table; BF6 mode runs unconditionally without HA
