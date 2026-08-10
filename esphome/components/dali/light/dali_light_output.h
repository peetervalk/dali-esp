#pragma once

#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/light/light_traits.h"
#include "esphome/components/light/color_mode.h"
#include "../dali_component.h"  // provides DaliBusLight + DaliComponent
#include "dali_light_state_mailbox.h"

#include <cstdint>

extern "C" {
#include "../../../../components/dali/dali_control.h"
#include "../../../../components/dali/dali_frame.h"
}

namespace esphome {
namespace dali {

class DaliLightOutput : public light::LightOutput, public DaliBusLight {
 public:
  // Called from __init__.py codegen — registers self with the bus component.
  void set_dali_component(DaliComponent *comp);

  void set_target(uint8_t type, uint8_t address) {
    target_.type    = static_cast<DaliAddressType>(type);
    target_.address = address;
  }

  // Optional short address to use for QUERY_ACTUAL_LEVEL (boot/refresh/poll).
  // 0xFF = no query (default).  Must be a short address even if target is a group.
  void    set_query_address(uint8_t addr) { query_address_ = addr; }
  uint8_t get_query_address() const override { return query_address_; }

  // Bitmask of DALI groups this entity belongs to (bit N = group N).
  // Used by notify_lights to propagate group-addressed dispatch results to
  // short-address entities.  Defaults to 0 (no group membership tracked).
  void set_member_groups(uint16_t groups) { member_groups_ = groups; }

  light::LightTraits get_traits() override {
    auto t = light::LightTraits();
    t.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return t;
  }

  void setup_state(light::LightState *state) override;
  void write_state(light::LightState *state) override;

  // DaliBusLight interface — Core 1 writes, Core 0 drains.
  void mark_state_from_bus(bool is_on, uint8_t level) override;
  void apply_bus_state() override;

 protected:
  DaliTarget      target_{};
  DaliComponent  *comp_{nullptr};
  uint8_t         query_address_{0xFFu};
  uint16_t        member_groups_{0};

  // Coherent latest bus state — published on Core 1, drained on Core 0.
  DaliLightStateMailbox bus_state_mailbox_;

  // Core 0 only — prevents re-issuing a DALI command when state is pushed
  // from the bus into ESPHome via LightCall::perform().
  uint32_t          setup_ms_{0};
  bool              known_state_valid_{false};
  bool              known_is_on_{false};
  uint8_t           known_level_{0};
  bool              suppress_initial_write_{true};
  bool              skip_next_write_{false};
  light::LightState *state_{nullptr};

  static void clear_unused_color_fields_(light::LightColorValues &values);
  static uint8_t brightness_to_level_(float brightness);
};

}  // namespace dali
}  // namespace esphome
