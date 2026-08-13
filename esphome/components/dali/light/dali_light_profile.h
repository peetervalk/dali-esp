#pragma once

/* Home Assistant reserves brightness code 0 for OFF, and its percent slider
 * emits round(255 * pct / 100) — so the lowest code the UI can produce is 3
 * (1%), and codes 1..2 exist only for an explicit `brightness:` service call.
 *
 * Spread codes 3..255 across the complete configured DALI on-range.  MIN LEVEL
 * then sits exactly on the slider's 1%, which keeps the dimmest level the gear
 * can reach selectable and reports it as 1% instead of a 0% that reads as OFF.
 * Pinning MIN to code 1 instead cost the whole span between code 1 and code 3:
 * on an 85..254 window the slider bottomed out at level 106, leaving 85..105
 * addressable only from a service call.  Codes 1..2 clamp onto MIN rather than
 * falling below it.  This header deliberately has no ESPHome types. */

#include <math.h>
#include <stdint.h>

extern "C" {
#include "../../../../components/dali/dali_dim_curve.h"
}

namespace esphome {
namespace dali {

static constexpr float DALI_HA_BRIGHTNESS_MAX = 255.0f;
/* Lowest code Home Assistant's percent slider can emit: round(255 * 1 / 100). */
static constexpr float DALI_HA_MIN_ON_CODE = 3.0f;
static constexpr float DALI_HA_ON_CODES =
    DALI_HA_BRIGHTNESS_MAX - DALI_HA_MIN_ON_CODE;

inline DaliError dali_light_brightness_to_level(const DaliLevelProfile *profile,
                                                float brightness,
                                                uint8_t *level) {
  if (profile == nullptr || level == nullptr || !isfinite(brightness) ||
      brightness <= 0.0f) {
    return DALI_ERR_INVALID;
  }
  float relative = (DALI_HA_BRIGHTNESS_MAX * brightness - DALI_HA_MIN_ON_CODE) /
                   DALI_HA_ON_CODES;
  if (relative < 0.0f) relative = 0.0f;
  if (relative > 1.0f) relative = 1.0f;
  return dali_level_profile_relative_to_level(profile, relative, level);
}

inline DaliError dali_light_level_to_brightness(const DaliLevelProfile *profile,
                                                uint8_t level,
                                                float *brightness) {
  if (profile == nullptr || brightness == nullptr) return DALI_ERR_INVALID;
  float relative = 0.0f;
  DaliError err = dali_level_profile_level_to_relative(profile, level, &relative);
  if (err != DALI_OK) return err;
  *brightness = (DALI_HA_MIN_ON_CODE + DALI_HA_ON_CODES * relative) /
                DALI_HA_BRIGHTNESS_MAX;
  return DALI_OK;
}

/*
 * Decode an observed on-level that need not lie inside the window.
 *
 * The bus legitimately shows levels an entity cannot itself address: gear
 * clamps a DAPC into its own MIN/MAX, an entity may be configured to a
 * narrower window than its gear, and a group member answers for levels the
 * whole group was given.  Reporting the nearest addressable level is the
 * honest reading — the light really is at the end of this entity's range —
 * whereas refusing the value leaves the entity showing something stale.
 *
 * `decoded_level` reports what the brightness actually represents, so a caller
 * can tell a clamped reading from an exact one.  OFF and MASK are still
 * refused: neither names an on-state.
 */
inline DaliError dali_light_observed_level_to_brightness(
    const DaliLevelProfile *profile, uint8_t level, uint8_t *decoded_level,
    float *brightness) {
  if (profile == nullptr || decoded_level == nullptr || brightness == nullptr ||
      dali_level_profile_validate(profile) != DALI_OK || level == 0u ||
      level == DALI_DAPC_MASK_LEVEL) {
    return DALI_ERR_INVALID;
  }

  uint8_t decoded = level;
  if (decoded < profile->min_level) {
    decoded = profile->min_level;
  } else if (decoded > profile->max_level) {
    decoded = profile->max_level;
  }

  DaliError err = dali_light_level_to_brightness(profile, decoded, brightness);
  if (err != DALI_OK) return err;
  *decoded_level = decoded;
  return DALI_OK;
}

}  // namespace dali
}  // namespace esphome
