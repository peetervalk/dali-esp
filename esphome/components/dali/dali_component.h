#pragma once

#include "esphome/core/component.h"

#include <atomic>
#include <cstdint>

// Forward declarations — full includes are in the .cpp files.
namespace esphome { namespace text_sensor { class TextSensor; } }

namespace esphome {
namespace dali {

/*
 * Minimal interface used by DaliComponent to update light entity state from
 * the DALI bus (dispatch snooping and QUERY_ACTUAL_LEVEL replies).
 * DaliLightOutput implements this; dali_component.cpp never needs to include
 * the light output header.
 */
class DaliBusLight {
 public:
  virtual ~DaliBusLight() = default;
  virtual void    mark_state_from_bus(bool is_on, uint8_t level) = 0;
  virtual void    apply_bus_state() = 0;
  virtual uint8_t get_query_address() const = 0;
};

class DaliComponent : public Component {
 public:
  void set_pins(uint8_t tx_pin, uint8_t rx_pin) {
    tx_pin_ = tx_pin;
    rx_pin_ = rx_pin;
  }

  void set_poll_interval(uint32_t seconds) { poll_interval_s_ = seconds; }

  void setup() override;
  void loop() override;

  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // ── Core actions (callable from Core 0 press handlers) ──────────────────

  void start_scan();
  void start_refresh();
  // Blink diag_address_ between max and min for 10 s to identify a fixture.
  void start_identify();
  // Record unsolicited bus frames for 30 s; publish result to couplers_result_.
  void start_find_couplers();
  // Direct control using the current diag_address_ (short address).
  void send_diag_on();
  void send_diag_off();
  void send_diag_max();
  void send_diag_min();

  // ── Diagnostic target address (set by DaliAddressNumber, Core 0 only) ───

  void    set_diag_address(uint8_t a) { diag_address_ = a; }
  uint8_t get_diag_address() const    { return diag_address_; }

  // ── Optional text sensors ────────────────────────────────────────────────

  void set_scan_status_sensor(text_sensor::TextSensor *s)     { scan_status_     = s; }
  void set_scan_result_sensor(text_sensor::TextSensor *s)     { scan_result_     = s; }
  void set_couplers_result_sensor(text_sensor::TextSensor *s) { couplers_result_ = s; }
  void set_bus_monitor_sensor(text_sensor::TextSensor *s)     { bus_monitor_     = s; }

  // ── Callbacks (called from other tasks / Core 1) ─────────────────────────

  // Called from scan task (Core 1) before scan_done_ gate fires.
  void set_scan_result_pending(const char *summary);
  // Called from scan task when finished.
  void on_scan_complete(uint8_t count, bool success);
  // Called by DaliLightOutput during codegen init (Core 0 setup phase).
  void register_light(uint8_t target_type, uint8_t target_address, DaliBusLight *light);

 protected:
  uint8_t tx_pin_{18};
  uint8_t rx_pin_{19};

  // Text sensors (Core 0 only).
  text_sensor::TextSensor *scan_status_{nullptr};
  text_sensor::TextSensor *scan_result_{nullptr};
  text_sensor::TextSensor *couplers_result_{nullptr};
  text_sensor::TextSensor *bus_monitor_{nullptr};

  // Scan state (Core 1 writes, Core 0 reads via atomic gate).
  std::atomic<bool>    scan_done_{false};
  std::atomic<bool>    scan_running_{false};
  std::atomic<uint8_t> scan_count_{0};
  std::atomic<bool>    scan_success_{false};

  // Periodic poll (Core 0 only).
  uint32_t poll_interval_s_{0};
  uint32_t last_poll_ms_{0};
  bool     boot_query_done_{false};

  // Deferred query after dim/scene (Core 0 only, signalled via module atomic).
  bool     deferred_query_armed_{false};
  uint32_t deferred_query_arm_ms_{0};

  // Diagnostic target — written and read exclusively on Core 0.
  uint8_t diag_address_{0};

  // Identify blink (Core 0 only).
  bool     identify_active_{false};
  uint32_t identify_start_ms_{0};
  uint32_t identify_last_ms_{0};
  bool     identify_phase_{false};

  // Find couplers timer (Core 0); active flag is the module-level atomic.
  uint32_t find_couplers_end_ms_{0};
  bool     find_couplers_collect_{false};  // one-tick drain gate
};

}  // namespace dali
}  // namespace esphome
