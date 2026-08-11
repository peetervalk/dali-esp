#pragma once

/*
 * Core affinity for the DALI worker tasks.
 *
 * Both the DALI task and the scan task block on bus round-trips — the scan for
 * tens of seconds — so neither belongs on the core running the ESPHome main
 * loop. On a dual-core part that means Core 1.
 *
 * Core 1 must not be hardcoded: on a single-core target (ESP32-S2, ESP32-C3,
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
