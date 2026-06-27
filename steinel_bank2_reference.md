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

Source: Steinel DALI-2 Interface Description V1.5 (see References).

| Offset | Parameter | Scale | Default | Notes |
|--------|-----------|-------|---------|-------|
| `0x02` | Lock byte | — | locked | Write `0x55` to unlock NVM writes; `memwrite` does this automatically |
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

```
raw C302 len=16          # DTR1 = 0x02 (Bank 2)
raw A304 len=16          # DTR0 = 0x04 (global sensitivity)
raw 01FE3C len=24 wait   # READ MEMORY LOCATION; 255 = 0xFF = factory max

raw C302 len=16          # DTR1 = 0x02 (Bank 2)
raw A305 len=16          # DTR0 = 0x05 (global detection range)
raw 01FE3C len=24 wait   # READ MEMORY LOCATION

raw C302 len=16          # DTR1 = 0x02 (Bank 2)
raw A306 len=16          # DTR0 = 0x06 (direction 1 range)
raw 01FE3C len=24 wait   # READ MEMORY LOCATION

raw C302 len=16          # DTR1 = 0x02 (Bank 2)
raw A30A len=16          # DTR0 = 0x0A (direction 1 sensitivity)
raw 01FE3C len=24 wait   # READ MEMORY LOCATION
```

### Raw write

Use `raw2` for ENABLE WRITE MEMORY because Home Assistant text entities can
ignore repeated identical text commands.

```
raw  C302 len=16       # DTR1 = 0x02 (Bank 2)
raw  A302 len=16       # DTR0 = 0x02 (lock byte)
raw2 01FE15 len=24     # ENABLE WRITE MEMORY x2
raw  C955 len=16       # WRITE MEMORY LOCATION NO REPLY = 0x55 unlock

raw  A304 len=16       # DTR0 = 0x04 (global sensitivity)
raw2 01FE15 len=24     # ENABLE WRITE MEMORY x2
raw  C980 len=16       # WRITE MEMORY LOCATION NO REPLY = 0x80
```

For global detection range `0x80`, use the same sequence with `raw A305 len=16`
in the second half. For direction 1 range `50` (`0x32`), use `raw A306 len=16`
and `raw C932 len=16`.

### Helper equivalents

The helper verbs are intended to produce the same frame sequences, but use the
raw commands above if helper output looks wrong.

`memwrite` handles the unlock sequence automatically (DTR1=bank, DTR0=lock byte,
ENABLE WRITE MEMORY ×2, WRITE 0x55, re-point DTR0, ENABLE WRITE MEMORY ×2, WRITE value).

```
memwrite a0 2 4 128   # global sensitivity   ~50% (0x80)
memwrite a0 2 5 128   # global detection range ~50%
memwrite a0 2 6 50    # direction 1 range = 50 % (0x32)
```

### Tuning workflow

1. Read current value before changing anything.
2. Write one parameter.
3. Wait ~10 s for the occupancy sensor poll to reflect any behavioural change.
4. Observe `Zone 2 Occupancy` over several minutes before tuning further.
5. Read back the written value to confirm NVM commit.

```
raw C302 len=16          # Bank 2
raw A305 len=16          # detection range offset
raw 01FE3C len=24 wait   # note current range value

# Write 0x80 using the raw write sequence above, then read back:
raw C302 len=16
raw A305 len=16
raw 01FE3C len=24 wait   # confirm 128 (0x80) was stored
```

### Verify occupancy state on demand

```
iquery a0:1 input-value   # 0=Unoccupied  85=Movement  170=Present  255=Present+Moving
```

---

## Wire-level Frame Sequence (for reference)

The `memwrite` command translates to two scheduler sequences:

**Sequence 1 — unlock Bank 2:**
| Frame | Type | Notes |
|-------|------|-------|
| `DTR1 = bank` | 16-bit broadcast special | Sets bank in all devices |
| `DTR0 = 0x02` | 16-bit broadcast special | Points at lock byte |
| `ENABLE WRITE MEMORY` (×2) | 24-bit device cmd `01 FE 15` | Addressed to Steinel only; lamp ignores 24-bit device frames |
| `WRITE MEMORY LOCATION NO REPLY = 0x55` | 16-bit broadcast special | Only Steinel commits (it was enabled); DTR0 auto-increments |

**Sequence 2 — write value:**
| Frame | Type | Notes |
|-------|------|-------|
| `DTR0 = offset` | 16-bit broadcast special | Re-points at target offset |
| `ENABLE WRITE MEMORY` (×2) | 24-bit device cmd `01 FE 15` | Re-enable; write-enable lapses after any non-data frame |
| `WRITE MEMORY LOCATION NO REPLY = value` | 16-bit broadcast special | Steinel writes value to NVM |

**`memread` uses the 24-bit device READ MEMORY LOCATION (`01 FE 3C`).**
Both the lamp and the Steinel share short address 0; the gear-addressed 16-bit
form (`01 C5`) would read from the lamp, not the Steinel.

The first 16 bits of the 24-bit frame (`01 FE`) look like gear address 0 +
command 0xFE to the lamp. Gear command 0xFE is the DT6 "QUERY MIN FAST FADE
TIME" which requires a prior ENABLE DEVICE TYPE 6 — the memread sequence does
not send that, so the lamp stays silent. All replies come from the Steinel.

---

## Diagnosing DTR0/DTR1 problems

`memread` and `memwrite` both work by broadcasting SET DTR0/DTR1 (Part-101
universal commands) before the 24-bit device READ or WRITE. If these SET
commands are not landing in the Steinel's DTR registers, reads return data from
wrong offsets and writes go to wrong locations.

Use `dtrcheck` to verify before trusting any memread results:

```
dtrcheck a0 0 66    # set DTR0 = 0x42, query it back; reply 66 = SET works
dtrcheck a0 1 2     # set DTR1 = 0x02 (bank 2), query it back; reply 2 = SET works
```

Use `devquery` for raw 24-bit queries:
```
devquery a0 48      # 0x30 = QUERY CONTENT DTR0 (current value in Steinel)
devquery a0 49      # 0x31 = QUERY CONTENT DTR1
devquery a0 53      # 0x35 = QUERY NUMBER OF INSTANCES (should return 4)
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
  Any other value from a `memread a0 2 0` indicates a DTR problem.
- **V1.5 Bank 2 map is definitive for type 107.** Offsets 0x13–0xFF are
  reserved (answer NO). Values outside the expected range from `memread` are
  a sign of wrong bank/offset due to DTR state issues, not extended NVM.

---

## References

1. **Steinel DALI-2 Interface Description V1.5**  
   Available from steinel.de (product page for HF 360-2 DALI-2 IPD, art. 064280,
   under Downloads / Technical Documents). This is the authoritative source for
   the Bank 2 offset map, lock byte protocol, and Bluetooth activation byte
   (Bank 3, address 0x03).

2. **Steinel HF 360-2 DALI-2 IPD Installation Manual**  
   Same source. Covers commissioning path (IEC 62386 Parts 101, 103, 303, 304)
   and distinguishes the IPD variant from the BT-IPD, COM1, and COM2 variants.

3. **IEC 62386-101:2014** — *Digital addressable lighting interface — Part 101:
   General requirements — System components*  
   Defines universal special commands (DTR0/1/2 SET, WRITE MEMORY LOCATION) that
   apply to both control gear and control devices.

4. **IEC 62386-103:2014** — *Digital addressable lighting interface — Part 103:
   General requirements — Control devices*  
   Defines the 24-bit device command frame format, ENABLE WRITE MEMORY (opcode
   `0x15`), READ MEMORY LOCATION (opcode `0x3C`), and the memory bank write gate
   (send-twice requirement).

5. **IEC 62386-303** — *Occupancy sensor* device type  
   Defines the DT303 instance type, hold timer, deadtime, and the bit-replicated
   output encoding (0 / 85 / 170 / 255).

6. **IEC 62386-304** — *Light sensor* device type  
   Defines the DT304 instance type used by Steinel instance 0 (lux).

7. **python-dali** (github.com/sde1000/python-dali)  
   Open-source Python implementation of IEC 62386. Used to verify command
   opcodes and frame encodings cited in this document, particularly
   `EnableWriteMemory` (send_twice), `WriteMemoryLocation`, and
   `ReadMemoryLocation` for the 103 control-device path.
