#include "dali_light_output.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace dali {

static const char *TAG = "dali.light";
static constexpr uint32_t STARTUP_WRITE_SUPPRESS_MS = 10000u;

// Only referenced from ESP_LOGD; unused when the build's log level is above DEBUG.
[[maybe_unused]] static const char *target_type_name(DaliAddressType type) {
  switch (type) {
    case DALI_ADDR_SHORT: return "short";
    case DALI_ADDR_GROUP: return "group";
    case DALI_ADDR_BROADCAST: return "broadcast";
    default: return "unknown";
  }
}

void DaliLightOutput::set_dali_component(DaliComponent *comp) {
  comp_ = comp;
  comp->register_light((uint8_t)target_.type, target_.address, member_groups_, this);
}

void DaliLightOutput::clear_unused_color_fields_(light::LightColorValues &values) {
  values.set_red(0.0f);
  values.set_green(0.0f);
  values.set_blue(0.0f);
  values.set_white(0.0f);
  values.set_color_temperature(0.0f);
  values.set_cold_white(0.0f);
  values.set_warm_white(0.0f);
}

uint8_t DaliLightOutput::brightness_to_level_(float brightness) {
  uint8_t level = static_cast<uint8_t>(roundf(brightness * 254.0f));
  return level == 0 ? 1 : level;
}

DaliError DaliLightOutput::enqueue_desired_state_(bool is_on, uint8_t level) {
  if (!is_on) {
    if (known_state_valid_ && !known_is_on_) return DALI_OK;
    ESP_LOGD(TAG, "tx off to %s %u",
             target_type_name(target_.type), (unsigned)target_.address);
    DaliError err = dali_control_off(target_);
    if (err != DALI_OK) {
      ESP_LOGW(TAG, "off failed: %d", (int)err);
      return err;
    }
    known_state_valid_ = true;
    known_is_on_ = false;
    known_level_ = 0u;
    return DALI_OK;
  }

  if (known_state_valid_ && known_is_on_ && known_level_ == level) return DALI_OK;
  ESP_LOGD(TAG, "tx level %u to %s %u",
           (unsigned)level, target_type_name(target_.type), (unsigned)target_.address);
  DaliError err = dali_control_set_level(target_, level);
  if (err != DALI_OK) {
    ESP_LOGW(TAG, "set_level failed: %d", (int)err);
    return err;
  }
  known_state_valid_ = true;
  known_is_on_ = true;
  known_level_ = level;
  return DALI_OK;
}

void DaliLightOutput::setup_state(light::LightState *state) {
  state_ = state;
  state->set_default_transition_length(0);
  clear_unused_color_fields_(state->remote_values);
  clear_unused_color_fields_(state->current_values);
  setup_ms_ = millis();
  suppress_initial_write_ = true;
}

void DaliLightOutput::write_state(light::LightState *state) {
  if (!state_) state_ = state;  // capture on first call
  clear_unused_color_fields_(state->remote_values);
  clear_unused_color_fields_(state->current_values);

  if (suppress_initial_write_) {
    suppress_initial_write_ = false;
    skip_next_write_ = false;
    ESP_LOGD(TAG, "suppressing initial restore/default write");
    return;
  }

  if (skip_next_write_) {
    skip_next_write_ = false;
    return;  // state was pushed from bus — don't re-issue DALI command
  }

  if ((uint32_t)(millis() - setup_ms_) < STARTUP_WRITE_SUPPRESS_MS) {
    ESP_LOGD(TAG, "suppressing startup write to %s %u",
             target_type_name(target_.type), (unsigned)target_.address);
    return;
  }

  const auto &target = state->remote_values;
  float brightness = target.get_state() * target.get_brightness();
  bool desired_is_on = target.is_on() && brightness >= (1.0f / 254.0f);
  uint8_t desired_level = desired_is_on ? brightness_to_level_(brightness) : 0u;

  if (comp_ != nullptr && comp_->is_scan_running()) {
    /* Preserve the latest desired target itself: a bus-state publication can
     * update LightState::remote_values before scan admission reopens. */
    scan_pending_is_on_ = desired_is_on;
    scan_pending_level_ = desired_level;
    scan_write_pending_ = true;
    ESP_LOGD(TAG, "deferring write to %s %u while scan is active",
             target_type_name(target_.type), (unsigned)target_.address);
    return;
  }

  (void) enqueue_desired_state_(desired_is_on, desired_level);
}

void DaliLightOutput::flush_pending_write() {
  if (!scan_write_pending_ || state_ == nullptr ||
      (comp_ != nullptr && comp_->is_scan_running())) {
    return;
  }
  /* Queue pressure is transient. Retain the packed desired state until the
   * scheduler accepts it; this also lets more lights drain than queue slots. */
  if (enqueue_desired_state_(scan_pending_is_on_, scan_pending_level_) == DALI_OK) {
    scan_write_pending_ = false;
  }
}

void DaliLightOutput::mark_state_from_bus(bool is_on, uint8_t level) {
  bus_state_mailbox_.publish(is_on, level);
}

void DaliLightOutput::apply_bus_state() {
  if (!state_) return;

  bool is_on;
  uint8_t level;
  if (!bus_state_mailbox_.take(is_on, level)) return;

  known_state_valid_ = true;
  known_is_on_ = is_on;
  known_level_ = level;

  skip_next_write_ = true;
  auto call = state_->make_call();
  call.set_state(is_on);
  if (is_on) call.set_brightness(static_cast<float>(level) / 254.0f);
  call.set_transition_length(0);
  clear_unused_color_fields_(state_->remote_values);
  clear_unused_color_fields_(state_->current_values);
  call.perform();
}

}  // namespace dali
}  // namespace esphome
