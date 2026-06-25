#pragma once

#include "esphome/core/component.h"
#include "esphome/components/text/text.h"
#include "../dali_component.h"

namespace esphome {
namespace dali {

class DaliCommandText : public text::Text, public Component {
 public:
  void set_dali_component(DaliComponent *parent) { parent_ = parent; }

  float get_setup_priority() const override { return setup_priority::HARDWARE - 10.0f; }

 protected:
  void control(const std::string &value) override {
    parent_->execute_command(value);
    publish_state(value);  // reflect what was typed back into the entity
  }

  DaliComponent *parent_{nullptr};
};

}  // namespace dali
}  // namespace esphome
