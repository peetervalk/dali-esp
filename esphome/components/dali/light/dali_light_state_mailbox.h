#pragma once

#include <atomic>
#include <cstdint>

namespace esphome {
namespace dali {

/*
 * Single-slot, latest-value mailbox for light-state updates, DALI task -> loop task.
 * Packing the pair into one atomic word prevents mixed on/level snapshots. An
 * publish racing with an exchange is either returned by that exchange or
 * remains pending for the next drain.
 *
 * Both sides are tasks, so the word need not be lock-free, and on an ESP32 or
 * ESP32-S3 with PSRAM it is not: IDF builds with -mdisable-hardware-atomics,
 * and an exchange becomes a call that takes a spinlock when the word is in
 * PSRAM. publish() exchanges rather than stores so it takes that lock too; a
 * plain store could land between the call's read and its write and be lost.
 */
class DaliLightStateMailbox {
 public:
  void publish(bool is_on, uint8_t level) {
    uint32_t value = VALID_BIT | static_cast<uint32_t>(level);
    if (is_on) value |= ON_BIT;
    value_.exchange(value, std::memory_order_release);
  }

  bool take(bool &is_on, uint8_t &level) {
    uint32_t value = value_.exchange(0u, std::memory_order_acq_rel);
    if ((value & VALID_BIT) == 0u) return false;
    is_on = (value & ON_BIT) != 0u;
    level = static_cast<uint8_t>(value & LEVEL_MASK);
    return true;
  }

 private:
  static constexpr uint32_t LEVEL_MASK = 0xFFu;
  static constexpr uint32_t VALID_BIT = 1u << 8u;
  static constexpr uint32_t ON_BIT = 1u << 9u;

  std::atomic<uint32_t> value_{0u};
};

}  // namespace dali
}  // namespace esphome
