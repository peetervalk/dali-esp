# DALI-ESP Project — Current Status

**Last updated:** 2026-06-05 (rev 3)  
**Framework:** ESP-IDF v6.0.1 (native CMake)  
**Hardware:** ESP32-DevKitC-VE (ESP32-WROVER-E) + MikroE DALI-2 Click (GPIO interface)  
**Timer:** GPTIMER, 104 µs alarm (4× oversampling of 416.7 µs half-bit)

---

## Current Status: All software complete — hardware verification remaining

Every software layer is implemented and host-tested. All 5 test suites pass (51 total tests). `idf.py build` passes. Remaining work requires physical hardware (flash, loopback, real DALI bus).

**What was implemented in this session:**
- `dali_phy_rx_process()` — full ring-buffer drain + interval accumulation + Manchester decode + idle detection
- `dali_diag.c` — `scan` (queries all 64 addresses via scheduler) and `query <addr>` (parses QUERY STATUS response)
- `main.c` — dedicated DALI processing task (priority 10, Core 1) driving `dali_phy_rx_process()` + `dali_sched_run()` at 1 ms intervals
- `cmd_reset` now calls `dali_sched_reset()` before `dali_phy_reset()`

---

## To-Do List

### Phase 0 — Toolchain Setup ✅ DONE
- [x] ESP-IDF v6.0.1 installed at `C:\esp\v6.0.1\esp-idf`; tools at `C:\Espressif\tools`
- [x] xtensa-esp-elf 15.2.0 toolchain present
- [x] **ESP-IDF VS Code extension** (`espressif.esp-idf-extension`) installed and configured
- [x] Setup wizard run: `idf.hasWalkthroughBeenShown: true`, workspace `idf.currentSetup` set
- [x] `idf.py --version` returns `ESP-IDF v6.0.1` (activate via `. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"`)
- [ ] Flash a hello_world project to the ESP32-DevKitC-VE to confirm toolchain works (**requires hardware**)

> Note: Activate ESP-IDF environment in each new PowerShell terminal by running:
> `. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"`
> cmake/ctest: `C:\Espressif\tools\cmake\4.0.3\bin`

### Phase 1 — Project Scaffold ✅ DONE
- [x] `CMakeLists.txt` (ESP-IDF top-level project)
- [x] `main/main.c` + `main/CMakeLists.txt`
- [x] `components/dali/CMakeLists.txt`
- [x] `components/dali/dali_frame.h` — shared types, error enum, timing constants
- [x] `components/dali/dali_ringbuf.h/.c` — lock-free SPSC ring buffer (host-portable)
- [x] `components/dali/dali_phy.h/.c` — PHY layer skeleton with GPTIMER ISR and Manchester encoder
- [x] `components/dali/dali_scheduler.h/.c` — fully implemented (DaliSchedOps injection, state machine, send-twice, retry, 8 tests)
- [x] `components/dali/dali_protocol.h/.c` — 16-bit + 24-bit builders, DaliStatus parsing, 24 tests
- [x] `components/dali/dali_diag.h/.c` — serial CLI: stats, trace on/off, reset, raw, scan, query
- [x] `esphome/dali_esphome.h` — stub
- [x] `test/CMakeLists.txt` — standalone host build, no ESP-IDF
- [x] `test/unity/` — Unity test framework vendored
- [x] `test/test_ringbuf.c`
- [x] `test/test_phy_encode.c`
- [x] `test/test_protocol.c`
- [x] `test/test_phy_decode.c`
- [x] `test/test_scheduler.c`

### Phase 2 — Verify Build ✅ PARTIAL
- [x] `idf.py build` succeeds — `dali_esp.bin` 0x27e70 bytes, 84% partition free
  - Fixed: ESP-IDF v6 split `driver` into sub-components; added `esp_driver_gpio`, `esp_driver_gptimer`, `esp_driver_uart` to `PRIV_REQUIRES` in `components/dali/CMakeLists.txt`
- [ ] `idf.py flash monitor` runs on device (**requires hardware**)
- [x] Host tests build and pass (5/5): `test_ringbuf`, `test_phy_encode`, `test_protocol`, `test_phy_decode`, `test_scheduler` (51 total tests)

### Phase 3 — PHY TX State Machine (implementation) ❌ NOT DONE
- [ ] Verify GPTIMER alarm fires at 104 µs — toggle debug GPIO, measure with logic analyzer
- [ ] Confirm `dali_phy_tx()` drives DALI_TX_GPIO with correct Manchester waveform
- [ ] Verify half-bit period = 416.7 µs ±25% on logic analyzer
- [ ] Set correct GPIO numbers for your mikroBUS adapter wiring in `main/main.c`:
  ```c
  #define DALI_TX_GPIO  18   // ← Update to match your wiring
  #define DALI_RX_GPIO  19   // ← Update to match your wiring
  ```
  > **WROVER-E constraint:** GPIO 16 and GPIO 17 are connected to the on-module PSRAM and must NOT be used. Current placeholder values (18, 19) are safe.

### Phase 4 — PHY RX Edge Capture + Manchester Decode ✅ DONE
- [x] `dali_phy_decode_manchester()` fully implemented (interval-based state machine, ±25% tolerance)
- [x] 22 host unit tests in `test/test_phy_decode.c` covering 8-bit, 16-bit, 24-bit round-trips and error cases
- [x] `dali_phy_rx_process()` — implemented: drains ring buffer, accumulates edge intervals, detects frame end via 1-bit-period bus idle, calls `dali_phy_decode_manchester()`, fires RX callback (**software complete; needs hardware to verify timing**)

### Phase 5 — PHY Loopback Test ❌ NOT DONE
- [ ] Wire `DALI_TX_GPIO` → `DALI_RX_GPIO` physically
- [ ] Transmit `0x0080` (16-bit DAPC), verify received `DaliFrame` matches
- [ ] Run `raw 0x0080 len=16` via serial CLI, check `stats` shows zero errors

### Phase 6 — Diagnostic CLI ✅ DONE (needs hardware verification)
- [x] `stats` — prints all `dali_stats_t` counters
- [x] `trace on/off` — enables/disables per-frame bus trace logging
- [x] `reset` — resets both scheduler (`dali_sched_reset()`) and PHY (`dali_phy_reset()`)
- [x] `raw <hex> len=<n>` — transmits arbitrary frame directly via PHY
- [x] `scan` — queries all 64 short addresses via scheduler; prints present devices
- [x] `query <addr>` — queries QUERY STATUS via scheduler; decodes and prints all status fields
- [x] FreeRTOS task notification pattern: CLI blocks on `ulTaskNotifyTake()`, DALI task drives scheduler, callback fires `xTaskNotifyGive()`

### Phase 7 — Scheduler ✅ DONE
- [x] Implemented FreeRTOS transaction queue (`dali_scheduler.c`)
- [x] State machine: IDLE → TX → WAIT_SETTLE → WAIT_REPLY → IDLE
- [x] Send-twice enforcement (two TXs separated by settle period)
- [x] Reply timeout (`DALI_REPLY_TIMEOUT_MS`) and retry (`retries_left` budget)
- [x] `DaliSchedOps` injection for host testability (mock PHY + controllable tick)
- [x] `dali_sched_init_device()` convenience initialiser for on-device use (ESP timer ms)
- [x] 8 unit tests in `test/test_scheduler.c` — all pass
  - simple send, send-twice, reply received, timeout→retry, retry exhausted, queue full, reset, multi-transaction order

### Phase 8 — Protocol Layer (full) ✅ DONE
- [x] 16-bit frame builders (DAPC, recall max/min, off, query status, broadcast)
- [x] 24-bit DALI-2 instance command builders: `dali_cmd_instance()`, `dali_cmd_instance_group()`, `dali_cmd_instance_broadcast()`
- [x] `DaliStatus` struct and `dali_parse_status()` — QUERY STATUS byte field parsing
- [x] `dali_is_yes()` — YES/NO backward frame helper
- [x] 24 unit tests in `test/test_protocol.c` — all pass

### Phase 9 — ESPHome Integration ❌ NOT DONE (stub only)
- [ ] Implement only after loopback and real-device tests pass
- [ ] Decide: custom component vs external component (open question)

---

## Open Questions

| Question | Status |
|---|---|
| Which GPIO numbers for TX/RX on your mikroBUS adapter? | **Needs your input** — update `main/main.c`; GPIO 16/17 are reserved for PSRAM on WROVER-E, must not be used |
| Exact TX-to-RX turnaround time (IEC 62386 part 1)? | **Resolved** — 7 ms confirmed per IEC 62386-101 §8 |
| Exact reply timeout (IEC 62386 part 1)? | **Resolved** — 25 ms (22 ms spec max + 3 ms margin) |
| Maximum retry count before marking device offline? | **Decided** — 3 retries (tune on real hardware) |
| DALI-2 instance discovery: auto or manual? | **Decided** — manual; auto-discovery deferred to post-loopback |
| ESPHome component type: custom or external? | **Decided** — in-tree custom component now; migrate to external after Phase 9 works |
| Host tests on Linux/WSL or MinGW on Windows? | **Decided** — MinGW on Windows |

---

## TODOs (require compiler / hardware access — do not edit source files until available)

- [ ] **sdkconfig — flash size:** After first `idf.py build` or via `idf.py menuconfig`, set `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y` (WROVER-E ships with 8 MB Flash; default ESP-IDF config targets 2 MB).
- [ ] **sdkconfig — PSRAM (optional):** If PSRAM is ever needed, enable `CONFIG_SPIRAM=y` (formerly `CONFIG_ESP32_SPIRAM_SUPPORT`) in menuconfig. Not required for DALI operation.
- [ ] **GPIO assignment:** Confirm mikroBUS header wiring on DevKitC-VE and update `DALI_TX_GPIO` / `DALI_RX_GPIO` in `main/main.c`. Avoid GPIO 16 and 17 (PSRAM).

---

## Architecture

```
ESPHome (stub — Phase 9)
↓
DALI Integration Layer (stub — Phase 9)
↓
DaliProtocol  ← dali_protocol.h/.c  [✅ fully implemented, 24 tests pass]
↓
DaliScheduler ← dali_scheduler.h/.c [✅ implemented, 8 tests pass]
↓
DaliPhy       ← dali_phy.h/.c       [TX state machine ✅, RX decode ✅ (software complete)]
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
| TX-to-RX settle | 7 ms | Confirmed per IEC 62386-101 §8 |
| Reply timeout | 25 ms | 22 ms spec max + 3 ms margin (IEC 62386-101 §8) |
| Send-twice window | 100 ms | Configuration commands |
| Bus idle (min) | 2× stop bits | ~1.67 ms |

```
app_main
  ├─ dali_phy_init()                         # GPTimer + GPIO ISR setup
  ├─ dali_sched_init_device()               # Wires ops to PHY, starts scheduler
  ├─ xTaskCreatePinnedToCore(dali_task, 10, Core 1)
  │     └─ loop: dali_phy_rx_process()         # Drain ring buffer → decode
  │           dali_sched_run()              # Advance scheduler state machine
  │           vTaskDelay(1 ms)
  └─ dali_diag_init()                        # Starts CLI task (priority 2)
```

---

## Build Commands (once ESP-IDF is installed)
# On-device build and flash
idf.py build
idf.py -p COM<N> flash monitor

# Host unit tests (MinGW or WSL)
cd test
cmake -B build -G "MinGW Makefiles"   # or "Unix Makefiles" on WSL
cmake --build build
ctest --output-on-failure
```
