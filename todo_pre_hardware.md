# Pre-Hardware Software Cleanup

This list covers work that can be done before relying on DALI hardware behavior.
The goal is to make native ESP-IDF bring-up less ambiguous.

## Configuration

- [ ] Add committed `sdkconfig.defaults`.
  - [ ] Set flash size for ESP32-WROVER-E 8 MB flash.
  - [ ] Decide whether to set CPU frequency to 240 MHz for ISR margin.
  - [ ] Avoid a high-priority busy loop if `pdMS_TO_TICKS(1u)` becomes 0 with
        a 100 Hz FreeRTOS tick.
  - [ ] Verify or enable IRAM-safe GPTIMER/GPIO options if available.

## PHY

- [ ] Add bus-idle / stuck-low guard before TX.
  - [ ] Return `DALI_ERR_BUS_STUCK` when the line is active too long.
  - [ ] Increment a diagnostic counter readable by `stats`.
- [ ] Add RX self-echo suppression while the controller is transmitting.
- [ ] Suppress RX during the 7 ms TX-to-RX settle period.
- [ ] Audit every ISR-called function for IRAM safety.
- [ ] Keep ISR work limited to symbol output, timestamp capture, state advance,
      and counters.

## Scheduler And RX Routing

- [ ] Gate scheduler replies so `dali_sched_notify_rx()` only completes a
      transaction when the scheduler is actually in `SCHED_WAIT_REPLY`.
- [ ] Add tests for stray, stale, and late RX frames.
- [ ] Add a separate event path for unsolicited DALI-2 input-device events.
- [ ] Make sure unsolicited frames cannot complete unrelated request/reply
      transactions.

## Diagnostic CLI

- [ ] Implement real task-context trace output.
  - TX example: `[BUS] TX 0x0B90 (16-bit)`
  - RX example: `[BUS] RX 0xAF (8-bit, 3.1 ms after TX)`
- [ ] Add `read` to print the last received raw frame.
- [ ] Extend `raw <hex> len=<n>` with an optional wait-for-reply mode.
- [ ] Add richer named command helpers so diagnostics do not require raw hex for
      common operations.
- [ ] Add `discover`, `inventory`, and `identify` commands after the scheduler
      RX gating is safe.

## Protocol And Control

- [ ] Finish specialized parser helpers.
  - [ ] Packed fade-time/fade-rate nibbles.
  - [ ] Combined 16-bit / multi-byte DALI-2 input values.
- [ ] Add focused tests for each command metadata entry and parser kind.
- [ ] Verify public-source command constants against IEC 62386 or manufacturer
      documentation before relying on them for commissioning.
- [ ] Keep Steinel-specific helpers out of PHY and scheduler.
- [ ] Add a future static mapping layer, but do not hard-code entity names or
      "lamp 1 means group 0" assumptions in protocol code.

## Diagnostics Counters

- [ ] Consider adding counters for:
  - [ ] Bus stuck / idle failures.
  - [ ] RX frames ignored outside reply window.
  - [ ] Unsolicited events routed.
  - [ ] RX self-echo frames suppressed.
  - [ ] Raw malformed frame length / parser failures.
