#pragma once

#include "esphome/components/button/button.h"
#include "../dali_component.h"

namespace esphome {
namespace dali {

class DaliScanButton : public button::Button {
 public:
  void set_dali_component(DaliComponent *c) { component_ = c; }

 protected:
  void press_action() override { component_->start_scan(); }

  DaliComponent *component_{nullptr};
};

}  // namespace dali
}  // namespace esphome
