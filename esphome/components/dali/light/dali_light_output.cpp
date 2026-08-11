#include "dali_light_output.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace dali {

static const char *TAG = "dali.light";

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
  return dali_dim_curve_output_to_level(brightness);
}

float DaliLightOutput::level_to_brightness_(uint8_t level) {
  return dali_dim_curve_level_to_output(level);
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
  suppress_initial_write_ = true;
}

/*
 * Which writes are ours to send.
 *
 * Two kinds of write_state() call are not an operator's intent, and both are
 * identified by what they are rather than by when they arrive:
 *
 *   1. ESPHome's restore/default write. LightState::setup() calls
 *      setup_state() and then performs exactly one call carrying the restored
 *      or default state, so the first write_state() after setup is that one.
 *      Sending it would drive the bus to a remembered level before anything has
 *      looked at what the gear is actually doing.
 *
 *   2. The echo of a bus reading. apply_bus_state() pushes an observed level
 *      into ESPHome via LightCall::perform(), which schedules a write of the
 *      value we just took off the bus. Re-sending it would be a command the
 *      operator never issued.
 *
 * The echo is matched on the exact (is_on, level) pair that was pushed, not on
 * a "skip the next one" flag: a flag is consumed by whichever call happens to
 * arrive first, so a user command racing an observation was dropped, and an
 * observation that produced no write left the flag armed to eat a later real
 * command. Matching on the value cannot do either — and a user command that
 * genuinely equals the current bus state is a no-op the write arbiter already
 * suppresses.
 *
 * There is deliberately no time window. The previous ten-second startup
 * suppression discarded every command in the first ten seconds after boot,
 * including ones a person had just issued.
 */
void DaliLightOutput::write_state(light::LightState *state) {
  if (!state_) state_ = state;  // capture on first call
  clear_unused_color_fields_(state->remote_values);
  clear_unused_color_fields_(state->current_values);

  const auto &target = state->remote_values;
  float brightness = target.get_state() * target.get_brightness();
  /* Any non-zero brightness is on. The dimmest arc power level still emits
   * 0.1 %, well under the 1/254 a linear reading treated as the floor, and OFF
   * is a separate command rather than the bottom of the curve. */
  bool desired_is_on = target.is_on() && brightness > 0.0f;
  uint8_t desired_level = desired_is_on ? brightness_to_level_(brightness) : 0u;

  if (suppress_initial_write_) {
    suppress_initial_write_ = false;
    echo_valid_ = false;
    ESP_LOGD(TAG, "suppressing initial restore/default write to %s %u",
             target_type_name(target_.type), (unsigned)target_.address);
    return;
  }

  if (echo_valid_ && echo_is_on_ == desired_is_on &&
      (!desired_is_on || echo_level_ == desired_level)) {
    echo_valid_ = false;
    return;  // state came from the bus — do not send it back
  }
  echo_valid_ = false;

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

  /* A real reading also settles what the gear is doing, so the restore write is
   * no longer pending arbitration — disarm it here as well as on first use, so
   * a boot where ESPHome never issued one cannot leave the flag to swallow a
   * later operator command. */
  suppress_initial_write_ = false;

  /* What LightCall::perform() below will schedule as a write. Recorded so that
   * write_state() can recognise it as this reading coming back rather than as
   * something a person asked for.
   *
   * The level survives the round trip unchanged: this pushes the level's light
   * output as the brightness and write_state() converts it back through the
   * inverse curve, which test_dim_curve asserts for all 254 levels. A level
   * that did not come back intact would be mistaken for an operator command
   * and sent to the bus. remote_values carries the un-gamma-corrected value,
   * so nothing else is applied in between. */
  echo_valid_ = true;
  echo_is_on_ = is_on;
  echo_level_ = is_on ? level : 0u;

  auto call = state_->make_call();
  call.set_state(is_on);
  if (is_on) call.set_brightness(level_to_brightness_(level));
  call.set_transition_length(0);
  clear_unused_color_fields_(state_->remote_values);
  clear_unused_color_fields_(state_->current_values);
  call.perform();
}

}  // namespace dali
}  // namespace esphome
