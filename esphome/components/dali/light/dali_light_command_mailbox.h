#pragma once

#include <atomic>
#include <cstdint>

namespace esphome {
namespace dali {

/*
 * Single-slot mailbox for the scheduler completion of one light command
 * (Core 1 -> Core 0).
 *
 * Only one command per light is in flight at a time, so a second publish before
 * a drain cannot happen in practice. The counter still makes the handoff
 * self-describing rather than relying on that invariant: take() reports how many
 * completions it is acknowledging and whether any of them failed, so a dropped
 * outcome can never be mistaken for a success.
 */
class DaliLightCommandMailbox {
 public:
  void publish(bool success) {
    uint32_t previous = value_.load(std::memory_order_relaxed);
    uint32_t next;
    do {
      uint32_t count = (previous & COUNT_MASK) + 1u;
      if (count > COUNT_MASK) count = COUNT_MASK;  // saturate rather than wrap
      next = count | ((previous & FAILED_BIT) != 0u || !success ? FAILED_BIT : 0u);
    } while (!value_.compare_exchange_weak(previous, next,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed));
  }

  // Returns false when no completion is waiting. Otherwise `success` is false
  // if any collected completion failed, and `count` is how many were collected.
  bool take(bool &success, uint32_t &count) {
    uint32_t value = value_.exchange(0u, std::memory_order_acq_rel);
    count = value & COUNT_MASK;
    if (count == 0u) return false;
    success = (value & FAILED_BIT) == 0u;
    return true;
  }

 private:
  static constexpr uint32_t COUNT_MASK = 0x7FFFFFFFu;
  static constexpr uint32_t FAILED_BIT = 1u << 31u;

  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "light-command mailbox requires lock-free 32-bit atomics");
  std::atomic<uint32_t> value_{0u};
};

}  // namespace dali
}  // namespace esphome
