# Project Context: ESP32 DALI-2 Gateway for ESPHome/Home Assistant

## Project Goal

Build a DALI-2 master/controller using an ESP32 and MikroE DALI-2 Click board.

The project is primarily a learning and development project, not a commercial product.

The long-term goal is:

* Communicate with DALI and DALI-2 devices
* Support DALI-2 sensors such as presence detectors
* Support DALI-2 instances and extended frame formats
* Integrate with ESPHome and Home Assistant
* Maintain access to low-level protocol functionality for debugging and experimentation

---

## For AI Coding Agents

* Use `current_status.md` as the active project status and roadmap; it is the most reliable source for what is implemented vs pending.
* Preserve the existing architecture: `components/dali` is the protocol/PHY stack, `main/main.c` is the device entry point, and `esphome/dali_esphome.h` is a stub integration layer.
* Avoid making ESPHome integration changes until the core PHY/Protocol/Scheduler stack is verified working.
* Use `test/CMakeLists.txt` and the Unity tests as the canonical host test harness for portable DALI logic.
* Do not assume 16-bit-only DALI frames; the architecture must support 16-bit and 24-bit DALI-2 frames.

---

## Hardware

### MCU

ESP32-DevKitC-VE (ESP32-WROVER-E)

**Key ESP32 characteristics relevant to this project:**

* Dual-core Xtensa LX6, 240 MHz
* 520 KB SRAM total
* 8 MB Flash (WROVER-E default)
* 8 MB SPI PSRAM (on-module; not required for DALI operation but available)
* **GPIO 16 and GPIO 17 are connected to the on-module PSRAM and must NOT be used for any application I/O**
* FreeRTOS-based; ISR-safe APIs are distinct from task-level APIs
* ISR functions must be placed in IRAM using `IRAM_ATTR` to avoid cache-miss latency spikes
* Watchdog timer must not be starved; avoid long or blocking ISR execution
* Consider pinning the DALI task to one core if real-time jitter is a concern

### DALI Interface

MikroE DALI-2 Click  
(https://www.mikroe.com/dali-2-click)

**Important timing constraint:**

* Uses a timer interrupt with approximately 104 μs period
* `dali2_isr()` must complete in under 104 μs — this is the hardest real-time constraint in the system
* All functions called from the ISR must be tagged `IRAM_ATTR`

**Bus characteristics:**

* DALI is half-duplex; TX and RX cannot occur simultaneously
* Bus idle detection is required before transmission
* After TX completes, the bus must be released and RX listening resumed
* Settling time between TX and RX must be respected per IEC 62386

---

## Real-Time Constraints

| Constraint | Value | Notes |
|---|---|---|
| Timer ISR period | ~104 μs | Hard upper bound on ISR execution time; 4× oversampling of half-bit |
| DALI bit period | ~833.3 μs | 1200 bps nominal (IEC 62386) |
| Half-bit period | ~416.7 μs | Manchester encoding unit |
| TX-to-RX turnaround | 7 ms | Confirmed per IEC 62386-101 §8 |
| Reply timeout | 25 ms | 22 ms spec max + 3 ms margin (IEC 62386-101 §8) |
| Max bus scan time | TBD | Target: < 5 s for 64 addresses |
| Command round-trip latency | TBD | Soft target, to be determined |

> Fill in TBD values from IEC 62386 as they become relevant. When asking for generated code that involves timing, reference the specific values from this table.

---

## Architectural Principles

### Principle 1: Protocol logic must be independent of ESPHome

ESPHome is considered a presentation/integration layer.

DALI protocol handling must not depend on ESPHome entities.

Desired architecture:

```
ESPHome
↓
DALI Integration Layer
↓
DALI Protocol Layer
↓
DALI Scheduler
↓
DALI PHY
↓
DALI-2 Click
↓
DALI Bus
```

### Principle 2: Support arbitrary frame lengths

Do not assume all DALI frames are 16 bits.

The design must support:

* 16-bit frames (standard DALI)
* 24-bit frames (DALI-2 extended)
* DALI-2 instance-related messages
* Future protocol extensions

Preferred frame representation:

```c
struct DaliFrame {
    uint32_t data;
    uint8_t  bit_length;
};
```

Or a byte-array representation if very long frames are needed in future.

### Principle 3: PHY layer must be protocol-agnostic

The PHY layer should only know:

* Manchester encoding and decoding
* Bit timing
* Edge capture
* Frame transmission and reception
* Bus idle detection
* Half-duplex TX/RX mode switching

The PHY layer must not know:

* Device addresses
* Commands
* Sensors
* Instances
* Home Assistant entities

### Principle 4: Interrupts must remain minimal

Interrupt handlers should only:

**TX:**
* Output next symbol
* Advance the TX state machine

**RX:**
* Capture edge timestamps
* Push events into a ring buffer

Interrupts must not:
* Parse protocol
* Allocate memory
* Perform logging
* Call ESPHome APIs
* Acquire mutexes (use FreeRTOS ISR-safe variants only if synchronisation is necessary)
* Call any function not in IRAM

### Principle 5: Buffer-first architecture

Preferred flow:

```
ISR
↓
Ring Buffer  (ISR-safe, fixed-size, statically allocated)
↓
Frame Decoder  (task context)
↓
Protocol Layer  (task context)
```

Avoid:

```
ISR
↓
Protocol Parser
```

The ring buffer must support ISR writes and task reads without disabling interrupts on the task side. Prefer a lock-free single-producer / single-consumer design.

### Principle 6: Static allocation preferred

Prefer:

* Fixed-size ring buffers with compile-time sizes
* Static allocation for all persistent objects
* Stack allocation for short-lived objects in non-ISR code

Avoid:

* `malloc()` / `new` in ISR or timing-critical paths
* `std::vector` or other heap-growing containers in PHY or protocol layers

---

## Resource Constraints

| Resource | Total | Notes |
|---|---|---|
| SRAM | 520 KB | Budget ring buffers and queues conservatively |
| IRAM | ~128 KB shared with ISR code | Keep ISR footprint small |
| Flash | 8 MB | WROVER-E default; sdkconfig flash size must be set to 8 MB |
| CPU | 240 MHz dual-core | ISR must complete < 104 μs |

Suggested initial buffer sizes (validate and adjust):

```c
#define DALI_RX_EDGE_BUFFER_SIZE   64   // Edge timestamps pushed by ISR
#define DALI_TX_BIT_BUFFER_SIZE    64   // Encoded bits for TX
#define DALI_CMD_QUEUE_SIZE        16   // Pending commands to scheduler
#define DALI_RESPONSE_BUFFER_SIZE   8   // Awaiting-response slots
```

---

## Core Components

### DaliPhy

**Responsibilities:**
* TX timing and bit output
* RX edge capture and timestamp storage
* Manchester encode / decode
* Frame transmission and reception
* Bus idle detection
* TX/RX mode switching (half-duplex)

No protocol knowledge.

**Key design notes:**
* TX state machine driven by timer ISR
* RX decodes ring buffer events in task context, not ISR
* All ISR-called functions must be `IRAM_ATTR`

### DaliScheduler

**Responsibilities:**
* Bus arbitration
* Transaction queue management
* Timeouts and retries
* Reply waiting and timeout handling
* Transaction state machine

**Key design notes:**
* Runs in task context only — never called from ISR
* Interfaces with DaliPhy via a command queue and response buffer
* Must handle DALI's "send twice" requirement for configuration commands
* Retry count and timeout values should be compile-time constants, overridable

### DaliProtocol

**Responsibilities:**
* Build DALI commands (standard 16-bit and DALI-2 24-bit extended)
* Parse DALI responses (8-bit and 16-bit)
* Support DALI-2 control devices, device types, and instances
* Sensor queries

**Key design notes:**
* Stateless command construction where possible
* Parser must handle both 8-bit and 16-bit response lengths
* Instance addressing scheme must be defined early and kept consistent

### ESPHome Integration Layer

**Responsibilities:**
* Expose entities to Home Assistant
* Map Home Assistant actions to protocol calls
* Publish protocol state to Home Assistant

Should remain thin. No protocol logic here.

---

## Error Handling Strategy

Define failure modes explicitly rather than relying on silent failure:

| Failure | Detection | Response |
|---|---|---|
| No ACK / no response | Reply timeout | Retry N times, then mark device offline |
| Bus stuck low | Bus idle timeout | Log error, attempt bus recovery |
| Malformed RX frame | Manchester decode error | Discard frame, increment counter |
| RX ring buffer overflow | Full flag at ISR write time | Drop event, increment overflow counter |
| ISR overrun | Timer tick before ISR returns | Increment overrun counter; log on task side |
| TX collision | Simultaneous TX detected | Abort TX, backoff, retry |
| Scheduler queue full | Queue full on enqueue | Return error code to caller |

**General principles:**

* ISRs must never silently discard errors — use counters readable by the task side
* All counters must be accessible via the diagnostic shell (`stats` command)
* Hard faults (bus lockup) should allow graceful recovery without full device reset
* Return codes, not exceptions; use an explicit `DaliError` enum

---

## DALI vs DALI-2 Notes

**Backward compatibility:**

* DALI-2 devices must respond to standard DALI commands
* DALI-1 devices may be present on the same bus
* The protocol layer should handle both transparently

**Frame type summary:**

| Type | Length | Description |
|---|---|---|
| Standard DALI | 16-bit | 1 start + 8 address + 8 command |
| DALI-2 extended | 24-bit | Extended addressing / instance commands |

**Known DALI gotchas:**

* Some configuration commands must be sent twice within 100 ms
* Broadcast, group, and individual addressing must all be supported
* Query commands generate a response; set commands do not
* DALI-2 instance model: one physical device can have multiple instances (e.g., presence + illuminance on one sensor)

**Addressing types to support:**

```c
typedef enum {
    DALI_ADDR_SHORT     = 0,
    DALI_ADDR_GROUP     = 1,
    DALI_ADDR_BROADCAST = 2,
} DaliAddressType;
```

---

## Frame Examples

Reference examples for encoding tests and documentation:

**16-bit: DAPC (Direct Arc Power Control), address 0, level 128**
```
Address byte: 0x00  (short address 0, DAPC=0)
Command byte: 0x80  (level 128)
Raw frame:    0x0080
bit_length:   16
```

**16-bit: QUERY STATUS, address 5**
```
Address byte: 0x0B  (short address 5, query=1)
Command byte: 0x90
Raw frame:    0x0B90
bit_length:   16
```

**24-bit: DALI-2 instance command (example)**
```
Raw frame:    0x123456
bit_length:   24
```

**8-bit response (typical query reply):**
```
0xAF  — status byte returned within reply window
```

Use these as inputs to PHY unit tests and diagnostic shell `raw` commands.

---

## Diagnostics Requirements

Low-level access is a first-class feature, not an afterthought.

**Transport:** Serial, 115200 baud, text-based CLI

**Required commands:**

| Command | Description |
|---|---|
| `scan` | Scan all 64 short addresses, report present devices |
| `query <addr>` | Query device status at short address |
| `raw <hex> len=<n>` | Transmit arbitrary frame of specified bit length |
| `read` | Read last received raw response |
| `trace on/off` | Enable/disable per-frame bus trace logging |
| `stats` | Print all error and diagnostic counters |
| `reset` | Reset scheduler and PHY state machines |

**Example session:**

```
> scan
Device  0: type=ballast
Device  5: type=sensor, instances=2
Device 12: type=ballast

> query 5
Status: 0xAF, Illuminance: 542 lux, Presence: yes

> raw 0xFF00 len=16
TX: OK
RX: 0xAF (8-bit, 3.2 ms)

> stats
RX overflow:      0
TX retries:       2
Malformed frames: 0
Reply timeouts:   1
ISR overruns:     0

> trace on
[BUS] TX 0x0B90 (16-bit)
[BUS] RX 0xAF   (8-bit, 3.1 ms after TX)
```

---

## Code Organisation

```
project/
├── components/
│   └── dali/
│       ├── dali_frame.h          # DaliFrame struct, DaliError enum, shared types
│       ├── dali_ringbuf.h/.c     # ISR-safe lock-free ring buffer
│       ├── dali_phy.h/.c         # PHY: TX, RX, Manchester, edge capture
│       ├── dali_scheduler.h/.c   # Bus arbitration, transaction queue, timeouts
│       ├── dali_protocol.h/.c    # Frame construction, response parsing, DALI-2
│       └── dali_diag.h/.c        # Diagnostic serial CLI
├── esphome/
│   └── dali_esphome.h/.cpp       # ESPHome integration (thin layer only)
├── test/
│   ├── test_ringbuf.c            # Ring buffer unit tests (host-runnable)
│   ├── test_phy_encode.c         # PHY Manchester encode unit tests (host-runnable)
│   ├── test_phy_decode.c         # PHY Manchester decode unit tests (host-runnable)
│   ├── test_protocol.c           # Protocol frame construction/parsing tests
│   └── test_scheduler.c          # Scheduler state machine tests with mock PHY
└── main/
    └── main.c                    # Entry point, hardware init
```

## Build & Test Notes

* Top-level build is `idf.py` via ESP-IDF. The project is currently an ESP-IDF native CMake project.
* Host tests are standalone in `test/` using CMake and Unity. `DALI_HOST_BUILD=1` is defined for host portability.
* Expected host test workflow:
  * `cd test`
  * `cmake -B build -G "MinGW Makefiles"` or `"Unix Makefiles"` on WSL
  * `cmake --build build`
  * `ctest --output-on-failure`
* There is no repository `README.md`; use `current_status.md` for status, architecture, and phase guidance.

**Naming conventions:**

* ISR functions: `dali_phy_tx_isr()`, `dali_phy_rx_isr()` — always `IRAM_ATTR`
* State machine states: `DALI_PHY_TX_IDLE`, `DALI_PHY_TX_START_BIT`, `DALI_PHY_TX_DONE`, etc.
* Ring buffer ops: `dali_rb_push_from_isr()`, `dali_rb_pop()`
* Error counters: `dali_stats_t` struct, globally readable, updated atomically
* Enums: `UPPER_SNAKE_CASE`; functions: `lower_snake_case` with component prefix

**Integer types:** Always use `uint8_t`, `uint16_t`, `uint32_t`. Do not use `int`, `unsigned`, or `size_t` in protocol or PHY layers.

---

## Logging Strategy

| Context | Logging approach |
|---|---|
| ISR | None. Increment counters only. |
| Task — state transitions | `ESP_LOGD` / `ESP_LOGI` |
| Diagnostic shell | Structured human-readable output |
| Protocol trace | Optional per-frame log, toggled by `trace on/off` |

Example trace format:

```
[DALI-PHY]   TX start: 0x0B90 (16-bit)
[DALI-PHY]   RX complete: 0xAF (8-bit), 3.1 ms after TX
[DALI-SCHED] Reply timeout: addr=5, retry 1/2
[DALI-PROTO] Response parsed: QUERY_STATUS = 0xAF
```

Do not log inside timing-critical paths. Logging overhead during protocol operation should not affect bus timing.

---

## Testing and Validation

### PHY Timing

* Toggle a spare GPIO at ISR entry and exit; measure with logic analyzer to verify execution stays < 104 μs
* Verify Manchester-encoded output bit timings against IEC 62386 tolerances
* Test worst-case ISR path (not average case)

### Frame Encoding/Decoding

* Unit-test `DaliFrame` encode/decode on host (no hardware required)
* Cover 16-bit, 24-bit, and boundary cases
* Regression: known-good frames from logic analyzer captures

### Scheduler

* Test state machine transitions with a mock PHY (no real bus required)
* Test timeout and retry behaviour under simulated no-response conditions
* Test "send twice" enforcement for configuration commands

### Integration

| Stage | Method |
|---|---|
| 1. Loopback | TX into RX on same device; verify frame integrity |
| 2. Known ballast | 16-bit commands; verify ACK and status responses |
| 3. DALI-2 sensor | Instance queries, 24-bit frames |
| 4. Mixed bus | DALI-1 and DALI-2 devices coexisting |

---

## FreeRTOS Considerations

* The DALI processing task should have higher priority than the ESPHome / Wi-Fi tasks to minimise scheduler response latency
* Consider pinning the DALI task to Core 1 if Wi-Fi activity on Core 0 causes timer jitter
* Do not use `vTaskDelay()` or any blocking FreeRTOS call from within ISR context
* If a semaphore or notification is needed to wake the DALI task from ISR, use `xSemaphoreGiveFromISR()` / `vTaskNotifyGiveFromISR()` — not the standard variants
* All DALI task stack sizes should be defined as named constants, not magic numbers

---

## Development Priorities

1. `DaliPhy` — TX state machine, Manchester encoding, timer ISR skeleton
2. `DaliPhy` — RX ring buffer, edge capture, Manchester decoding in task context
3. Raw TX/RX testing (loopback or logic analyzer verification)
4. Diagnostic serial CLI
5. `DaliScheduler` — transaction queue, timeouts, retries, send-twice
6. `DaliProtocol` — 16-bit frame construction and response parsing
7. Device discovery (bus scan)
8. `DaliProtocol` — DALI-2 extensions, instance commands, sensor queries
9. ESPHome integration layer
10. Home Assistant entities

ESPHome entities should be added only after reliable end-to-end protocol communication is confirmed on real hardware.

---

## Open Questions

Track these explicitly rather than letting them become hidden assumptions:

| Question | Status |
|---|---|
| Which ESP32 timer peripheral to use? (Timer Group vs RMT vs LEDC) | **Resolved** — GPTIMER, 104 µs alarm |
| Exact TX-to-RX turnaround time from IEC 62386? | **Resolved** — 7 ms (IEC 62386-101 §8) |
| Exact reply timeout from IEC 62386? | **Resolved** — 25 ms (22 ms spec max + 3 ms margin) |
| Maximum retry count before marking device offline? | **Decided** — 3 retries (tune on real hardware) |
| How to handle DALI-2 instance discovery (auto vs manual)? | **Decided** — manual; auto-discovery deferred to post-loopback |
| ESPHome component type: custom component or external component? | **Decided** — in-tree custom component now; migrate to external after Phase 9 works |
| Will unit tests run on host (Linux) or on-device only? | **Decided** — MinGW on Windows |
| How to handle firmware updates to DALI-2 devices (DFU)? | Out of scope for now |

---

## Guidance for Generated Code

If in doubt, ask.

**Prefer:**

* Explicit enum-based state machines with `switch/case`
* Fixed-size buffers with compile-time size constants (`#define` or `static const`)
* Static allocation; avoid `malloc` / `new` in protocol or PHY layers
* `uint8_t`, `uint16_t`, `uint32_t` — not `int`, `size_t`, or `unsigned`
* `IRAM_ATTR` on all functions called from ISR context
* Error counters over silent failure
* Named return codes via `DaliError` enum, not bare integers or exceptions
* Explicit state enum values — do not rely on implicit integer ordering

**Avoid:**

* Dynamic allocation in timing-critical or ISR paths
* Protocol logic inside interrupt handlers
* `std::vector`, `std::string`, or other heap-growing STL types in PHY or protocol layers
* ESPHome-specific assumptions in `DaliPhy`, `DaliScheduler`, or `DaliProtocol`
* Mutex / semaphore calls from ISR context (use FreeRTOS ISR-safe variants only)
* Magic numbers — all timing constants, buffer sizes, and retry counts should be named

**When proposing designs:**

* Show state machine states explicitly as named enums
* Show ring buffer producer/consumer contracts clearly, especially ISR-safety guarantees
* Explain timing assumptions when relevant and reference the constraints table above
* Prioritise maintainability, observability, and protocol correctness over rapid Home Assistant integration