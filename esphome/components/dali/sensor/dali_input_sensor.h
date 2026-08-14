#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "../dali_component.h"
#include "dali_input_value_mailbox.h"

#include <cstdint>

namespace esphome {
namespace dali {

/*
 * ESPHome sensor entity for a single DALI-2 input device instance.
 * Polling and value transfer are driven by DaliComponent; this class
 * only holds the configuration and the Core 1 → Core 0 value mailbox.
 */
class DaliInputSensor : public sensor::Sensor, public Component, public DaliBusSensor {
 public:
  void set_address(uint8_t a)        { address_        = a; }
  void set_instance(uint8_t i)       { instance_       = i; }
  void set_poll_interval_s(uint32_t s) { poll_interval_s_ = s; }
  void set_poll_on_event(bool enabled) { poll_on_event_   = enabled; }
  void set_value_bytes(uint8_t b)    { value_bytes_    = b; }
  void set_scale(float s)            { scale_          = s; }
  void set_offset(float o)           { offset_         = o; }

  uint8_t  get_address()          const { return address_; }
  uint8_t  get_instance()         const { return instance_; }
  uint32_t get_poll_interval_s()  const { return poll_interval_s_; }
  bool     get_poll_on_event()    const { return poll_on_event_; }
  uint8_t  get_value_bytes()      const { return value_bytes_; }
  uint32_t get_last_poll_ms()     const { return last_poll_ms_; }
  void     set_last_poll_ms(uint32_t ms) { last_poll_ms_ = ms; }

  void describe_config(DaliSensorConfig *out) const override {
    if (out == nullptr) return;
    out->name            = this->get_name().c_str();
    out->address         = address_;
    out->instance        = instance_;
    out->poll_interval_s = poll_interval_s_;
    out->poll_on_event   = poll_on_event_;
    out->value_bytes     = value_bytes_;
    out->scale           = scale_;
    out->offset          = offset_;
  }

  /* Called from Core 1 (DALI task completion callback). */
  void mark_raw_value(uint16_t raw) { value_mailbox_.publish(raw); }

  /* Called from Core 0 (DaliComponent::loop). Publishes if a value is waiting. */
  void apply_value() {
    uint16_t raw;
    if (!value_mailbox_.take(raw)) return;
    publish_state((float)raw * scale_ + offset_);
  }

  float get_setup_priority() const override { return setup_priority::HARDWARE - 10.0f; }

 protected:
  uint8_t  address_{0};
  uint8_t  instance_{0};
  uint32_t poll_interval_s_{30};
  bool     poll_on_event_{true};
  uint8_t  value_bytes_{1};
  float    scale_{1.0f};
  float    offset_{0.0f};

  uint32_t last_poll_ms_{0};

  /* Coherent latest reading — published on Core 1, drained on Core 0. */
  DaliInputValueMailbox value_mailbox_;
};

}  // namespace dali
}  // namespace esphome
