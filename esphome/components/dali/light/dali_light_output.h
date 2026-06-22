#pragma once

#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/light/light_traits.h"
#include "esphome/components/light/color_mode.h"

extern "C" {
#include "../../../../components/dali/dali_control.h"
#include "../../../../components/dali/dali_frame.h"
}

namespace esphome {
namespace dali {

class DaliLightOutput : public light::LightOutput {
 public:
  // Called from __init__.py codegen
  void set_target(uint8_t type, uint8_t address) {
    target_.type = static_cast<DaliAddressType>(type);
    target_.address = address;
  }

  light::LightTraits get_traits() override {
    auto t = light::LightTraits();
    t.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return t;
  }

  void write_state(light::LightState *state) override;

 protected:
  DaliTarget target_{};
};

}  // namespace dali
}  // namespace esphome
