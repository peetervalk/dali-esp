#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "../../../components/dali/dali_refresh_cursor.h"

extern "C" {
#include "../../../components/dali/dali_frame.h"  // DaliError
#include "../../../components/dali/dali_cli.h"    // DaliCliTokens
#include "../../../components/dali/dali_discovery.h"
}

#include <atomic>
#include <cstdint>

// Forward declarations — full includes are in the .cpp files.
namespace esphome { namespace text_sensor { class TextSensor; } }

namespace esphome {
namespace dali {

/*
 * The `shell:` sub-block, which DaliShellServer owns rather than
 * DaliComponent. Passed in at export time so the component does not need a
 * back-reference to a front end it otherwise knows nothing about.
 */
struct DaliShellConfigInfo {
  uint16_t port;
  uint32_t idle_timeout_s;
  bool     allow_commissioning;
};

/*
 * The shell's per-instance input-device lookup, as `export config` receives
 * it. Declared here rather than pulled in from dali_shell.h for the same
 * reason DaliShellConfigInfo is: that header is the diagnostic shell's whole
 * interface, and this component needs two names out of it. The definition
 * there is canonical; a drift between them is a compile error at the one
 * place both headers are in scope, DaliShellServer::export_config_cb.
 */
using DaliShellInputLookupFn = bool (*)(uint8_t addr,
                                        DaliDiscoveryInputDevice *out);

/*
 * What a light entity was configured with, as `export config` needs to print
 * it back. Every field is the value the YAML set, not the value in force: a
 * scan may have supplied a query address the config did not, and printing the
 * derived one would turn a discovered fact into a hand-written line the
 * operator never wrote and cannot re-derive.
 */
struct DaliLightConfig {
  const char *name;             // entity name; never null
  uint8_t     target_type;      // DaliAddressType
  uint8_t     target_address;
  uint8_t     query_address;    // 0xFF when the YAML set none
  uint16_t    member_groups;    // bit N = member of group N
  bool        has_min_level;
  uint8_t     min_level;
  bool        has_max_level;
  uint8_t     max_level;
  bool        has_dimming_curve;
  uint8_t     dimming_curve;    // DaliDimCurve
};

/* As above, for an input sensor entity. */
struct DaliSensorConfig {
  const char *name;             // entity name; never null
  uint8_t     address;
  uint8_t     instance;
  uint32_t    poll_interval_s;
  bool        poll_on_event;
  uint8_t     value_bytes;
  float       scale;
  float       offset;
};

/*
 * Minimal interface used by DaliComponent to update light entity state from
 * the DALI bus (dispatch snooping and QUERY_ACTUAL_LEVEL replies).
 * DaliLightOutput implements this; dali_component.cpp never needs to include
 * the light output header.
 */
class DaliBusLight {
 public:
  virtual ~DaliBusLight() = default;
  // Report the configuration this entity was built from. Read-only, Core 0.
  virtual void describe_config(DaliLightConfig *out) const = 0;
  virtual void    mark_state_from_bus(bool is_on, uint8_t level) = 0;
  virtual void    apply_bus_state() = 0;
  // Collect the scheduler completion for any in-flight command, then admit at
  // most one pending write. Called every loop, scan or not; the implementation
  // gates its own transmission on the scan.
  virtual void    flush_pending_write() = 0;
  virtual uint8_t get_query_address() const = 0;
  // Core 0 only. Logical intent remains owned by the entity while a new
  // profile is acquired; raw levels from older generations cannot be sent.
  virtual void begin_level_profile_update(uint32_t generation) = 0;
  virtual void set_level_profile(const DaliLevelProfile &profile,
                                 uint32_t generation) = 0;
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
  // Report the configuration this entity was built from. Read-only, Core 0.
  virtual void     describe_config(DaliSensorConfig *out) const = 0;
  virtual uint8_t  get_address()         const = 0;
  virtual uint8_t  get_instance()        const = 0;
  virtual uint32_t get_poll_interval_s() const = 0;
  virtual bool     get_poll_on_event()   const = 0;
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

  // Exclusive bus access for a long-running workflow started outside this
  // component — currently a shell session's scan or commissioning walk. Reuses
  // the scan gate rather than adding a second one, so a shell scan pauses the
  // light refresh pump and the diagnostic buttons exactly as a button-initiated
  // scan does, and the two cannot run at once.
  //
  // Returns false when the bus is already claimed. Every true must be matched
  // by one release_bus(). Callable from a worker task.
  bool try_claim_bus(const char *what);
  void release_bus();
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

  // Parse and execute a DALI command line (called from Core 0).
  //
  // Verbs, argument forms, and the named command tables are shared with the
  // native CLI via dali_cli.h; see s_console_commands in the .cpp for the
  // subset this surface implements. Argument counts are checked against that
  // table before a handler runs, so a trailing token is rejected rather than
  // ignored.
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
  // Called by the scan task before on_scan_complete(). Copies only control-gear
  // profile metadata; scan_done_'s release/acquire handoff publishes the copy.
  void set_scan_level_profile_snapshot(const DaliDiscoveryInventory *inventory);
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

  // ── Configuration export (dali_config_export.cpp) ───────────────────────
  //
  // Print the `dali:` block, and the light/sensor platform entries that name
  // it, as the YAML that would rebuild this device. Everything comes from live
  // state — the source YAML is a host-side artefact that never reaches the
  // ESP32 — so what it prints is what the firmware is actually running, which
  // is the more useful of the two when they disagree.
  //
  // `shell` describes the session's own front end, which is configured on
  // DaliShellServer rather than here; null omits the `shell:` sub-block.
  // `inventory` is the last scan, or null when none has run: gear the bus
  // reported but no entity covers is emitted commented out, so a re-export
  // proposes the additions without silently enabling them.
  // `input_lookup` reaches the caller's per-instance input-device detail, the
  // type and resolution an uncovered instance needs to be drafted rather than
  // merely counted; null falls back to reporting instance counts.
  //
  // Runs on the shell task, not the loop task. It only reads Core 0 entity
  // configuration, which is written once during setup() and never after.
  void export_config_yaml(const DaliCliOut *out,
                          const DaliShellConfigInfo *shell,
                          const DaliDiscoveryInventory *inventory,
                          DaliShellInputLookupFn input_lookup);

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

  // Bus fault tracking (Core 0 only; reads volatile g_dali_stats).
  // last_bus_fault_count_ is the cumulative history already reported;
  // bus_fault_recovered_ plus the tx_frames_ok watermark taken when the fault
  // was seen give current availability, which the cumulative counter cannot.
  uint32_t last_bus_fault_count_{0u};
  uint32_t tx_ok_at_bus_fault_{0u};
  bool     bus_fault_recovered_{true};

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
  // Set by release_bus() on a worker task, consumed by loop() on Core 0, which
  // is what keeps refresh_cursor_ single-owner while still letting a shell
  // workflow ask for a re-read when it puts the bus down.
  std::atomic<bool> external_refresh_request_{false};

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
  // Apply the scan task's level-profile snapshot on Core 0.
  void apply_scan_level_profile_snapshot_();
  // Surface a rejected diagnostic-button enqueue; no-op on DALI_OK.
  void report_diag_enqueue_(const char *what, DaliError err);
  // Publish current bus availability alongside its cumulative fault count.
  void update_bus_fault_();

  // Console verb handlers. Split out of execute_command() only for length;
  // each is called with the resolved token list and runs on Core 0.
  void console_queue_(const DaliCliTokens &tokens);
  void console_group_(const DaliCliTokens &tokens);
  void console_raw_(const DaliCliTokens &tokens, bool send_twice, void *ctx);
  void console_special_(const DaliCliTokens &tokens, void *ctx);
  void console_dt6_(const DaliCliTokens &tokens, void *ctx);
  void console_memread_(const DaliCliTokens &tokens, void *ctx);
  void console_devmem_(const DaliCliTokens &tokens, void *ctx);
  void console_iquery_(const DaliCliTokens &tokens, void *ctx);
  void console_iconfig_(const DaliCliTokens &tokens);
  void console_vendor_(const DaliCliTokens &tokens, void *ctx);
  // Log a warning when the scheduler's cumulative rejection counters advance.
  void report_queue_drops_();
};

}  // namespace dali
}  // namespace esphome
