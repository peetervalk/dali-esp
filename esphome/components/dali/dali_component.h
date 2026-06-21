#pragma once

#include "esphome/core/component.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <atomic>

extern "C" {
#include "dali_phy.h"
#include "dali_scheduler.h"
}

namespace esphome {
namespace dali {

class DaliComponent : public Component {
 public:
  void set_pins(uint8_t tx_pin, uint8_t rx_pin) {
    tx_pin_ = tx_pin;
    rx_pin_ = rx_pin;
  }

  // Optional text sensor that shows scan status in HA ("Idle" / "Scanning..." / "Found N devices")
  void set_scan_status_sensor(text_sensor::TextSensor *s) { scan_status_ = s; }

  void setup() override;
  void loop() override;

  // Run before Wi-Fi / network components so the DALI task is up before
  // any entity tries to issue a command.
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // Called from the ESPHome loop (button press handler).
  void start_scan();

  // Called from the scan task when it finishes (Core 1 → loop picks it up).
  void on_scan_complete(uint8_t count, bool success);

 protected:
  uint8_t tx_pin_{18};
  uint8_t rx_pin_{19};

  text_sensor::TextSensor *scan_status_{nullptr};

  std::atomic<bool>    scan_done_{false};
  std::atomic<bool>    scan_running_{false};
  uint8_t              scan_count_{0};
  bool                 scan_success_{false};
};

}  // namespace dali
}  // namespace esphome
