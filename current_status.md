# DALI-ESP Current Status

**Last updated:** 2026-06-17
**Framework:** ESP-IDF v6.0.1 native CMake
**Hardware target:** ESP32-DevKitC-VE / ESP32-WROVER-E + MikroE DALI-2 Click

## Where Things Stand

The software stack is complete through the first real-bus baseline. PHY,
scheduler, protocol, commissioning, discovery, and control layers are all
implemented and covered by 13 host test suites (16 in the discovery suite).

`discover` is now a full one-shot command: per found device it queries status,
group membership (16-bit bitmask), device type, DALI version, actual level, and
number of input instances. If a device responds to the instance count query,
`discover` automatically enumerates its instances and caches them — no separate
`instances <addr>` step needed. The JSON export includes all these fields;
`kind` is now `control_gear`, `input_device`, or `unknown` instead of the
previous `control_gear_candidate`.

On hardware, the ESP32 boots cleanly, the diagnostic shell is reachable over
COM6, and bidirectional DALI communication with Lunatone is confirmed: passive
RX decodes correctly (Broadcast OFF `0xFF00`, Recall Max) and Lunatone confirms
ESP-originated TX. The MikroE DALI-2 Click optocoupler inversion is handled in
the PHY; polarity, glitch filtering, frame-gap detection, and settle suppression
are all working on real hardware.

**The next milestone is 8-bit device replies.** This requires a real DALI
control gear on the bus. Once reply RX is solid, the Steinel HF 360 II sensor
and DALI-2 instance discovery follow.

### Terminology note
"Pushbutton coupler" is the correct term for devices that inject DALI bus
commands when a physical button is pressed (both DALI-1 legacy controllers and
DALI-2 push-button input devices). The word "switch" is avoided in code and
documentation because Home Assistant uses "switch" for a binary on/off toggle
entity, which is a different concept.

## Immediate Priorities

1. TX pattern sweep with Lunatone only: `max b`, `min b`, `raw 0xFE80 len=16`,
   `raw 0xFF05 len=16`.
2. Add one known DALI control gear; run `scan` and confirm 8-bit replies decode.
3. Compare ESP frames and replies against Lunatone DALI Cockpit captures.
4. Bring up the Steinel HF 360 II after control gear replies are stable.
5. Only after native diagnostics are reliable: ESPHome-flashable diagnostic
   firmware (`todo_esphome_release.md`).

## Architecture

```text
ESPHome / Home Assistant integration      (stub)
DALI entity mapping / release integration (mapping helpers ready, release future)
DALI discovery / inventory helpers        (implemented, host-tested)
DaliControl                               (implemented)
DaliProtocol                              (implemented, hardware sensor polling pending)
DaliScheduler                             (implemented, host-tested)
DaliPhy                                   (implemented, hardware-verified for TX/RX baseline)
DaliRingBuf                               (implemented, host-tested)
DALI-2 Click
DALI bus
```

## Known Target Sensor

Steinel HF 360 II DALI-2 IPD UP — EAN `4007841064280`, DALI Alliance ID `3742`.
Parts: IEC 62386-101, -103, -303, -304.

Expected instances:

| Instance | Type | Meaning |
|---:|---:|---|
| 0 | 4 | Brightness |
| 1 | 3 | Motion |
| 2 | 0 | Temperature (`T_C = binValue × 0.1 − 5`) |
| 3 | 0 | Humidity (`H_percent = binValue × 0.5`) |

## Open Questions

| Topic | Status |
|---|---|
| HA brightness 0 behavior | Prefer explicit `OFF`; confirm on hardware. |
| ESPHome component packaging | In-tree custom component first; external later. |
| DALI-2 firmware update / DFU | Out of scope. |

## Build Commands

Native ESP-IDF:

```powershell
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
idf.py build
idf.py -p COM6 flash monitor
```

Host tests:

```powershell
cd test
C:\msys64\ucrt64\bin\mingw32-make.exe --directory build
C:\msys64\ucrt64\bin\mingw32-make.exe --directory build test CTEST_OUTPUT_ON_FAILURE=1
```
