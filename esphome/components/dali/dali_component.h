#pragma once

#include "esphome/core/component.h"

#include <atomic>
#include <cstdint>

// Forward declarations — full includes are in the .cpp files.
namespace esphome { namespace text_sensor { class TextSensor; } }

namespace esphome {
namespace dali {

class DaliLightOutput;  // defined in light/dali_light_output.h

class DaliComponent : public Component {
 public:
  void set_pins(uint8_t tx_pin, uint8_t rx_pin) {
    tx_pin_ = tx_pin;
    rx_pin_ = rx_pin;
  }

  void set_scan_status_sensor(text_sensor::TextSensor *s) { scan_status_ = s; }

  // Periodic state refresh interval in seconds (0 = disabled).
  void set_poll_interval(uint32_t seconds) { poll_interval_s_ = seconds; }

  void setup() override;
  void loop() override;

  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // Called from the ESPHome loop (button press handler).
  void start_scan();
  // Query ACTUAL_LEVEL for all registered lights that have a query_address.
  void start_refresh();

  // Called from the scan task when it finishes (Core 1 → loop picks it up).
  void on_scan_complete(uint8_t count, bool success);

  // Called by DaliLightOutput::set_dali_component() during codegen init.
  // target_type/address match DaliAddressType enum (uint8_t).
  void register_light(uint8_t target_type, uint8_t target_address, DaliLightOutput *light);

 protected:
  uint8_t tx_pin_{18};
  uint8_t rx_pin_{19};

  text_sensor::TextSensor *scan_status_{nullptr};

  // Scan state (Core 1 writes, Core 0 reads via atomic gate).
  std::atomic<bool>    scan_done_{false};
  std::atomic<bool>    scan_running_{false};
  std::atomic<uint8_t> scan_count_{0};
  std::atomic<bool>    scan_success_{false};

  // Periodic poll (Core 0 only).
  uint32_t poll_interval_s_{0};
  uint32_t last_poll_ms_{0};
  bool     boot_query_done_{false};
};

}  // namespace dali
}  // namespace esphome
