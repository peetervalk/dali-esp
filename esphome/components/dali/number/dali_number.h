#pragma once

#include "esphome/components/number/number.h"
#include "../dali_component.h"

namespace esphome {
namespace dali {

class DaliAddressNumber : public number::Number {
 public:
  void set_dali_component(DaliComponent *c) { component_ = c; }

 protected:
  void control(float value) override {
    publish_state(value);
    component_->set_diag_address(static_cast<uint8_t>(value));
  }

  DaliComponent *component_{nullptr};
};

}  // namespace dali
}  // namespace esphome
