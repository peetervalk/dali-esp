#include "unity.h"
#include "dali_dim_curve.h"

#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

/*
 * Anchors taken from the curve IEC 62386-102 defines, not from the
 * implementation: three decades of light output spread over levels 1..254.
 * Recomputing them here with double precision keeps the expectation
 * independent of the float arithmetic under test.
 */
static float expected_output(int level)
{
    return (float)pow(10.0, ((double)(level - 1) / (253.0 / 3.0)) - 3.0);
}

/* ── level → output ──────────────────────────────────────────────────────── */

static void test_endpoints_span_three_decades(void)
{
    /* Dimmest legal level is 0.1 %, brightest is 100 %. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.001f, dali_dim_curve_level_to_output(1u));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f,   dali_dim_curve_level_to_output(254u));
}

static void test_off_and_mask_are_not_on_the_curve(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, dali_dim_curve_level_to_output(0u));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, dali_dim_curve_level_to_output(255u));
}

static void test_level_85_is_about_one_percent(void)
{
    /* The case that motivated this module: a linear reading calls level 85
     * "33 %", but the gear is emitting roughly 1 % of maximum. */
    TEST_ASSERT_FLOAT_WITHIN(0.0002f, 0.0099f, dali_dim_curve_level_to_output(85u));
}

static void test_matches_the_standard_curve_at_every_level(void)
{
    for (int level = 1; level <= 254; level++) {
        float expect = expected_output(level);
        /* Relative tolerance: absolute error is meaningless across three
         * decades, where a legal output can be as small as 0.001. */
        TEST_ASSERT_FLOAT_WITHIN(expect * 1e-4f, expect,
                                 dali_dim_curve_level_to_output((uint8_t)level));
    }
}

static void test_output_increases_with_level(void)
{
    for (int level = 2; level <= 254; level++) {
        TEST_ASSERT_TRUE(dali_dim_curve_level_to_output((uint8_t)level) >
                         dali_dim_curve_level_to_output((uint8_t)(level - 1)));
    }
}

/* ── output → level ──────────────────────────────────────────────────────── */

static void test_requested_percentages_land_on_the_matching_level(void)
{
    /* A caller asking for N % light should get the level that emits N %,
     * not the level N % of the way up the scale. */
    TEST_ASSERT_EQUAL_UINT8(85u,  dali_dim_curve_output_to_level(0.01f));
    TEST_ASSERT_EQUAL_UINT8(170u, dali_dim_curve_output_to_level(0.10f));
    TEST_ASSERT_EQUAL_UINT8(229u, dali_dim_curve_output_to_level(0.50f));
    TEST_ASSERT_EQUAL_UINT8(254u, dali_dim_curve_output_to_level(1.0f));
}

static void test_out_of_range_output_clamps_to_a_legal_level(void)
{
    /* Never level 0: OFF is a separate command, not the bottom of the curve. */
    TEST_ASSERT_EQUAL_UINT8(1u, dali_dim_curve_output_to_level(0.0f));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_dim_curve_output_to_level(-1.0f));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_dim_curve_output_to_level(0.0001f));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_dim_curve_output_to_level(DALI_DIM_CURVE_MIN_OUTPUT));

    TEST_ASSERT_EQUAL_UINT8(254u, dali_dim_curve_output_to_level(1.5f));
    TEST_ASSERT_EQUAL_UINT8(254u, dali_dim_curve_output_to_level(DALI_DIM_CURVE_MAX_OUTPUT));
}

static void test_nan_clamps_instead_of_propagating(void)
{
    TEST_ASSERT_EQUAL_UINT8(1u, dali_dim_curve_output_to_level((float)NAN));
}

static void test_level_increases_with_requested_output(void)
{
    uint8_t previous = dali_dim_curve_output_to_level(0.002f);
    for (int permille = 3; permille <= 1000; permille++) {
        uint8_t level = dali_dim_curve_output_to_level((float)permille / 1000.0f);
        TEST_ASSERT_TRUE(level >= previous);
        previous = level;
    }
}

/* ── round trip ──────────────────────────────────────────────────────────── */

static void test_every_level_survives_a_round_trip(void)
{
    /*
     * The ESPHome light entity recognizes the echo of a bus reading by
     * converting the level it published back and comparing. A level that does
     * not come back unchanged would be read as an operator command and sent
     * to the bus, so this has to hold for all 254 of them, not just the ends.
     */
    for (int level = 1; level <= 254; level++) {
        float output = dali_dim_curve_level_to_output((uint8_t)level);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)level,
                                dali_dim_curve_output_to_level(output));
    }
}

static void test_typed_curves_match_definitions_and_round_trip(void)
{
    for (int level = 1; level <= 254; level++) {
        float output = -1.0f;
        uint8_t round_trip = 0u;
        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_dim_curve_level_to_output_for(
                              DALI_DIM_CURVE_STANDARD, (uint8_t)level, &output));
        TEST_ASSERT_FLOAT_WITHIN(expected_output(level) * 1e-4f,
                                 expected_output(level), output);
        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_dim_curve_output_to_level_for(
                              DALI_DIM_CURVE_STANDARD, output, &round_trip));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)level, round_trip);

        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_dim_curve_level_to_output_for(
                              DALI_DIM_CURVE_LINEAR, (uint8_t)level, &output));
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, (float)level / 254.0f, output);
        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_dim_curve_output_to_level_for(
                              DALI_DIM_CURVE_LINEAR, output, &round_trip));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)level, round_trip);
    }
}

static void test_typed_curve_validation_and_clamping(void)
{
    float output = 42.0f;
    uint8_t level = 42u;
    TEST_ASSERT_TRUE(dali_dim_curve_is_valid(DALI_DIM_CURVE_STANDARD));
    TEST_ASSERT_TRUE(dali_dim_curve_is_valid(DALI_DIM_CURVE_LINEAR));
    TEST_ASSERT_FALSE(dali_dim_curve_is_valid((DaliDimCurve)2));

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dim_curve_level_to_output_for(
                          DALI_DIM_CURVE_STANDARD, 0u, &output));
    TEST_ASSERT_EQUAL_FLOAT(42.0f, output);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dim_curve_level_to_output_for(
                          DALI_DIM_CURVE_LINEAR, 255u, &output));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dim_curve_level_to_output_for(
                          (DaliDimCurve)2, 1u, &output));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dim_curve_level_to_output_for(
                          DALI_DIM_CURVE_STANDARD, 1u, NULL));

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dim_curve_output_to_level_for(
                          DALI_DIM_CURVE_STANDARD, (float)NAN, &level));
    TEST_ASSERT_EQUAL_UINT8(42u, level);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dim_curve_output_to_level_for(
                          DALI_DIM_CURVE_STANDARD, (float)INFINITY, &level));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dim_curve_output_to_level_for(
                          DALI_DIM_CURVE_LINEAR, -(float)INFINITY, &level));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dim_curve_output_to_level_for(
                          (DaliDimCurve)2, 0.5f, &level));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dim_curve_output_to_level_for(
                          DALI_DIM_CURVE_STANDARD, 0.5f, NULL));

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dim_curve_output_to_level_for(
                          DALI_DIM_CURVE_STANDARD, -1.0f, &level));
    TEST_ASSERT_EQUAL_UINT8(1u, level);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dim_curve_output_to_level_for(
                          DALI_DIM_CURVE_LINEAR, 0.0f, &level));
    TEST_ASSERT_EQUAL_UINT8(1u, level);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dim_curve_output_to_level_for(
                          DALI_DIM_CURVE_LINEAR, 2.0f, &level));
    TEST_ASSERT_EQUAL_UINT8(254u, level);
}

static void test_profile_validation_and_bad_arguments(void)
{
    DaliLevelProfile profile = {
        .curve = DALI_DIM_CURVE_STANDARD,
        .min_level = 1u,
        .max_level = 254u,
    };
    uint8_t level = 42u;
    float relative = 0.25f;

    TEST_ASSERT_EQUAL(DALI_OK, dali_level_profile_validate(&profile));
    profile.curve = DALI_DIM_CURVE_LINEAR;
    TEST_ASSERT_EQUAL(DALI_OK, dali_level_profile_validate(&profile));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_level_profile_validate(NULL));
    profile.curve = (DaliDimCurve)2;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_level_profile_validate(&profile));
    profile.curve = DALI_DIM_CURVE_STANDARD;
    profile.min_level = 0u;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_level_profile_validate(&profile));
    profile.min_level = 2u;
    profile.max_level = 1u;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_level_profile_validate(&profile));
    profile.min_level = 1u;
    profile.max_level = 255u;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_level_profile_validate(&profile));

    profile.max_level = 254u;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_level_profile_relative_to_level(NULL, 0.5f, &level));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_level_profile_relative_to_level(&profile, 0.5f, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_level_profile_relative_to_level(&profile, -0.01f,
                                                            &level));
    TEST_ASSERT_EQUAL_UINT8(42u, level);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_level_profile_relative_to_level(&profile, 1.01f,
                                                            &level));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_level_profile_relative_to_level(&profile, (float)NAN,
                                                            &level));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_level_profile_relative_to_level(&profile,
                                                            (float)INFINITY,
                                                            &level));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_level_profile_level_to_relative(NULL, 1u,
                                                            &relative));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_level_profile_level_to_relative(&profile, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_level_profile_level_to_relative(&profile, 0u,
                                                            &relative));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, relative);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_level_profile_level_to_relative(&profile, 255u,
                                                            &relative));
}

static void test_profile_mapping_is_in_physical_output_space(void)
{
    DaliLevelProfile standard = {
        .curve = DALI_DIM_CURVE_STANDARD,
        .min_level = 1u,
        .max_level = 254u,
    };
    DaliLevelProfile linear = {
        .curve = DALI_DIM_CURVE_LINEAR,
        .min_level = 1u,
        .max_level = 254u,
    };
    uint8_t level = 0u;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_level_profile_relative_to_level(&standard, 0.5f,
                                                            &level));
    TEST_ASSERT_EQUAL_UINT8(229u, level);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_level_profile_relative_to_level(&linear, 0.5f,
                                                            &level));
    TEST_ASSERT_EQUAL_UINT8(128u, level);
}

static void test_fixed_profile_has_canonical_full_relative_value(void)
{
    for (int curve = DALI_DIM_CURVE_STANDARD;
         curve <= DALI_DIM_CURVE_LINEAR; curve++) {
        DaliLevelProfile profile = {
            .curve = (DaliDimCurve)curve,
            .min_level = 85u,
            .max_level = 85u,
        };
        const float inputs[] = {0.0f, 0.25f, 0.5f, 1.0f};
        for (unsigned int i = 0u; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
            uint8_t level = 0u;
            TEST_ASSERT_EQUAL(DALI_OK,
                              dali_level_profile_relative_to_level(
                                  &profile, inputs[i], &level));
            TEST_ASSERT_EQUAL_UINT8(85u, level);
        }
        float relative = 0.0f;
        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_level_profile_level_to_relative(&profile, 85u,
                                                                &relative));
        TEST_ASSERT_EQUAL_FLOAT(1.0f, relative);
        TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                          dali_level_profile_level_to_relative(&profile, 84u,
                                                                &relative));
        TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                          dali_level_profile_level_to_relative(&profile, 86u,
                                                                &relative));
    }
}

static void test_every_profile_level_round_trips_for_both_curves(void)
{
    for (int curve = DALI_DIM_CURVE_STANDARD;
         curve <= DALI_DIM_CURVE_LINEAR; curve++) {
        for (int min_level = 1; min_level <= 254; min_level++) {
            for (int max_level = min_level; max_level <= 254; max_level++) {
                DaliLevelProfile profile = {
                    .curve = (DaliDimCurve)curve,
                    .min_level = (uint8_t)min_level,
                    .max_level = (uint8_t)max_level,
                };
                TEST_ASSERT_EQUAL(DALI_OK,
                                  dali_level_profile_validate(&profile));
                uint8_t mapped = 0u;
                TEST_ASSERT_EQUAL(DALI_OK,
                                  dali_level_profile_relative_to_level(
                                      &profile, 0.0f, &mapped));
                TEST_ASSERT_EQUAL_UINT8((uint8_t)min_level, mapped);
                TEST_ASSERT_EQUAL(DALI_OK,
                                  dali_level_profile_relative_to_level(
                                      &profile, 1.0f, &mapped));
                TEST_ASSERT_EQUAL_UINT8((uint8_t)max_level, mapped);

                for (int level = min_level; level <= max_level; level++) {
                    float relative = -1.0f;
                    TEST_ASSERT_EQUAL(DALI_OK,
                                      dali_level_profile_level_to_relative(
                                          &profile, (uint8_t)level, &relative));
                    TEST_ASSERT_TRUE(relative >= 0.0f);
                    TEST_ASSERT_TRUE(relative <= 1.0f);
                    TEST_ASSERT_EQUAL(DALI_OK,
                                      dali_level_profile_relative_to_level(
                                          &profile, relative, &mapped));
                    TEST_ASSERT_EQUAL_UINT8((uint8_t)level, mapped);
                }
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_endpoints_span_three_decades);
    RUN_TEST(test_off_and_mask_are_not_on_the_curve);
    RUN_TEST(test_level_85_is_about_one_percent);
    RUN_TEST(test_matches_the_standard_curve_at_every_level);
    RUN_TEST(test_output_increases_with_level);

    RUN_TEST(test_requested_percentages_land_on_the_matching_level);
    RUN_TEST(test_out_of_range_output_clamps_to_a_legal_level);
    RUN_TEST(test_nan_clamps_instead_of_propagating);
    RUN_TEST(test_level_increases_with_requested_output);

    RUN_TEST(test_every_level_survives_a_round_trip);

    RUN_TEST(test_typed_curves_match_definitions_and_round_trip);
    RUN_TEST(test_typed_curve_validation_and_clamping);
    RUN_TEST(test_profile_validation_and_bad_arguments);
    RUN_TEST(test_profile_mapping_is_in_physical_output_space);
    RUN_TEST(test_fixed_profile_has_canonical_full_relative_value);
    RUN_TEST(test_every_profile_level_round_trips_for_both_curves);

    return UNITY_END();
}
