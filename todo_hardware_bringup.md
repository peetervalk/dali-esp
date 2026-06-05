# Hardware Bring-Up Plan

This plan is for developer stack debugging. It uses native ESP-IDF firmware,
serial diagnostics, logic analyzer captures, and Lunatone DALI USB / DALI
Cockpit as the main external reference. ESPHome is not part of low-level stack
debugging.

## Tools

- Native ESP-IDF build and monitor.
- Serial CLI at 115200 baud.
- Logic analyzer on ESP-side logic signals.
- Oscilloscope or multimeter for electrical sanity checks.
- Lunatone DALI USB / DALI Cockpit for known-good DALI behavior and bus
  monitoring.

## Stage 0 - Basic ESP32 Bring-Up

- [ ] Flash a known-good ESP-IDF hello-world project.
- [ ] Confirm USB serial monitor output.
- [ ] Confirm the board can be erased and reflashed reliably.
- [ ] Confirm `idf.py build flash monitor` workflow on the target COM port.

## Stage 1 - Wiring And Electrical Sanity

- [ ] Confirm MikroE DALI-2 Click wiring to the ESP32-DevKitC-VE.
- [ ] Update `DALI_TX_GPIO` and `DALI_RX_GPIO` in `main/main.c`.
- [ ] Do not use GPIO 16 or GPIO 17 on WROVER-E because they are connected to
      PSRAM.
- [ ] Confirm DALI bus supply and idle voltage with a meter/scope.
- [ ] Confirm ESP-side RX/TX polarity at the Click board logic pins.

## Stage 2 - Timing And Loopback

- [ ] Toggle a spare debug GPIO at GPTIMER ISR entry/exit.
- [ ] Measure ISR duration and verify worst-case execution is below 104 us.
- [ ] Verify GPTIMER alarm period is 104 us.
- [ ] Verify generated Manchester half-bit timing is about 416.7 us.
- [ ] Wire ESP-side TX to RX for loopback if safe for the adapter wiring.
- [ ] Run `raw 0x0080 len=16` and verify received frame integrity.
- [ ] Check `stats` for zero RX overflow, malformed frames, and ISR overruns.

## Stage 3 - Lunatone Reference Baseline

- [ ] Use Lunatone DALI Cockpit to scan/address the DALI line.
- [ ] Use DALI Monitor to capture known-good commands and replies.
- [ ] Save useful monitor logs for later comparison.
- [ ] Record known addresses, groups, device types, and sensor/control-device
      configuration.
- [ ] Avoid two active masters during early tests. Use Lunatone and ESP gateway
      one at a time unless explicitly testing multi-master behavior later.

## Stage 4 - Known Control Gear

- [ ] Start with one known ballast/driver.
- [ ] Run `scan` and confirm the expected address responds.
- [ ] Run `status <addr>` or `query <addr>` and parse QUERY STATUS.
- [ ] Compare the ESP frame bytes and replies with Lunatone captures.
- [ ] Test `off`, `max`, `min`, and DAPC `level` commands once TX/RX are
      stable.
- [ ] Verify group commands only after single-address commands are stable.

## Stage 5 - Steinel Sensor

- [ ] Confirm the Steinel HF 360 II DALI-2 IPD address in Lunatone Cockpit.
- [ ] Query basic status and version/device information from ESP diagnostics.
- [ ] Implement and test instance value reads.
- [ ] Poll expected instances:
  - [ ] Instance 0 brightness.
  - [ ] Instance 1 motion.
  - [ ] Instance 2 temperature.
  - [ ] Instance 3 humidity.
- [ ] Compare input values and events with Lunatone monitor output.

## Stage 6 - Regression Captures

- [ ] Keep known-good logic analyzer traces for:
  - [ ] 16-bit DAPC.
  - [ ] 16-bit QUERY STATUS with 8-bit reply.
  - [ ] 24-bit DALI-2 instance query.
  - [ ] Sensor event / switch event.
- [ ] Convert stable captures into host regression tests when practical.
