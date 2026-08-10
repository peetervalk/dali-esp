#include "unity.h"
#include "dali_input_config.h"

void setUp(void) {}
void tearDown(void) {}

/*
 * Independent standard vectors use short address A = 5 and instance number
 * N = 2. Their complete 24-bit prefix is therefore 0x0B02; expected values
 * below are literals rather than values derived with the production encoder.
 */
#define TEST_ADDR      5u
#define TEST_INSTANCE  2u

static void assert_full_frame(DaliFrame frame, uint32_t expected)
{
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, frame.bit_length);
    TEST_ASSERT_EQUAL_HEX32(expected, frame.data);
}

void test_part_103_common_configuration_vectors(void)
{
    assert_full_frame(dali_input_build_set_event_priority(TEST_ADDR, TEST_INSTANCE),
                      0x0B0261u);
    assert_full_frame(dali_input_build_enable_instance(TEST_ADDR, TEST_INSTANCE),
                      0x0B0262u);
    assert_full_frame(dali_input_build_disable_instance(TEST_ADDR, TEST_INSTANCE),
                      0x0B0263u);
    assert_full_frame(dali_input_build_set_primary_group(TEST_ADDR, TEST_INSTANCE),
                      0x0B0264u);
    assert_full_frame(dali_input_build_set_instance_group1(TEST_ADDR, TEST_INSTANCE),
                      0x0B0265u);
    assert_full_frame(dali_input_build_set_instance_group2(TEST_ADDR, TEST_INSTANCE),
                      0x0B0266u);
    assert_full_frame(dali_input_build_set_event_scheme(TEST_ADDR, TEST_INSTANCE),
                      0x0B0267u);
    assert_full_frame(dali_input_build_set_event_filter(TEST_ADDR, TEST_INSTANCE),
                      0x0B0268u);
    assert_full_frame(dali_input_build_set_instance_type(TEST_ADDR, TEST_INSTANCE),
                      0x0B0269u);
    assert_full_frame(dali_input_build_set_instance_configuration(TEST_ADDR, TEST_INSTANCE),
                      0x0B026Au);
}

void test_part_301_instance_type_1_vectors(void)
{
    assert_full_frame(dali_input_pb_build_set_short_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B0200u);
    assert_full_frame(dali_input_pb_build_set_double_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B0201u);
    assert_full_frame(dali_input_pb_build_set_repeat_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B0202u);
    assert_full_frame(dali_input_pb_build_set_stuck_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B0203u);

    assert_full_frame(dali_input_pb_build_query_short_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B020Au);
    assert_full_frame(dali_input_pb_build_query_short_timer_min(TEST_ADDR, TEST_INSTANCE),
                      0x0B020Bu);
    assert_full_frame(dali_input_pb_build_query_double_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B020Cu);
    assert_full_frame(dali_input_pb_build_query_double_timer_min(TEST_ADDR, TEST_INSTANCE),
                      0x0B020Du);
    assert_full_frame(dali_input_pb_build_query_repeat_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B020Eu);
    assert_full_frame(dali_input_pb_build_query_stuck_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B020Fu);
}

void test_part_303_instance_type_3_vectors(void)
{
    assert_full_frame(dali_input_occ_build_catch_movement(TEST_ADDR, TEST_INSTANCE),
                      0x0B0220u);
    assert_full_frame(dali_input_occ_build_set_hold_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B0221u);
    assert_full_frame(dali_input_occ_build_set_report_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B0222u);
    assert_full_frame(dali_input_occ_build_set_deadtime(TEST_ADDR, TEST_INSTANCE),
                      0x0B0223u);
    assert_full_frame(dali_input_occ_build_cancel_hold_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B0224u);
    assert_full_frame(dali_input_occ_build_set_detection_range(TEST_ADDR, TEST_INSTANCE),
                      0x0B0225u);
    assert_full_frame(dali_input_occ_build_set_sensitivity(TEST_ADDR, TEST_INSTANCE),
                      0x0B0226u);

    assert_full_frame(dali_input_occ_build_query_capabilities(TEST_ADDR, TEST_INSTANCE),
                      0x0B0229u);
    assert_full_frame(dali_input_occ_build_query_detection_range(TEST_ADDR, TEST_INSTANCE),
                      0x0B022Au);
    assert_full_frame(dali_input_occ_build_query_sensitivity(TEST_ADDR, TEST_INSTANCE),
                      0x0B022Bu);
    assert_full_frame(dali_input_occ_build_query_deadtime(TEST_ADDR, TEST_INSTANCE),
                      0x0B022Cu);
    assert_full_frame(dali_input_occ_build_query_hold_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B022Du);
    assert_full_frame(dali_input_occ_build_query_report_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B022Eu);
    assert_full_frame(dali_input_occ_build_query_catching(TEST_ADDR, TEST_INSTANCE),
                      0x0B022Fu);
}

void test_part_304_instance_type_4_vectors(void)
{
    assert_full_frame(dali_input_light_build_set_report_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B0230u);
    assert_full_frame(dali_input_light_build_set_hysteresis(TEST_ADDR, TEST_INSTANCE),
                      0x0B0231u);
    assert_full_frame(dali_input_light_build_set_deadtime(TEST_ADDR, TEST_INSTANCE),
                      0x0B0232u);
    assert_full_frame(dali_input_light_build_set_hysteresis_min(TEST_ADDR, TEST_INSTANCE),
                      0x0B0233u);

    assert_full_frame(dali_input_light_build_query_hysteresis_min(TEST_ADDR, TEST_INSTANCE),
                      0x0B023Cu);
    assert_full_frame(dali_input_light_build_query_deadtime(TEST_ADDR, TEST_INSTANCE),
                      0x0B023Du);
    assert_full_frame(dali_input_light_build_query_report_timer(TEST_ADDR, TEST_INSTANCE),
                      0x0B023Eu);
    assert_full_frame(dali_input_light_build_query_hysteresis(TEST_ADDR, TEST_INSTANCE),
                      0x0B023Fu);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_part_103_common_configuration_vectors);
    RUN_TEST(test_part_301_instance_type_1_vectors);
    RUN_TEST(test_part_303_instance_type_3_vectors);
    RUN_TEST(test_part_304_instance_type_4_vectors);
    return UNITY_END();
}
