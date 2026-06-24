#include "dali_light_output.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace dali {

static const char *TAG = "dali.light";

void DaliLightOutput::set_dali_component(DaliComponent *comp) {
  comp_ = comp;
  comp->register_light((uint8_t)target_.type, target_.address, this);
}

void DaliLightOutput::write_state(light::LightState *state) {
  if (!state_) state_ = state;  // capture on first call

  if (skip_next_write_) {
    skip_next_write_ = false;
    return;  // state was pushed from bus — don't re-issue DALI command
  }

  float brightness = 0.0f;
  state->current_values_as_brightness(&brightness);

  if (!state->current_values.is_on() || brightness < (1.0f / 254.0f)) {
    dali_control_off(target_);
    return;
  }

  uint8_t level = static_cast<uint8_t>(roundf(brightness * 254.0f));
  if (level == 0) level = 1;

  DaliError err = dali_control_set_level(target_, level);
  if (err != DALI_OK) {
    ESP_LOGW(TAG, "set_level failed: %d", (int)err);
  }
}

void DaliLightOutput::mark_state_from_bus(bool is_on, uint8_t level) {
  bus_is_on_.store(is_on, std::memory_order_relaxed);
  bus_level_.store(level, std::memory_order_relaxed);
  bus_dirty_.store(true, std::memory_order_release);  // release: ensures is_on/level visible before dirty
}

void DaliLightOutput::apply_bus_state() {
  if (!bus_dirty_.load(std::memory_order_acquire) || !state_) return;

  bool    is_on = bus_is_on_.load(std::memory_order_relaxed);
  uint8_t level = bus_level_.load(std::memory_order_relaxed);
  bus_dirty_.store(false, std::memory_order_relaxed);

  skip_next_write_ = true;
  auto call = state_->make_call();
  call.set_state(is_on);
  if (is_on) call.set_brightness(static_cast<float>(level) / 254.0f);
  call.set_transition_length(0);
  call.perform();
}

}  // namespace dali
}  // namespace esphome
