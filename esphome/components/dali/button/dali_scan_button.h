#pragma once

#include "esphome/components/button/button.h"
#include "../dali_component.h"

namespace esphome {
namespace dali {

enum DaliButtonType : uint8_t {
  DALI_BUTTON_SCAN = 0,
  DALI_BUTTON_REFRESH,
  DALI_BUTTON_IDENTIFY,
  DALI_BUTTON_FIND_COUPLERS,
  DALI_BUTTON_TURN_ON,
  DALI_BUTTON_TURN_OFF,
  DALI_BUTTON_MAX,
  DALI_BUTTON_MIN,
};

class DaliScanButton : public button::Button {
 public:
  void set_dali_component(DaliComponent *c) { component_ = c; }
  void set_button_type(uint8_t type) { type_ = static_cast<DaliButtonType>(type); }

 protected:
  void press_action() override {
    if (component_ == nullptr) return;
    switch (type_) {
      case DALI_BUTTON_SCAN:
        component_->start_scan();
        break;
      case DALI_BUTTON_REFRESH:
        component_->start_refresh();
        break;
      case DALI_BUTTON_IDENTIFY:
        component_->start_identify();
        break;
      case DALI_BUTTON_FIND_COUPLERS:
        component_->start_find_couplers();
        break;
      case DALI_BUTTON_TURN_ON:
        component_->send_diag_on();
        break;
      case DALI_BUTTON_TURN_OFF:
        component_->send_diag_off();
        break;
      case DALI_BUTTON_MAX:
        component_->send_diag_max();
        break;
      case DALI_BUTTON_MIN:
        component_->send_diag_min();
        break;
    }
  }

  DaliComponent *component_{nullptr};
  DaliButtonType type_{DALI_BUTTON_SCAN};
};

}  // namespace dali
}  // namespace esphome
