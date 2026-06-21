#include "dali_light_output.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace dali {

static const char *TAG = "dali.light";

void DaliLightOutput::write_state(light::LightState *state) {
  float brightness = 0.0f;
  state->current_values_as_brightness(&brightness);

  if (!state->current_values.is_on() || brightness < (1.0f / 254.0f)) {
    dali_control_off(target_);
    return;
  }

  // DALI arc power levels: 1–254. Level 0 = off (use the OFF command instead).
  uint8_t level = static_cast<uint8_t>(roundf(brightness * 254.0f));
  if (level == 0) level = 1;

  DaliError err = dali_control_set_level(target_, level);
  if (err != DALI_OK) {
    ESP_LOGW(TAG, "set_level failed: %d", (int)err);
  }
}

}  // namespace dali
}  // namespace esphome
