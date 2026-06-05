# Pre-Hardware Software Cleanup

This list covers work that can be done before relying on DALI hardware behavior.
The goal is to make native ESP-IDF bring-up less ambiguous.

## Configuration

- [x] Add committed `sdkconfig.defaults`.
  - [x] Set flash size for ESP32-WROVER-E 8 MB flash.
  - [x] Decide whether to set CPU frequency to 240 MHz for ISR margin.
  - [x] Avoid a high-priority busy loop if `pdMS_TO_TICKS(1u)` becomes 0 with
        a 100 Hz FreeRTOS tick.
  - [x] Verify or enable IRAM-safe GPTIMER/GPIO options if available.

## PHY

- [x] Add bus-idle / stuck-low guard before TX.
  - [x] Return `DALI_ERR_BUS_STUCK` when the line is active too long.
  - [x] Increment a diagnostic counter readable by `stats`.
- [x] Add RX self-echo suppression while the controller is transmitting.
- [x] Suppress RX during the 7 ms TX-to-RX settle period.
- [x] Audit every ISR-called function for IRAM safety.
- [x] Keep ISR work limited to symbol output, timestamp capture, state advance,
      and counters.

## Scheduler And RX Routing

- [x] Gate scheduler replies so `dali_sched_notify_rx()` only completes a
      transaction when the scheduler is actually in `SCHED_WAIT_REPLY`.
- [x] Add tests for stray, stale, and late RX frames.
- [x] Add a separate event path for unsolicited DALI-2 input-device events.
- [x] Make sure unsolicited frames cannot complete unrelated request/reply
      transactions.

## Diagnostic CLI

- [x] Implement real task-context trace output.
  - TX example: `[BUS] TX 0x0B90 (16-bit)`
  - RX example: `[BUS] RX 0xAF (8-bit, 3.1 ms after TX)`
- [x] Add `read` to print the last received raw frame.
- [x] Extend `raw <hex> len=<n>` with an optional wait-for-reply mode.
- [x] Route `raw` TX through the scheduler in both wait and no-wait modes.
- [x] Add richer named command helpers so diagnostics do not require raw hex for
      common operations.
- [x] Add `discover`, `inventory`, and `identify` commands after the scheduler
      RX gating is safe.

## Protocol And Control

- [x] Finish specialized parser helpers.
  - [x] Packed fade-time/fade-rate nibbles.
  - [x] Combined 16-bit / multi-byte DALI-2 input values.
- [x] Add focused tests for each command metadata entry and parser kind.
- [x] Verify public-source command constants against IEC 62386 or manufacturer
      documentation before relying on them for commissioning.
  - [x] Mark `0x40`..`0x46` input-instance scaling queries as Lunatone-specific,
        not generic IEC 62386-103 commands.
- [x] Keep Steinel-specific helpers out of PHY and scheduler.
- [x] Add a future static mapping layer, but do not hard-code entity names or
      "lamp 1 means group 0" assumptions in protocol code.

## Diagnostics Counters

- [x] Consider adding counters for:
  - [x] Bus stuck / idle failures.
  - [x] RX frames ignored outside reply window.
  - [x] Unsolicited events routed.
  - [x] RX self-echo frames suppressed.
  - [x] RX settle-period frames suppressed.
  - [x] Raw malformed frame length / parser failures.
