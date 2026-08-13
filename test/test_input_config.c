#include "unity.h"
#include "dali_input_config.h"
#include "dali_input_device.h"

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

/* ---------------------------------------------------------------------------
 * Atomic configuration sequences
 *
 * These are Part 103 control-device commands, so the DTR loads use the 24-bit
 * control-device DTR frames (0xC1 0x30/0x31/0x32 <value>) and there is no
 * ENABLE DEVICE TYPE step. Keeping the loads with the command they feed is what
 * stops another locally scheduled frame from replacing a DTR in between.
 * --------------------------------------------------------------------------*/

static void test_config_sequence_uses_control_device_dtrs(void)
{
    DaliSequence seq;
    const uint8_t dtr[3] = { 0x11u, 0x22u, 0x33u };
    DaliFrame command = dali_input_build_set_event_filter(TEST_ADDR, TEST_INSTANCE);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_config_sequence(command, true, false,
                                                       dtr, 3u, &seq));
    TEST_ASSERT_EQUAL_UINT8(4u, seq.step_count);
    TEST_ASSERT_EQUAL_HEX32(0xC13011u, seq.steps[0].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC13122u, seq.steps[1].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC13233u, seq.steps[2].frame.data);
    TEST_ASSERT_EQUAL_HEX32(command.data, seq.steps[3].frame.data);
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, seq.steps[0].frame.bit_length);
    TEST_ASSERT_EQUAL_UINT8(3u, DALI_INPUT_CONFIG_COMMAND_STEP(3u));
}

/*
 * Send-twice expansion is the scheduler's job, so a repeated command still
 * occupies one step. ENABLE INSTANCE takes no DTR at all.
 */
static void test_config_sequence_without_dtr_is_one_step(void)
{
    DaliSequence seq;
    DaliFrame command = dali_input_build_enable_instance(TEST_ADDR, TEST_INSTANCE);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_config_sequence(command, true, false,
                                                       NULL, 0u, &seq));
    TEST_ASSERT_EQUAL_UINT8(1u, seq.step_count);
    TEST_ASSERT_EQUAL_HEX32(command.data, seq.steps[0].frame.data);
    TEST_ASSERT_TRUE(seq.steps[0].send_twice);
    TEST_ASSERT_FALSE(seq.steps[0].needs_reply);
    TEST_ASSERT_EQUAL_UINT8(0u, DALI_INPUT_CONFIG_COMMAND_STEP(0u));
}

/* The same builder carries the DTR0-selected queries, which do expect a reply. */
static void test_config_sequence_can_expect_a_reply(void)
{
    DaliSequence seq;
    const uint8_t dtr[1] = { 0x05u };
    DaliFrame command;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_query_instance_configuration(TEST_ADDR,
                                                                    TEST_INSTANCE,
                                                                    &command));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_config_sequence(command, false, true,
                                                       dtr, 1u, &seq));
    TEST_ASSERT_EQUAL_UINT8(2u, seq.step_count);
    TEST_ASSERT_EQUAL_HEX32(0xC13005u, seq.steps[0].frame.data);
    TEST_ASSERT_FALSE(seq.steps[0].needs_reply);
    TEST_ASSERT_TRUE(seq.steps[1].needs_reply);
    TEST_ASSERT_FALSE(seq.steps[1].send_twice);
}

static void test_config_sequence_carries_no_retry_budget(void)
{
    DaliSequence seq;
    const uint8_t dtr[2] = { 0x01u, 0x02u };
    DaliFrame command = dali_input_build_set_instance_configuration(TEST_ADDR,
                                                                    TEST_INSTANCE);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_config_sequence(command, true, false,
                                                       dtr, 2u, &seq));
    for (uint8_t i = 0u; i < seq.step_count; i++) {
        TEST_ASSERT_EQUAL_UINT8(0u, seq.steps[i].retries_left);
    }
}

/* A 16-bit control-gear frame is not a Part 103 command and must be refused. */
static void test_config_sequence_rejects_invalid_arguments(void)
{
    DaliSequence seq;
    const uint8_t dtr[3] = { 0u, 0u, 0u };
    DaliFrame command = dali_input_build_enable_instance(TEST_ADDR, TEST_INSTANCE);
    DaliFrame gear_frame = { .data = 0x07E3u, .bit_length = DALI_FORWARD_FRAME_BITS };

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_build_config_sequence(command, true, false,
                                                       dtr, 3u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_build_config_sequence(gear_frame, true, false,
                                                       NULL, 0u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_build_config_sequence(command, true, false,
                                                       dtr, 4u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_build_config_sequence(command, true, false,
                                                       NULL, 1u, &seq));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_part_103_common_configuration_vectors);
    RUN_TEST(test_part_301_instance_type_1_vectors);
    RUN_TEST(test_part_303_instance_type_3_vectors);
    RUN_TEST(test_part_304_instance_type_4_vectors);
    RUN_TEST(test_config_sequence_uses_control_device_dtrs);
    RUN_TEST(test_config_sequence_without_dtr_is_one_step);
    RUN_TEST(test_config_sequence_can_expect_a_reply);
    RUN_TEST(test_config_sequence_carries_no_retry_budget);
    RUN_TEST(test_config_sequence_rejects_invalid_arguments);
    return UNITY_END();
}
