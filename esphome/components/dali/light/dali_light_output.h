#pragma once

#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/light/light_traits.h"
#include "esphome/components/light/color_mode.h"

#include <atomic>
#include <cstdint>

extern "C" {
#include "../../../../components/dali/dali_control.h"
#include "../../../../components/dali/dali_frame.h"
}

namespace esphome {
namespace dali {

class DaliComponent;  // forward — full include in dali_light_output.cpp

class DaliLightOutput : public light::LightOutput {
 public:
  // Called from __init__.py codegen — registers self with the bus component.
  void set_dali_component(DaliComponent *comp);

  void set_target(uint8_t type, uint8_t address) {
    target_.type    = static_cast<DaliAddressType>(type);
    target_.address = address;
  }

  // Optional short address to use for QUERY_ACTUAL_LEVEL (boot/refresh/poll).
  // 0xFF = no query (default).  Must be a short address even if target is a group.
  void set_query_address(uint8_t addr) { query_address_ = addr; }
  uint8_t get_query_address() const    { return query_address_; }

  light::LightTraits get_traits() override {
    auto t = light::LightTraits();
    t.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return t;
  }

  void write_state(light::LightState *state) override;

  // Called from Core 1 (DALI task) after a bus command is dispatched or a
  // query reply is received.  Sets atomic dirty flag; Core 0 loop() drains it.
  void mark_state_from_bus(bool is_on, uint8_t level);

  // Called from Core 0 (DaliComponent::loop()).  Publishes pending bus state.
  void apply_bus_state();

 protected:
  DaliTarget      target_{};
  DaliComponent  *comp_{nullptr};
  uint8_t         query_address_{0xFFu};

  // Bus state — written on Core 1, read on Core 0.
  std::atomic<bool>    bus_dirty_{false};
  std::atomic<bool>    bus_is_on_{false};
  std::atomic<uint8_t> bus_level_{0};

  // Core 0 only — prevents re-issuing a DALI command when state is pushed
  // from the bus into ESPHome via LightCall::perform().
  bool              skip_next_write_{false};
  light::LightState *state_{nullptr};
};

}  // namespace dali
}  // namespace esphome
