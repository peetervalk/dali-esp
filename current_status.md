# DALI-ESP Project — Current Status

**Last updated:** 2026-06-04  
**Framework:** ESP-IDF v5.x (native CMake)  
**Hardware:** ESP32-DevKitC-32E + MikroE DALI-2 Click (GPIO interface)  
**Timer:** GPTIMER, 104 µs alarm (4× oversampling of 416.7 µs half-bit)

---

## Current Status: Phase 1 complete — awaiting toolchain install

The full project scaffold has been created. No build tools are installed yet.
The next concrete action is installing ESP-IDF (see Phase 0 below).

---

## To-Do List

### Phase 0 — Toolchain Setup ❌ NOT DONE
- [ ] Install **ESP-IDF v5.3+** (Windows offline installer: https://dl.espressif.com/dl/esp-idf/)
- [ ] Install **ESP-IDF VS Code extension** (`espressif.vscode-esp-idf-tools`) and run setup wizard
- [ ] Verify: `idf.py --version` returns 5.x
- [ ] Flash a hello_world project to the ESP32-DevKitC-32E to confirm toolchain works
- [ ] Install **MinGW-w64** (or use WSL) for host unit tests (`gcc`, `cmake`)
  - Recommended: https://www.mingw-w64.org/ or via `winget install mingw`

### Phase 1 — Project Scaffold ✅ DONE
- [x] `CMakeLists.txt` (ESP-IDF top-level project)
- [x] `main/main.c` + `main/CMakeLists.txt`
- [x] `components/dali/CMakeLists.txt`
- [x] `components/dali/dali_frame.h` — shared types, error enum, timing constants
- [x] `components/dali/dali_ringbuf.h/.c` — lock-free SPSC ring buffer (host-portable)
- [x] `components/dali/dali_phy.h/.c` — PHY layer skeleton with GPTIMER ISR and Manchester encoder
- [x] `components/dali/dali_scheduler.h/.c` — stub (Phase 8)
- [x] `components/dali/dali_protocol.h/.c` — 16-bit frame builders + response parser
- [x] `components/dali/dali_diag.h/.c` — serial CLI task (stats, trace, reset, raw)
- [x] `esphome/dali_esphome.h` — stub
- [x] `test/CMakeLists.txt` — standalone host build, no ESP-IDF
- [x] `test/unity/` — Unity test framework vendored
- [x] `test/test_ringbuf.c`
- [x] `test/test_phy_encode.c`
- [x] `test/test_protocol.c`

### Phase 2 — Verify Build ❌ NOT DONE
- [ ] `idf.py build` succeeds with all stubs (requires Phase 0)
- [ ] `idf.py flash monitor` runs on device (requires Phase 0)
- [ ] Host tests build and pass:
  ```
  cd test
  cmake -B build -G "MinGW Makefiles"
  cmake --build build
  ctest --output-on-failure
  ```

### Phase 3 — PHY TX State Machine (implementation) ❌ NOT DONE
- [ ] Verify GPTIMER alarm fires at 104 µs — toggle debug GPIO, measure with logic analyzer
- [ ] Confirm `dali_phy_tx()` drives DALI_TX_GPIO with correct Manchester waveform
- [ ] Verify half-bit period = 416.7 µs ±25% on logic analyzer
- [ ] Set correct GPIO numbers for your mikroBUS adapter wiring in `main/main.c`:
  ```c
  #define DALI_TX_GPIO  18   // ← Update to match your wiring
  #define DALI_RX_GPIO  19   // ← Update to match your wiring
  ```

### Phase 4 — PHY RX Edge Capture + Manchester Decode ❌ NOT DONE
- [ ] Implement `dali_phy_rx_process()` — full Manchester decode from edge timestamps
- [ ] Unit-test decode with known interval sequences in `test/test_phy_encode.c`
- [ ] `dali_phy_decode_manchester()` currently returns `DALI_ERR_MALFORMED` (stub)

### Phase 5 — PHY Loopback Test ❌ NOT DONE
- [ ] Wire `DALI_TX_GPIO` → `DALI_RX_GPIO` physically
- [ ] Transmit `0x0080` (16-bit DAPC), verify received `DaliFrame` matches
- [ ] Run `raw 0x0080 len=16` via serial CLI, check `stats` shows zero errors

### Phase 6 — Diagnostic CLI Completion ❌ NOT DONE (partial)
- [ ] `stats`, `trace on/off`, `reset`, `raw` — implemented (needs build verification)
- [ ] `scan` — deferred until Scheduler (Phase 8)
- [ ] `query <addr>` — deferred until Scheduler (Phase 8)

### Phase 7 — Scheduler ❌ NOT DONE (stub only)
- [ ] Implement FreeRTOS transaction queue (`dali_scheduler.c`)
- [ ] State machine: IDLE → TX → WAIT_SETTLE → WAIT_REPLY → DONE
- [ ] Send-twice enforcement (100 ms window)
- [ ] Reply timeout (20 ms, TBD from IEC 62386) and retry (up to `DALI_MAX_RETRIES`)
- [ ] Unit tests in `test/test_scheduler.c` (file not yet created)

### Phase 8 — Protocol Layer (full) ❌ NOT DONE (partial)
- [ ] 16-bit frame builders ✅ implemented
- [ ] 24-bit DALI-2 instance command builder ❌
- [ ] Full response parsing (8-bit and 16-bit responses) ❌
- [ ] Device discovery (`scan` command via broadcast QUERY STATUS)

### Phase 9 — ESPHome Integration ❌ NOT DONE (stub only)
- [ ] Implement only after loopback and real-device tests pass
- [ ] Decide: custom component vs external component (open question)

---

## Open Questions

| Question | Status |
|---|---|
| Which GPIO numbers for TX/RX on your mikroBUS adapter? | **Needs your input** — update `main/main.c` |
| Exact TX-to-RX turnaround time (IEC 62386 part 1)? | Open — placeholder 7 ms |
| Exact reply timeout (IEC 62386 part 1)? | Open — placeholder 20 ms |
| Maximum retry count before marking device offline? | Open — placeholder 3 |
| DALI-2 instance discovery: auto or manual? | Open |
| ESPHome component type: custom or external? | Open |
| Host tests on Linux/WSL or MinGW on Windows? | Open |

---

## Architecture

```
ESPHome (stub — Phase 9)
↓
DALI Integration Layer (stub — Phase 9)
↓
DaliProtocol  ← dali_protocol.h/.c  [16-bit frames ✅, 24-bit ❌]
↓
DaliScheduler ← dali_scheduler.h/.c [stub ❌]
↓
DaliPhy       ← dali_phy.h/.c       [TX state machine ✅, RX decode ❌]
↓
DaliRingBuf   ← dali_ringbuf.h/.c   [SPSC ring buffer ✅]
↓
DALI-2 Click  (GPIO: optocouplers — pure level shifter, no onboard MCU)
↓
DALI Bus
```

---

## Key Timing Facts (IEC 62386)

| Parameter | Value | Notes |
|---|---|---|
| Data rate | 1200 bps | Manchester encoded |
| Bit period | 833.3 µs | 1 / 1200 |
| Half-bit period | 416.7 µs | Manchester encoding unit |
| Timer tick | 104 µs | 4× oversampling |
| TX-to-RX settle | ~7 ms | TBD from IEC 62386 §8 |
| Reply timeout | ~20 ms | TBD from IEC 62386 §8 |
| Send-twice window | 100 ms | Configuration commands |
| Bus idle (min) | 2× stop bits | ~1.67 ms |

---

## Build Commands (once ESP-IDF is installed)

```bash
# On-device build and flash
idf.py build
idf.py -p COM<N> flash monitor

# Host unit tests (MinGW or WSL)
cd test
cmake -B build -G "MinGW Makefiles"   # or "Unix Makefiles" on WSL
cmake --build build
ctest --output-on-failure
```
