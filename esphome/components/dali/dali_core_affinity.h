#pragma once

/*
 * Core affinity for the DALI worker tasks: the DALI task, the scan task and
 * the shell task.
 *
 * All three block on bus round-trips — the scan for tens of seconds — so they
 * run as tasks of their own and never on the ESPHome main loop. On a
 * dual-core part they are pinned to core 1. ESPHome 2026.9 pins its loop
 * task to core 1 as well (components/esp32/core.cpp), so the loop and the
 * workers share that core. A worker that blocks yields it to the loop; one
 * that spins does not, which is why the PHY's pre-transmit idle check costs
 * the loop time.
 *
 * The core must not be hardcoded: on a single-core target (ESP32-S2, ESP32-C3,
 * and the single-core ESP32 variants) it does not exist, and
 * xTaskCreatePinnedToCore() fails outright rather than falling back. There the
 * right request is no affinity, letting the scheduler interleave the workers
 * with the main loop.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace dali {

#if defined(CONFIG_FREERTOS_NUMBER_OF_CORES)
static constexpr int DALI_CORE_COUNT = CONFIG_FREERTOS_NUMBER_OF_CORES;
#elif defined(CONFIG_FREERTOS_UNICORE)
static constexpr int DALI_CORE_COUNT = 1;
#else
static constexpr int DALI_CORE_COUNT = portNUM_PROCESSORS;
#endif

constexpr BaseType_t dali_worker_core() {
  return DALI_CORE_COUNT > 1 ? static_cast<BaseType_t>(1) : tskNO_AFFINITY;
}

}  // namespace dali
}  // namespace esphome
