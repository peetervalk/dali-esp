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
     * The ESPHome light entity recognises the echo of a bus reading by
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

    return UNITY_END();
}
