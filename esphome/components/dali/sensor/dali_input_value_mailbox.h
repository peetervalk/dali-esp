#pragma once

#include <atomic>
#include <cstdint>

namespace esphome {
namespace dali {

/*
 * Single-slot, latest-value mailbox for Core 1 -> Core 0 input-sensor readings.
 *
 * The value and its "something is here" flag live in one atomic word for the
 * same reason the light-state mailbox packs its pair: with a separate value and
 * dirty flag, the reader has to clear the flag and then read the value, and a
 * publish landing between those two steps is lost with its flag already
 * consumed. The next poll recovers it, so the symptom is a stale reading for
 * one interval rather than a wrong one — but it is the same lost update, and
 * one exchange removes it.
 *
 * A publish racing with a take is either returned by that take or stays pending
 * for the next one.
 */
class DaliInputValueMailbox {
 public:
  void publish(uint16_t raw) {
    value_.store(VALID_BIT | static_cast<uint32_t>(raw), std::memory_order_release);
  }

  bool take(uint16_t &raw) {
    uint32_t value = value_.exchange(0u, std::memory_order_acq_rel);
    if ((value & VALID_BIT) == 0u) return false;
    raw = static_cast<uint16_t>(value & RAW_MASK);
    return true;
  }

 private:
  static constexpr uint32_t RAW_MASK = 0xFFFFu;
  static constexpr uint32_t VALID_BIT = 1u << 16u;

  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "input-value mailbox requires lock-free 32-bit atomics");
  std::atomic<uint32_t> value_{0u};
};

}  // namespace dali
}  // namespace esphome
