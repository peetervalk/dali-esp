#include "unity.h"

#include "../components/dali/dali_light_write.h"
#include "../esphome/components/dali/light/dali_light_profile.h"

using esphome::dali::dali_light_brightness_to_level;
using esphome::dali::dali_light_level_to_brightness;
using esphome::dali::dali_light_observed_level_to_brightness;

void setUp(void) {}
void tearDown(void) {}

static float ha_brightness(uint8_t code) {
  return static_cast<float>(code) / 255.0f;
}

/* Code 1 reaches MIN by clamping below the on-code floor; see
 * test_min_level_sits_on_the_lowest_ui_reachable_code for the floor itself. */
void test_ha_endpoints_cover_every_valid_profile(void) {
  for (int curve = DALI_DIM_CURVE_STANDARD; curve <= DALI_DIM_CURVE_LINEAR; curve++) {
    for (uint16_t min_level = 1u; min_level <= DALI_DAPC_MAX_LEVEL; min_level++) {
      for (uint16_t max_level = min_level; max_level <= DALI_DAPC_MAX_LEVEL;
           max_level++) {
        DaliLevelProfile profile = {
            static_cast<DaliDimCurve>(curve),
            static_cast<uint8_t>(min_level),
            static_cast<uint8_t>(max_level),
        };
        uint8_t level = 0u;

        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_light_brightness_to_level(&profile,
                                                         ha_brightness(1u), &level));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(min_level), level);
        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_light_brightness_to_level(&profile,
                                                         ha_brightness(255u), &level));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(max_level), level);
      }
    }
  }
}

void test_min_level_sits_on_the_lowest_ui_reachable_code(void) {
  /* Home Assistant's percent slider emits round(255 * pct / 100), so its
   * lowest output is code 3 and codes 1..2 cannot be selected from it. MIN
   * LEVEL therefore has to sit on code 3: pinned to code 1 it renders as 0% —
   * indistinguishable from OFF — and the slider can never return to it. */
  for (int curve = DALI_DIM_CURVE_STANDARD; curve <= DALI_DIM_CURVE_LINEAR; curve++) {
    for (uint16_t min_level = 1u; min_level < DALI_DAPC_MAX_LEVEL; min_level++) {
      for (uint16_t max_level = min_level + 1u; max_level <= DALI_DAPC_MAX_LEVEL;
           max_level++) {
        DaliLevelProfile profile = {
            static_cast<DaliDimCurve>(curve),
            static_cast<uint8_t>(min_level),
            static_cast<uint8_t>(max_level),
        };
        uint8_t level = 0u;
        float brightness = 0.0f;

        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_light_brightness_to_level(&profile,
                                                         ha_brightness(3u), &level));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(min_level), level);

        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_light_level_to_brightness(
                              &profile, static_cast<uint8_t>(min_level),
                              &brightness));
        TEST_ASSERT_EQUAL_FLOAT(ha_brightness(3u), brightness);
      }
    }
  }
}

void test_codes_below_the_on_code_floor_clamp_onto_min(void) {
  /* Codes 1 and 2 survive only in an explicit `brightness:` service call. They
   * must stay ON at the floor rather than dropping below MIN or being refused —
   * a stored scene carrying one of them still has to replay as light. */
  for (int curve = DALI_DIM_CURVE_STANDARD; curve <= DALI_DIM_CURVE_LINEAR; curve++) {
    for (uint16_t min_level = 1u; min_level <= DALI_DAPC_MAX_LEVEL; min_level++) {
      for (uint16_t max_level = min_level; max_level <= DALI_DAPC_MAX_LEVEL;
           max_level++) {
        DaliLevelProfile profile = {
            static_cast<DaliDimCurve>(curve),
            static_cast<uint8_t>(min_level),
            static_cast<uint8_t>(max_level),
        };
        for (uint8_t code = 1u; code <= 2u; code++) {
          uint8_t level = 0u;
          TEST_ASSERT_EQUAL(DALI_OK,
                            dali_light_brightness_to_level(&profile,
                                                           ha_brightness(code),
                                                           &level));
          TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(min_level), level);
          TEST_ASSERT_TRUE(level != 0u);
        }
      }
    }
  }
}

void test_ui_floor_reaches_min_on_the_bus_2_gear_window(void) {
  /* The gear on bus 2 reports MIN LEVEL 85 (1% output) and MAX LEVEL 254 on the
   * standard curve. With the floor at code 1 the slider's 1% landed on level
   * 106 and levels 85..105 were unreachable from the UI. */
  DaliLevelProfile profile = {DALI_DIM_CURVE_STANDARD, 85u, 254u};
  uint8_t level = 0u;
  float brightness = 0.0f;

  TEST_ASSERT_EQUAL(DALI_OK,
                    dali_light_brightness_to_level(&profile, ha_brightness(3u),
                                                   &level));
  TEST_ASSERT_EQUAL_UINT8(85u, level);

  TEST_ASSERT_EQUAL(DALI_OK,
                    dali_light_level_to_brightness(&profile, 85u, &brightness));
  /* Quantized back to the byte HA publishes: 3, which the frontend renders as
   * round(3 * 100 / 255) = 1%. Code 1 rendered as 0%. */
  TEST_ASSERT_EQUAL_INT(3, static_cast<int>(lroundf(brightness * 255.0f)));

  TEST_ASSERT_EQUAL(DALI_OK,
                    dali_light_brightness_to_level(&profile, ha_brightness(255u),
                                                   &level));
  TEST_ASSERT_EQUAL_UINT8(254u, level);
}

void test_ha_on_codes_always_map_to_a_legal_profile_level(void) {
  for (int curve = DALI_DIM_CURVE_STANDARD; curve <= DALI_DIM_CURVE_LINEAR; curve++) {
    for (uint16_t min_level = 1u; min_level <= DALI_DAPC_MAX_LEVEL; min_level++) {
      for (uint16_t max_level = min_level; max_level <= DALI_DAPC_MAX_LEVEL;
           max_level++) {
        DaliLevelProfile profile = {
            static_cast<DaliDimCurve>(curve),
            static_cast<uint8_t>(min_level),
            static_cast<uint8_t>(max_level),
        };
        for (uint16_t code = 1u; code <= 255u; code++) {
          uint8_t level = 0u;
          TEST_ASSERT_EQUAL(DALI_OK,
                            dali_light_brightness_to_level(
                                &profile, ha_brightness(static_cast<uint8_t>(code)),
                                &level));
          TEST_ASSERT_TRUE(level >= profile.min_level);
          TEST_ASSERT_TRUE(level <= profile.max_level);
          TEST_ASSERT_TRUE(level != 0u);
          TEST_ASSERT_TRUE(level != DALI_DAPC_MASK_LEVEL);
        }
      }
    }
  }
}

void test_level_to_ha_to_level_is_canonical_for_every_profile_level(void) {
  for (int curve = DALI_DIM_CURVE_STANDARD; curve <= DALI_DIM_CURVE_LINEAR; curve++) {
    for (uint16_t min_level = 1u; min_level <= DALI_DAPC_MAX_LEVEL; min_level++) {
      for (uint16_t max_level = min_level; max_level <= DALI_DAPC_MAX_LEVEL;
           max_level++) {
        DaliLevelProfile profile = {
            static_cast<DaliDimCurve>(curve),
            static_cast<uint8_t>(min_level),
            static_cast<uint8_t>(max_level),
        };
           for (uint16_t source_level = min_level; source_level <= max_level;
             source_level++) {
          float brightness = 0.0f;
          uint8_t canonical_level = 0u;
          TEST_ASSERT_EQUAL(DALI_OK,
                            dali_light_level_to_brightness(
                                &profile, static_cast<uint8_t>(source_level),
                                &brightness));
          TEST_ASSERT_EQUAL(DALI_OK,
                            dali_light_brightness_to_level(&profile, brightness,
                                                           &canonical_level));
          TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(source_level),
                                  canonical_level);
        }
      }
    }
  }
}

void test_fixed_profile_reports_full_brightness(void) {
  for (int curve = DALI_DIM_CURVE_STANDARD; curve <= DALI_DIM_CURVE_LINEAR; curve++) {
    DaliLevelProfile profile = {
        static_cast<DaliDimCurve>(curve),
        85u,
        85u,
    };
    uint8_t level = 0u;
    float brightness = 0.0f;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_light_brightness_to_level(&profile, ha_brightness(1u),
                                                     &level));
    TEST_ASSERT_EQUAL_UINT8(85u, level);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_light_brightness_to_level(&profile, ha_brightness(255u),
                                                     &level));
    TEST_ASSERT_EQUAL_UINT8(85u, level);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_light_level_to_brightness(&profile, 85u, &brightness));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, brightness);
  }
}

void test_mask_and_off_cannot_be_decoded_as_on_brightness(void) {
  DaliLevelProfile profile = {DALI_DIM_CURVE_STANDARD, 20u, 200u};
  float brightness = 0.5f;

  TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                    dali_light_level_to_brightness(&profile, 0u, &brightness));
  TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                    dali_light_level_to_brightness(&profile,
                                                   DALI_DAPC_MASK_LEVEL,
                                                   &brightness));
}

void test_observed_level_outside_window_reports_the_nearest_level(void) {
  DaliLevelProfile profile = {DALI_DIM_CURVE_STANDARD, 50u, 200u};
  uint8_t decoded = 0u;
  float brightness = 0.0f;
  float expected = 0.0f;

  /* Below MIN: the gear is at this entity's floor as far as it can express. */
  TEST_ASSERT_EQUAL(DALI_OK,
                    dali_light_observed_level_to_brightness(&profile, 30u,
                                                            &decoded,
                                                            &brightness));
  TEST_ASSERT_EQUAL_UINT8(50u, decoded);
  TEST_ASSERT_EQUAL(DALI_OK,
                    dali_light_level_to_brightness(&profile, 50u, &expected));
  TEST_ASSERT_EQUAL_FLOAT(expected, brightness);

  /* Above MAX: likewise the ceiling, not a refusal. */
  TEST_ASSERT_EQUAL(DALI_OK,
                    dali_light_observed_level_to_brightness(&profile, 254u,
                                                            &decoded,
                                                            &brightness));
  TEST_ASSERT_EQUAL_UINT8(200u, decoded);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, brightness);

  /* Inside the window nothing is clamped. */
  TEST_ASSERT_EQUAL(DALI_OK,
                    dali_light_observed_level_to_brightness(&profile, 120u,
                                                            &decoded,
                                                            &brightness));
  TEST_ASSERT_EQUAL_UINT8(120u, decoded);

  /* Neither OFF nor MASK is an on-state, at any window. */
  TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                    dali_light_observed_level_to_brightness(&profile, 0u,
                                                            &decoded,
                                                            &brightness));
  TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                    dali_light_observed_level_to_brightness(
                        &profile, DALI_DAPC_MASK_LEVEL, &decoded, &brightness));
}

void test_clamped_reading_round_trips_to_a_level_inside_the_window(void) {
  /* The clamped brightness is what write_state() converts back, so it must map
   * to the clamped level — otherwise the echo is missed and the entity sends
   * the reading it just took off the bus back to the gear. */
  for (uint16_t min_level = 1u; min_level <= DALI_DAPC_MAX_LEVEL;
       min_level += 7u) {
    for (uint16_t max_level = min_level; max_level <= DALI_DAPC_MAX_LEVEL;
         max_level += 11u) {
      DaliLevelProfile profile = {
          DALI_DIM_CURVE_STANDARD,
          static_cast<uint8_t>(min_level),
          static_cast<uint8_t>(max_level),
      };
      for (uint16_t level = 1u; level < DALI_DAPC_MASK_LEVEL; level++) {
        uint8_t decoded = 0u;
        uint8_t round_trip = 0u;
        float brightness = 0.0f;
        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_light_observed_level_to_brightness(
                              &profile, static_cast<uint8_t>(level), &decoded,
                              &brightness));
        TEST_ASSERT_TRUE(decoded >= profile.min_level);
        TEST_ASSERT_TRUE(decoded <= profile.max_level);
        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_light_brightness_to_level(&profile, brightness,
                                                         &round_trip));
        TEST_ASSERT_EQUAL_UINT8(decoded, round_trip);
      }
    }
  }
}

void test_group_union_covers_every_member_range(void) {
  DaliLevelProfile window = {DALI_DIM_CURVE_STANDARD, 85u, 200u};
  const DaliLevelProfile wide = {DALI_DIM_CURVE_STANDARD, 1u, 254u};
  const DaliLevelProfile narrow = {DALI_DIM_CURVE_STANDARD, 100u, 150u};

  TEST_ASSERT_EQUAL(DALI_OK, dali_level_profile_widen(&window, &narrow));
  TEST_ASSERT_EQUAL_UINT8(85u, window.min_level);
  TEST_ASSERT_EQUAL_UINT8(200u, window.max_level);

  /* A wider member wins in both directions: the group must still be able to
   * drive it over its whole range. */
  TEST_ASSERT_EQUAL(DALI_OK, dali_level_profile_widen(&window, &wide));
  TEST_ASSERT_EQUAL_UINT8(1u, window.min_level);
  TEST_ASSERT_EQUAL_UINT8(254u, window.max_level);
  TEST_ASSERT_EQUAL(DALI_DIM_CURVE_STANDARD, window.curve);
}

void test_group_union_falls_back_to_standard_on_mixed_curves(void) {
  DaliLevelProfile window = {DALI_DIM_CURVE_LINEAR, 20u, 200u};
  const DaliLevelProfile same_curve = {DALI_DIM_CURVE_LINEAR, 30u, 210u};
  const DaliLevelProfile other_curve = {DALI_DIM_CURVE_STANDARD, 40u, 220u};

  TEST_ASSERT_EQUAL(DALI_OK, dali_level_profile_widen(&window, &same_curve));
  TEST_ASSERT_EQUAL(DALI_DIM_CURVE_LINEAR, window.curve);

  TEST_ASSERT_EQUAL(DALI_OK, dali_level_profile_widen(&window, &other_curve));
  TEST_ASSERT_EQUAL(DALI_DIM_CURVE_STANDARD, window.curve);
  TEST_ASSERT_EQUAL_UINT8(20u, window.min_level);
  TEST_ASSERT_EQUAL_UINT8(220u, window.max_level);
}

void test_group_union_rejects_invalid_members(void) {
  DaliLevelProfile window = {DALI_DIM_CURVE_STANDARD, 20u, 200u};
  const DaliLevelProfile off_level = {DALI_DIM_CURVE_STANDARD, 0u, 100u};
  const DaliLevelProfile mask_level = {DALI_DIM_CURVE_STANDARD, 10u,
                                       DALI_DAPC_MASK_LEVEL};
  const DaliLevelProfile inverted = {DALI_DIM_CURVE_STANDARD, 200u, 20u};

  TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                    dali_level_profile_widen(&window, &off_level));
  TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                    dali_level_profile_widen(&window, &mask_level));
  TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                    dali_level_profile_widen(&window, &inverted));
  TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_level_profile_widen(&window, NULL));
  /* A rejected member must not have moved the window it could not join. */
  TEST_ASSERT_EQUAL_UINT8(20u, window.min_level);
  TEST_ASSERT_EQUAL_UINT8(200u, window.max_level);
}

void test_profile_change_remaps_pending_logical_brightness(void) {
  const float requested_brightness = ha_brightness(128u);
  DaliLevelProfile before = {DALI_DIM_CURVE_STANDARD, 1u, 254u};
  DaliLevelProfile after = {DALI_DIM_CURVE_LINEAR, 20u, 200u};
  uint8_t before_level = 0u;
  uint8_t after_level = 0u;
  DaliLightWrite write = {};

  TEST_ASSERT_EQUAL(DALI_OK,
                    dali_light_brightness_to_level(&before, requested_brightness,
                                                   &before_level));
  dali_light_write_request(&write, true, before_level);
  dali_light_write_invalidate(&write);

  TEST_ASSERT_EQUAL(DALI_OK,
                    dali_light_brightness_to_level(&after, requested_brightness,
                                                   &after_level));
  dali_light_write_request(&write, true, after_level);
  uint8_t queued_level = 0u;
  TEST_ASSERT_EQUAL(DALI_LIGHT_WRITE_SEND_LEVEL,
                    dali_light_write_next(&write, &queued_level));
  TEST_ASSERT_NOT_EQUAL(before_level, after_level);
  TEST_ASSERT_EQUAL_UINT8(after_level, queued_level);
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_ha_endpoints_cover_every_valid_profile);
  RUN_TEST(test_min_level_sits_on_the_lowest_ui_reachable_code);
  RUN_TEST(test_codes_below_the_on_code_floor_clamp_onto_min);
  RUN_TEST(test_ui_floor_reaches_min_on_the_bus_2_gear_window);
  RUN_TEST(test_ha_on_codes_always_map_to_a_legal_profile_level);
  RUN_TEST(test_level_to_ha_to_level_is_canonical_for_every_profile_level);
  RUN_TEST(test_fixed_profile_reports_full_brightness);
  RUN_TEST(test_mask_and_off_cannot_be_decoded_as_on_brightness);
  RUN_TEST(test_observed_level_outside_window_reports_the_nearest_level);
  RUN_TEST(test_clamped_reading_round_trips_to_a_level_inside_the_window);
  RUN_TEST(test_group_union_covers_every_member_range);
  RUN_TEST(test_group_union_falls_back_to_standard_on_mixed_curves);
  RUN_TEST(test_group_union_rejects_invalid_members);
  RUN_TEST(test_profile_change_remaps_pending_logical_brightness);

  return UNITY_END();
}