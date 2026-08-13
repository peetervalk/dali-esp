#pragma once

#include <atomic>
#include <cstdint>

namespace esphome {
namespace dali {

/*
 * Single-slot, latest-value mailbox for Core 1 -> Core 0 light-state updates.
 * Packing the pair into one atomic word prevents mixed on/level snapshots. An
 * publish racing with an exchange is either returned by that exchange or
 * remains pending for the next drain.
 */
class DaliLightStateMailbox {
 public:
  void publish(bool is_on, uint8_t level) {
    uint32_t value = VALID_BIT | static_cast<uint32_t>(level);
    if (is_on) value |= ON_BIT;
    value_.store(value, std::memory_order_release);
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

  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "light-state mailbox requires lock-free 32-bit atomics");
  std::atomic<uint32_t> value_{0u};
};

}  // namespace dali
}  // namespace esphome
