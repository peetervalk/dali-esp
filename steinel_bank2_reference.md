# Steinel HF 360-2 — Sensitivity and Range Tuning

Product: Steinel HF 360-2 DALI-2 IPD (art. 064280)
Reference installation, sensor at short address 0

**Last reviewed:** 2026-08-14

The HF 360-2 DALI-2 IPD is a **control device** (IEC 62386-103), not control
gear. Sensitivity and detection range cannot be set over Bluetooth — the app
variants (Steinel Connect, SmartRemote) target the COM1/COM2/BT-IPD family and
will not discover the 064280. Everything is done over the DALI bus.

Bank 2 is the Steinel-proprietary NVM bank holding those parameters. A standard
DALI `RESET` reverts all of it to factory defaults.

Commands below are shell verbs (`tools/dali-shell`, or the serial CLI). The
Home Assistant command console spells them identically — anything declaring a
`text:` platform and a `command_result:` sensor has the same verbs, including
`dali-starter.yaml`. Verb syntax is in `dali_commands.md`.

## Bank 2 Offset Map

Source: Steinel DALI-2 Interface Description V1.5.

| Offset | Parameter | Scale | Default | Notes |
|---|---|---|---|---|
| `0x02` | Lock byte | — | locked | `0x55` unlocks NVM writes; `devmem write` does this for you |
| `0x04` | Global sensitivity | `0x00`–`0xFF` | `0xFF` | Amplification of all motion signals, all directions |
| `0x05` | Global detection range | `0x00`–`0xFF` | `0xFF` | Minimum signal strength to register as motion; HF and US |
| `0x06`–`0x09` | Detection range, directions 1–4 | `0x00`–`0x64` | — | Percent 0–100; per-direction override |
| `0x0A`–`0x0D` | Sensitivity, directions 1–4 | `0x00`–`0x64` | — | Percent 0–100; per-direction override |

**The two scales differ.** Global offsets use the full `0x00`–`0xFF` byte;
per-direction offsets are percent (`0x00`–`0x64`). Mixing them is an easy
mistake.

**Sensitivity is not detection range.** Sensitivity (`0x04`) amplifies how
strongly the sensor responds to any motion signal. Detection range (`0x05`) sets
the threshold a signal must clear to be classified as motion. For an HF sensor
that triggers through walls or into adjacent rooms, **lower the detection range
first** — it rejects weak far-field returns without suppressing genuine
close-range events.

## Reading and Writing Bank 2

```text
devmem read 0 2 4              # read global sensitivity
devmem read 0 2 4 10           # read offsets 0x04..0x0D in one sequence
devmem write 0 2 4 128         # set global sensitivity to 0x80
devmem read 0 2 4              # confirm
```

`devmem` is the Part 103 control-device form — the one that reaches the Steinel.
`memread` is the Part 102 control-gear form and would address the lamp instead.
Both devices can share short address 0; they answer different frame widths.

`devmem write` writes the `0x55` unlock to the lock byte and then the requested
byte, as one queued sequence. It does not verify the commit, so the read back is
not optional. `devmem read` returns every byte of a multi-byte read in hex, and
holds the device's auto-incrementing cursor across the whole read.

## Tuning Workflow

1. Read the current value before changing anything.
2. Write one parameter.
3. Wait ~10 s for the occupancy poll to reflect any behavioural change.
4. Watch the occupancy entity over several minutes before tuning further.
5. Read the value back to confirm the NVM commit.

```text
devmem read 0 2 5              # current global detection range
devmem write 0 2 5 128
devmem read 0 2 5              # expect 128 (0x80)
```

Per-direction range 1 to 50 %:

```text
devmem read 0 2 6
devmem write 0 2 6 50
devmem read 0 2 6
```

### Check occupancy state on demand

```text
iquery 0 1 input-value    # 0 unoccupied, 85 movement, 170 present, 255 both
```

### Verify the Part 303 timers

Steinel's optimized occupancy defaults are hold timer `1`, report timer `5`, and
event filter `7`:

```text
iquery 0 1 occ-hold-timer      # expect 1 (10 seconds)
iquery 0 1 occ-report-timer    # expect 5 (5 seconds)
iquery 0 1 event-filter0       # expect 7
```

Restore the report timer to 5 seconds:

```text
iconfig 0 1 occ-set-report-timer 5
iquery 0 1 occ-report-timer
```

### Convert a raw reading

`vendor steinel` applies the sensor platform's own conversion to a value you
already have, without touching the bus:

```text
vendor steinel 2 262     ->  temperature: 21.2 C
vendor steinel 3 110     ->  humidity: 55.0 %
```

Instances are `0` lux (scale 0.01), `1` motion, `2` temperature, `3` humidity.

## Diagnosing DTR Problems

The memory path sets DTR0/DTR1 with Part 103 control-device specials before the
addressed read or write. If those are not landing, reads return data from the
wrong offsets and writes go to the wrong locations. Confirm the registers take a
value before trusting any result:

```text
dtrcheck 0 0 66           # expect: read 66 (0x42)
dtrcheck 0 1 2            # expect: read 2
```

`dtrcheck` loads and reads back in one sequence, which separates "the DTR never
took the value" from "the command that consumes it was ignored".

Bank 2 offset `0x00` is ROM and reports the last accessible address: `0x05` for
base-model sensors (type < 100), `0x0D` for type 107 (HF 360 II DALI-2 IPD, which
supports per-direction settings but not True Presence). **Any other value from
offset `0x00` means a DTR problem**, not extended NVM:

```text
devmem read 0 2 0         # expect 13 (0x0D) on type 107
```

Other useful device-level reads:

```text
instances 0               # instance types this device offers (expect 4)
iquery 0 1 status
iquery 0 0 type           # instance 0 should report type 4 (light sensor)
```

## Caveats

- **DALI `RESET` wipes Bank 2.** Do not issue `config a0 reset` while tuning.
- **Allow ~1 s after a write before cutting bus power** — the NVM commit is
  asynchronous.
- **Only one device should be write-enabled at a time.** WRITE MEMORY LOCATION is
  a broadcast; every device that was write-enabled commits it. `devmem write`
  addresses the enable to one device, but a broadcast ENABLE WRITE MEMORY issued
  by another tool leaves that guarantee behind.
- **Offsets `0x13`–`0xFF` are reserved** and answer NO. Values outside the
  expected range are a sign of a wrong bank or offset, not of extended NVM.
- No write is verified by the firmware. The read-back is the verification.

## Appendix — Raw Frame Equivalents

The helper verbs send exactly these frames. They are worth having for when a helper
is suspect.

Read: set DTR1 to the bank, DTR0 to the offset, then READ MEMORY LOCATION, which
auto-increments DTR0 — a repeated bare read walks later offsets.

```text
raw C13102 len=24        # DTR1 = 0x02 (Bank 2)
raw C13004 len=24        # DTR0 = 0x04 (global sensitivity)
raw 01FE3C len=24 wait   # READ MEMORY LOCATION
```

Write: unlock, then write. `raw2` is required for ENABLE WRITE MEMORY — it must
be sent twice inside the DALI window, which two typed lines cannot achieve, and
Home Assistant text entities ignore a repeated identical state anyway.

```text
raw  C13102 len=24     # DTR1 = 0x02 (Bank 2)
raw  C13002 len=24     # DTR0 = 0x02 (lock byte)
raw2 01FE15 len=24     # ENABLE WRITE MEMORY, twice
raw  C12155 len=24     # WRITE MEMORY LOCATION NO REPLY = 0x55 unlock

raw  C13004 len=24     # DTR0 = 0x04 (global sensitivity)
raw2 01FE15 len=24     # re-enable; write-enable lapses after any non-data frame
raw  C12180 len=24     # WRITE MEMORY LOCATION NO REPLY = 0x80
```

Device-level queries:

```text
raw 01FE30 len=24 wait   # QUERY DEVICE STATUS
raw 01FE35 len=24 wait   # QUERY NUMBER OF INSTANCES (expect 4)
raw 01FE36 len=24 wait   # QUERY CONTENT DTR0
raw 01FE37 len=24 wait   # QUERY CONTENT DTR1
```

A lamp with a same short address stays silent: the first 16 bits of `01 FE …`
look like gear address 0 plus command `0xFE` to the lamp, which is the DT6 QUERY
MIN FAST FADE TIME — and that requires a preceding ENABLE DEVICE TYPE 6 that the
sequence never sends. Every reply comes from the Steinel. The gear-addressed
16-bit read form (`01 C5`) would read the lamp instead.

## References

1. **Steinel DALI-2 Interface Description V1.5** — local copy at
   `_local/94546_DALI-2 Interface Description_V1.5.pdf`. Authoritative for the
   Bank 2 offset map, the lock-byte protocol, type 107 capabilities, the scaling
   of global and per-direction values, and the Bluetooth activation byte
   (Bank 3, offset `0x03`).
2. **Steinel HF 360-2 DALI-2 IPD installation manual** — same source; covers the
   commissioning path (IEC 62386 Parts 101, 103, 303, 304) and distinguishes the
   IPD variant from BT-IPD, COM1, and COM2.
3. **IEC 62386-101:2014** — universal special commands (DTR0/1/2 SET, WRITE
   MEMORY LOCATION) for both control gear and control devices.
4. **IEC 62386-103:2022** — 24-bit device command frames, ENABLE WRITE MEMORY
   (`0x15`), READ MEMORY LOCATION (`0x3C`), and the send-twice write gate.
5. **IEC 62386-303** — instance type 3, its hold timer and deadtime, and the
   bit-replicated output encoding (0 / 85 / 170 / 255).
6. **IEC 62386-304** — instance type 4, used by Steinel instance 0 (lux).
7. **python-dali** (github.com/sde1000/python-dali) — used to verify the opcodes
   and frame encodings cited here, particularly `EnableWriteMemory` (send_twice),
   `WriteMemoryLocation`, and `ReadMemoryLocation` on the Part 103 path.
