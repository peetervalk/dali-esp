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

void DaliLightOutput::on_command_complete_(DaliError result,
                                           const DaliFrame * /*reply*/,
                                           void *ctx) {
  // Runs on the DALI task. Hand the outcome over; Core 0 owns write_.
  static_cast<DaliLightOutput *>(ctx)->command_mailbox_.publish(result == DALI_OK);
}

void DaliLightOutput::pump_write_() {
  /* Collect the completion first: an enqueue returning DALI_OK only means the
   * frame was queued, so the confirmed state this light deduplicates against
   * may only be committed here. A failed transmission invalidates it instead,
   * which is what lets the identical command be retried rather than suppressed. */
  bool success = true;
  uint32_t completions = 0u;
  if (command_mailbox_.take(success, completions)) {
    if (dali_light_write_confirm(&write_, success)) {
      ESP_LOGW(TAG, "tx to %s %u failed; state unknown, retrying",
               target_type_name(target_.type), (unsigned)target_.address);
    } else if (!success) {
      ESP_LOGW(TAG, "tx to %s %u failed; retry budget spent, state unknown",
               target_type_name(target_.type), (unsigned)target_.address);
    }
  }

  if (comp_ != nullptr && comp_->is_scan_running()) return;

  uint8_t level = 0u;
  DaliError err;
  switch (dali_light_write_next(&write_, &level)) {
    case DALI_LIGHT_WRITE_SEND_OFF:
      ESP_LOGD(TAG, "tx off to %s %u",
               target_type_name(target_.type), (unsigned)target_.address);
      err = dali_control_off_cb(target_, on_command_complete_, this);
      break;

    case DALI_LIGHT_WRITE_SEND_LEVEL:
      ESP_LOGD(TAG, "tx level %u to %s %u", (unsigned)level,
               target_type_name(target_.type), (unsigned)target_.address);
      err = dali_control_set_level_cb(target_, level, on_command_complete_, this);
      break;

    case DALI_LIGHT_WRITE_SUPPRESS:
    case DALI_LIGHT_WRITE_IDLE:
    default:
      return;
  }

  /* Queue pressure is transient, so a rejected command stays pending and is
   * retried on a later loop rather than dropped. */
  if (err != DALI_OK) {
    ESP_LOGW(TAG, "enqueue to %s %u failed: %d; retrying",
             target_type_name(target_.type), (unsigned)target_.address, (int)err);
  }
  dali_light_write_sent(&write_, err == DALI_OK);
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

  /* Record the desired state itself rather than relying on LightState: a
   * bus-state publication can overwrite remote_values before this is sent. */
  dali_light_write_request(&write_, desired_is_on, desired_level);

  if (comp_ != nullptr && comp_->is_scan_running()) {
    ESP_LOGD(TAG, "deferring write to %s %u while scan is active",
             target_type_name(target_.type), (unsigned)target_.address);
    return;
  }

  pump_write_();
}

void DaliLightOutput::flush_pending_write() {
  /* Called every loop, so this is also where an in-flight command's completion
   * is collected — including after a scan, when nothing new is pending. */
  if (state_ == nullptr) return;
  pump_write_();
}

void DaliLightOutput::mark_state_from_bus(bool is_on, uint8_t level) {
  bus_state_mailbox_.publish(is_on, level);
}

void DaliLightOutput::apply_bus_state() {
  if (!state_) return;

  bool is_on;
  uint8_t level;
  if (!bus_state_mailbox_.take(is_on, level)) return;

  /* Refused while a command is in flight: that command's own completion is the
   * authority for the state it establishes, and this reading may predate it. */
  dali_light_write_observe(&write_, is_on, level);

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
