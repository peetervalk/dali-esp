#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "../../../components/dali/dali_refresh_cursor.h"

extern "C" {
#include "../../../components/dali/dali_frame.h"  // DaliError
}

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
  // Collect the scheduler completion for any in-flight command, then admit at
  // most one pending write. Called every loop, scan or not; the implementation
  // gates its own transmission on the scan.
  virtual void    flush_pending_write() = 0;
  virtual uint8_t get_query_address() const = 0;
};

/*
 * Minimal interface used by DaliComponent to poll input device instances and
 * transfer values to ESPHome sensor entities (Core 1 → Core 0 mailbox).
 * DaliInputSensor implements this; dali_component.cpp never needs to include
 * the sensor/ subdirectory header.
 */
class DaliBusSensor {
 public:
  virtual ~DaliBusSensor() = default;
  virtual uint8_t  get_address()         const = 0;
  virtual uint8_t  get_instance()        const = 0;
  virtual uint32_t get_poll_interval_s() const = 0;
  virtual uint8_t  get_value_bytes()     const = 0;
  virtual uint32_t get_last_poll_ms()    const = 0;
  virtual void     set_last_poll_ms(uint32_t ms) = 0;
  virtual void     mark_raw_value(uint16_t raw)  = 0;
  virtual void     apply_value()                 = 0;
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
  bool is_scan_running() const {
    return scan_running_.load(std::memory_order_acquire);
  }
  // Blink diag_address_ between max and min for 10 s to identify a fixture.
  void start_identify();
  // Record unsolicited bus frames for 30 s; publish result to couplers_result_.
  void start_find_couplers();
  // Direct control using the current diag_address_ (short address).
  void send_diag_on();
  void send_diag_off();
  void send_diag_max();
  void send_diag_min();
  // Query ACTUAL_LEVEL for diag_address_ and publish result to scan_status_.
  void send_diag_refresh();

  // ── Diagnostic target address (set by DaliAddressNumber, Core 0 only) ───

  void    set_diag_address(uint8_t a) { diag_address_ = a; }
  uint8_t get_diag_address() const    { return diag_address_; }

  // ── Optional text sensors ────────────────────────────────────────────────

  void set_scan_status_sensor(text_sensor::TextSensor *s)     { scan_status_     = s; }
  void set_scan_result_sensor(text_sensor::TextSensor *s)     { scan_result_     = s; }
  void set_yaml_result_sensor(text_sensor::TextSensor *s)     { yaml_result_     = s; }
  void set_couplers_result_sensor(text_sensor::TextSensor *s) { couplers_result_ = s; }
  void set_bus_monitor_sensor(text_sensor::TextSensor *s)     { bus_monitor_     = s; }
  void set_command_result_sensor(text_sensor::TextSensor *s)  { command_result_  = s; }
  void set_bus_fault_sensor(text_sensor::TextSensor *s)       { bus_fault_       = s; }

  // ── Text command interface ──────────────────────────────────────────────────

  // Parse and execute a CLI-style DALI command string (called from Core 0).
  // Supports: off/max/min/level/query/config/iconfig <target> [args...]
  void execute_command(const std::string &cmd);

  // ── Callbacks (called from other tasks / Core 1) ─────────────────────────

  // Called from scan task (Core 1) before scan_done_ gate fires.
  void set_scan_result_pending(const char *summary);
  void set_scan_yaml_pending(const char *yaml);
  // Called from scan task when finished.
  void on_scan_complete(uint8_t count, bool success, bool data_complete);
  // Returns bitmask of DALI groups that had frames observed during last coupler scan.
  uint16_t get_coupler_group_mask() const;
  // Called from scan task (Core 1) after a successful scan — replaces the
  // runtime group-membership table (masks[g] = bitmask of short addresses
  // currently in group g) used to auto-select query_address in start_refresh().
  // Rejects partial/unverified observations so they cannot overwrite and
  // persist the previous known-good group map.
  bool set_group_membership_snapshot(const uint64_t masks[16], uint16_t verified,
                                     uint64_t observed_gear);
  // Called by DaliLightOutput during codegen init (Core 0 setup phase).
  void register_light(uint8_t target_type, uint8_t target_address,
                      uint16_t member_groups, DaliBusLight *light);
  // Called by DaliInputSensor during codegen init (Core 0 setup phase).
  void register_input_sensor(DaliBusSensor *sensor);
  // Called once per headless_dispatch entry during codegen init (Core 0 setup phase).
  void add_dispatch_entry(uint8_t frame_kind, uint8_t address_kind, uint8_t address,
                          uint16_t event_information, uint8_t instance,
                          uint8_t output_type, uint8_t output_address,
                          uint8_t action, uint8_t scene);

 protected:
  uint8_t tx_pin_{18};
  uint8_t rx_pin_{19};

  // Text sensors (Core 0 only).
  text_sensor::TextSensor *scan_status_{nullptr};
  text_sensor::TextSensor *scan_result_{nullptr};
  text_sensor::TextSensor *yaml_result_{nullptr};
  text_sensor::TextSensor *couplers_result_{nullptr};
  text_sensor::TextSensor *bus_monitor_{nullptr};
  text_sensor::TextSensor *command_result_{nullptr};
  text_sensor::TextSensor *bus_fault_{nullptr};

  // Bus fault tracking (Core 0 only; reads volatile g_dali_stats.bus_idle_failures).
  uint32_t last_bus_fault_count_{0u};

  // Scheduler queue drop tracking (Core 0 only). Cumulative rejections already
  // reported, so only newly dropped work produces a log line.
  uint32_t last_queue_rejected_full_{0u};
  uint32_t last_queue_rejected_busy_{0u};

  // Scan state (Core 1 writes, Core 0 reads via atomic gate).
  std::atomic<bool>    scan_done_{false};
  std::atomic<bool>    scan_running_{false};
  std::atomic<uint8_t> scan_count_{0};
  std::atomic<bool>    scan_success_{false};
  std::atomic<bool>    scan_data_complete_{false};

  // Periodic poll (Core 0 only).
  uint32_t poll_interval_s_{0};
  uint32_t last_poll_ms_{0};
  bool     boot_query_done_{false};
  DaliRefreshCursor refresh_cursor_{};
  bool     refresh_queue_blocked_{false};

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
  bool     identify_scan_paused_{false};
  uint32_t identify_scan_pause_ms_{0};

  // Find couplers timer (Core 0); active flag is the module-level atomic.
  uint32_t find_couplers_end_ms_{0};
  bool     find_couplers_collect_{false};  // waits for DALI-task stop acknowledgement

  // Input sensor boot query (Core 0 only).
  bool boot_sensor_query_done_{false};

  // Group-membership persistence (Core 0 only). Snapshot of s_group_members +
  // verified mask, saved to flash whenever a scan or console group edit dirties
  // the table so it survives reboots. Loaded once in setup().
  ESPPreferenceObject group_pref_;
  // Load persisted membership into the runtime table; true if valid data applied.
  bool load_group_membership();
  // Write the current runtime table to flash.
  void save_group_membership();
  // Admit at most one eligible light query per loop; queue pressure retains the
  // cursor so that the same entry is retried instead of being dropped.
  void pump_refresh();
  // Surface a rejected diagnostic-button enqueue; no-op on DALI_OK.
  void report_diag_enqueue_(const char *what, DaliError err);
  // Log a warning when the scheduler's cumulative rejection counters advance.
  void report_queue_drops_();
};

}  // namespace dali
}  // namespace esphome
