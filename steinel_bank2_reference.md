# Steinel HF 360-2 DALI-2 IPD — Memory Bank 2 Sensitivity Reference

Product: Steinel HF 360-2 DALI-2 IPD (art. 064280)  
Firmware context: `dali_2k` / `dali_2k_local`, sensor at short address 0  
Last reviewed: 2026-06-27

---

## Background

The HF 360-2 DALI-2 IPD is a **control device** (IEC 62386-103), not a control gear.
Sensitivity and detection range are not set over Bluetooth — the app variants
(Steinel Connect, SmartRemote) target the COM1/COM2/BT-IPD family; they will not
discover or connect to the 064280.  All commissioning is done over the DALI bus.

Bank 2 is the Steinel-proprietary NVM bank holding sensitivity and range parameters.
It must be unlocked before any write (see below); a standard DALI `RESET` reverts
everything in Bank 2 to factory defaults.

---

## Bank 2 Offset Map

Source: local copy of Steinel DALI-2 Interface Description V1.5 (see References).

| Offset | Parameter | Scale | Default | Notes |
|--------|-----------|-------|---------|-------|
| `0x02` | Lock byte | — | locked | Write `0x55` to unlock NVM writes before raw writes |
| `0x04` | Global sensitivity | `0x00`–`0xFF` | `0xFF` | Amplification of all motion signals, all directions |
| `0x05` | Global detection range | `0x00`–`0xFF` | `0xFF` | Minimum signal strength to register as motion; applies to HF and US technology |
| `0x06` | Detection range, direction 1 | `0x00`–`0x64` | — | Percent (0–100); per-direction override |
| `0x07` | Detection range, direction 2 | `0x00`–`0x64` | — | |
| `0x08` | Detection range, direction 3 | `0x00`–`0x64` | — | |
| `0x09` | Detection range, direction 4 | `0x00`–`0x64` | — | |
| `0x0A` | Sensitivity, direction 1 | `0x00`–`0x64` | — | Percent (0–100); per-direction override |
| `0x0B` | Sensitivity, direction 2 | `0x00`–`0x64` | — | |
| `0x0C` | Sensitivity, direction 3 | `0x00`–`0x64` | — | |
| `0x0D` | Sensitivity, direction 4 | `0x00`–`0x64` | — | |

**Global vs. per-direction:** Global offsets (`0x04`, `0x05`) use the full
`0x00`–`0xFF` raw byte scale. Per-direction offsets (`0x06`–`0x0D`) use percent
(`0x00`–`0x64` = 0–100 %). Mixing the scales is an easy mistake.

**Sensitivity vs. detection range:** *Sensitivity* (`0x04`) amplifies how strongly
the sensor responds to any motion signal. *Detection range* (`0x05`) sets the
threshold a signal must clear before it is classified as motion. For an HF sensor
that over-triggers through walls or into adjacent rooms, **lower the detection range
first** (`0x05`) — it rejects weak far-field returns without suppressing genuine
close-range events.

---

## Commands (dali_2k firmware)

Commands are entered via the **DALI Command** text entity in Home Assistant.
Results appear in **DALI Command Result**.

### Raw read (recommended)

Send the DTR setup lines before every `READ MEMORY LOCATION`. The read command
uses the current DTR1 bank and DTR0 offset, then auto-increments DTR0; a repeated
bare `raw 01FE3C len=24 wait` walks through later offsets.

```
raw C13102 len=24        # DTR1 = 0x02 (Bank 2)
raw C13004 len=24        # DTR0 = 0x04 (global sensitivity)
raw 01FE3C len=24 wait   # READ MEMORY LOCATION; 255 = 0xFF = factory max

raw C13102 len=24        # DTR1 = 0x02 (Bank 2)
raw C13005 len=24        # DTR0 = 0x05 (global detection range)
raw 01FE3C len=24 wait   # READ MEMORY LOCATION

raw C13102 len=24        # DTR1 = 0x02 (Bank 2)
raw C13006 len=24        # DTR0 = 0x06 (direction 1 range)
raw 01FE3C len=24 wait   # READ MEMORY LOCATION

raw C13102 len=24        # DTR1 = 0x02 (Bank 2)
raw C1300A len=24        # DTR0 = 0x0A (direction 1 sensitivity)
raw 01FE3C len=24 wait   # READ MEMORY LOCATION
```

### Raw write

Use `raw2` for ENABLE WRITE MEMORY because Home Assistant text entities can
ignore repeated identical text commands.

```
raw  C13102 len=24     # DTR1 = 0x02 (Bank 2)
raw  C13002 len=24     # DTR0 = 0x02 (lock byte)
raw2 01FE15 len=24     # ENABLE WRITE MEMORY x2
raw  C12155 len=24     # WRITE MEMORY LOCATION NO REPLY = 0x55 unlock

raw  C13004 len=24     # DTR0 = 0x04 (global sensitivity)
raw2 01FE15 len=24     # ENABLE WRITE MEMORY x2
raw  C12180 len=24     # WRITE MEMORY LOCATION NO REPLY = 0x80
```

For global detection range `0x80`, use the same sequence with `raw C13005 len=24`
in the second half. For direction 1 range `50` (`0x32`), use `raw C13006 len=24`
and `raw C12132 len=24`.

### Helper verbs

Firmware with the corrected helper verbs sends the same 24-bit Part 103 frames
as the raw examples above:

```
devmem read a0 2 4        # C13102, C13004, 01FE3C wait
dtrcheck a0 0 66          # C13042, 01FE36 wait
dtrcheck a0 1 2           # C13102, 01FE37 wait
devmem write a0 2 4 128   # unlock lock byte, then write Bank 2 offset 4 = 0x80
```

If an older firmware is still flashed, use the raw commands instead; the stale
helpers used 16-bit DTR/memory special frames and will not address the Steinel
control-device memory path correctly.

### Tuning workflow

1. Read current value before changing anything.
2. Write one parameter.
3. Wait ~10 s for the occupancy sensor poll to reflect any behavioural change.
4. Observe `Zone 2 Occupancy` over several minutes before tuning further.
5. Read back the written value to confirm NVM commit.

```
raw C13102 len=24        # Bank 2
raw C13005 len=24        # detection range offset
raw 01FE3C len=24 wait   # note current range value

# Write 0x80 using the raw write sequence above, then read back:
raw C13102 len=24
raw C13005 len=24
raw 01FE3C len=24 wait   # confirm 128 (0x80) was stored
```

### Verify occupancy state on demand

```
iquery a0 1 input-value   # 0=Unoccupied  85=Movement  170=Present  255=Present+Moving
```

### Verify Section 3 factory settings

Steinel section 3 lists optimized occupancy defaults: hold timer `1`, report
timer `5`, and event filter `7`. Corrected firmware maps the Part 303/type 3 `iquery`
timer helpers to the same opcodes as these raw commands.

```
raw 01012D len=24 wait   # Part 303/type 3 query hold timer; expected 1 = 10 seconds
raw 01012E len=24 wait   # Part 303/type 3 query report timer; expected 5 = 5 seconds
raw 010190 len=24 wait   # Part 103 query event filter bits 0-7; expected 7
```

Avoid `raw 010184 len=24 wait` for report timer: opcode `0x84` is Query Event
Priority, so a reply like `4` is plausible but answers a different question.

To restore the Steinel report timer to 5 seconds:

```
raw  C13005 len=24       # DTR0 = 5
raw2 010122 len=24       # Part 303/type 3 set report timer from DTR0; send twice
raw  01012E len=24 wait  # verify report timer; expected 5
```

---

## Wire-level Frame Sequence (for reference)

The raw write sequence has two phases:

**Sequence 1 — unlock Bank 2:**
| Frame | Type | Notes |
|-------|------|-------|
| `DTR1 = bank` (`C1 31 bank`) | 24-bit control-device special | Sets bank in control-device DTR1 |
| `DTR0 = 0x02` (`C1 30 02`) | 24-bit control-device special | Points at lock byte |
| `ENABLE WRITE MEMORY` (×2) | 24-bit device cmd `01 FE 15` | Addressed to Steinel only; lamp ignores 24-bit device frames |
| `WRITE MEMORY LOCATION NO REPLY = 0x55` (`C1 21 55`) | 24-bit control-device special | Only Steinel commits (it was enabled); DTR0 auto-increments |

**Sequence 2 — write value:**
| Frame | Type | Notes |
|-------|------|-------|
| `DTR0 = offset` (`C1 30 offset`) | 24-bit control-device special | Re-points at target offset |
| `ENABLE WRITE MEMORY` (×2) | 24-bit device cmd `01 FE 15` | Re-enable; write-enable lapses after any non-data frame |
| `WRITE MEMORY LOCATION NO REPLY = value` (`C1 21 value`) | 24-bit control-device special | Steinel writes value to NVM |

**Raw reads use the 24-bit device READ MEMORY LOCATION (`01 FE 3C`).**
Both the lamp and the Steinel share short address 0; the gear-addressed 16-bit
form (`01 C5`) would read from the lamp, not the Steinel.

The first 16 bits of the 24-bit frame (`01 FE`) look like gear address 0 +
command 0xFE to the lamp. Gear command 0xFE is the DT6 "QUERY MIN FAST FADE
TIME" which requires a prior ENABLE DEVICE TYPE 6 — the raw read sequence does
not send that, so the lamp stays silent. All replies come from the Steinel.

---

## Diagnosing DTR0/DTR1 problems

The raw memory path sets DTR0/DTR1 with Part 103 control-device special
commands (`C1 30 xx`, `C1 31 xx`) before the addressed 24-bit device READ or
WRITE. If these SET commands are not landing in the Steinel's DTR registers,
reads return data from wrong offsets and writes go to wrong locations.

Use raw DTR queries to verify before trusting any memory-read results:

```
raw C13042 len=24        # set DTR0 = 0x42
raw 01FE36 len=24 wait   # query DTR0; reply 66 (0x42) = SET works

raw C13102 len=24        # set DTR1 = 0x02 (Bank 2)
raw 01FE37 len=24 wait   # query DTR1; reply 2 = SET works
```

Use `raw` for raw 24-bit device queries:
```
raw 01FE30 len=24 wait   # 0x30 = QUERY DEVICE STATUS
raw 01FE35 len=24 wait   # 0x35 = QUERY NUMBER OF INSTANCES (should return 4)
raw 01FE36 len=24 wait   # 0x36 = QUERY CONTENT DTR0
raw 01FE37 len=24 wait   # 0x37 = QUERY CONTENT DTR1
```

## Caveats

- **DALI `RESET` wipes Bank 2.** Do not issue `config a0 reset` while tuning.
- **Allow ~1 s after writing before cutting bus power** — NVM commit is asynchronous.
- **Only one device should be write-enabled at a time.** `WRITE MEMORY LOCATION` is
  a broadcast; if multiple devices were enabled (e.g. after a broadcast
  `ENABLE WRITE MEMORY`), all of them would commit the write.
- **Bank 2, offset 0x00** is ROM. Its value is the last accessible address:
  `0x05` for base-model sensors (type < 100), `0x0D` for type 107 (HF 360 II
  DALI-2 IPD, which supports per-direction settings but not True Presence).
  Any other value from Bank 2 offset `0x00` indicates a DTR problem.
- **V1.5 Bank 2 map is definitive for type 107.** Offsets 0x13–0xFF are
  reserved (answer NO). Values outside the expected range from raw reads are
  a sign of wrong bank/offset due to DTR state issues, not extended NVM.

---

## References

1. **Steinel DALI-2 Interface Description V1.5**  
   Local copy: `_local/94546_DALI-2 Interface Description_V1.5.pdf`.
   This is the authoritative source for the Bank 2 offset map, lock byte
   protocol, type number 107 capabilities, scaling for global and per-direction
   sensitivity/range, and Bluetooth activation byte (Bank 3, address 0x03).

2. **Steinel HF 360-2 DALI-2 IPD Installation Manual**  
   Same source. Covers commissioning path (IEC 62386 Parts 101, 103, 303, 304)
   and distinguishes the IPD variant from the BT-IPD, COM1, and COM2 variants.

3. **IEC 62386-101:2014** — *Digital addressable lighting interface — Part 101:
   General requirements — System components*  
   Defines universal special commands (DTR0/1/2 SET, WRITE MEMORY LOCATION) that
   apply to both control gear and control devices.

4. **IEC 62386-103:2022** — *Digital addressable lighting interface — Part 103:
   General requirements — Control devices*  
   Defines the 24-bit device command frame format, ENABLE WRITE MEMORY (opcode
   `0x15`), READ MEMORY LOCATION (opcode `0x3C`), and the memory bank write gate
   (send-twice requirement).

5. **IEC 62386-303** — occupancy-sensor input-device requirements
   Defines instance type 3, its hold timer, deadtime, and the bit-replicated
   output encoding (0 / 85 / 170 / 255).

6. **IEC 62386-304** — light-sensor input-device requirements
   Defines instance type 4, used by Steinel instance 0 (lux).

7. **python-dali** (github.com/sde1000/python-dali)  
   Open-source Python implementation of IEC 62386. Used to verify command
   opcodes and frame encodings cited in this document, particularly
   `EnableWriteMemory` (send_twice), `WriteMemoryLocation`, and
   `ReadMemoryLocation` for the 103 control-device path.
