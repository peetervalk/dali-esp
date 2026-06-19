#include "unity.h"
#include "dali_input_config.h"
#include "dali_protocol.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------*/

/* 24-bit instance command frame for addr, instance, opcode */
static uint32_t expected_instance_frame(uint8_t addr, uint8_t instance, uint8_t opcode)
{
    uint8_t addr_byte = (uint8_t)((addr << 1u) | 1u);
    return ((uint32_t)addr_byte << 16u) | ((uint32_t)instance << 8u) | opcode;
}

static void assert_instance_frame(DaliFrame frame,
                                  uint8_t addr,
                                  uint8_t instance,
                                  uint8_t opcode)
{
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, frame.bit_length);
    TEST_ASSERT_EQUAL_HEX32(expected_instance_frame(addr, instance, opcode), frame.data);
}

/* ---------------------------------------------------------------------------
 * Generic IEC 62386-103 instance configuration
 * --------------------------------------------------------------------------*/

void test_set_event_priority_frame(void)
{
    DaliFrame f = dali_input_build_set_event_priority(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x61u);
}

void test_enable_instance_frame(void)
{
    DaliFrame f = dali_input_build_enable_instance(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x62u);
}

void test_disable_instance_frame(void)
{
    DaliFrame f = dali_input_build_disable_instance(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x63u);
}

void test_set_primary_group_frame(void)
{
    DaliFrame f = dali_input_build_set_primary_group(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x64u);
}

void test_set_instance_group1_frame(void)
{
    DaliFrame f = dali_input_build_set_instance_group1(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x65u);
}

void test_set_instance_group2_frame(void)
{
    DaliFrame f = dali_input_build_set_instance_group2(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x66u);
}

void test_set_event_scheme_frame(void)
{
    DaliFrame f = dali_input_build_set_event_scheme(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x67u);
}

void test_set_event_filter_frame(void)
{
    DaliFrame f = dali_input_build_set_event_filter(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x68u);
}

void test_set_report_timer_frame(void)
{
    DaliFrame f = dali_input_build_set_report_timer(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x6Eu);
}

void test_set_hysteresis_frame(void)
{
    DaliFrame f = dali_input_build_set_hysteresis(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x6Fu);
}

void test_set_deadtime_timer_frame(void)
{
    DaliFrame f = dali_input_build_set_deadtime_timer(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0x70u);
}

/* opcodes must be distinct within the generic range */
void test_generic_opcodes_are_all_distinct(void)
{
    uint8_t addr = 0u, inst = 0u;
    DaliFrame frames[] = {
        dali_input_build_set_event_priority(addr, inst),
        dali_input_build_enable_instance(addr, inst),
        dali_input_build_disable_instance(addr, inst),
        dali_input_build_set_primary_group(addr, inst),
        dali_input_build_set_instance_group1(addr, inst),
        dali_input_build_set_instance_group2(addr, inst),
        dali_input_build_set_event_scheme(addr, inst),
        dali_input_build_set_event_filter(addr, inst),
        dali_input_build_set_report_timer(addr, inst),
        dali_input_build_set_hysteresis(addr, inst),
        dali_input_build_set_deadtime_timer(addr, inst),
    };
    uint8_t n = (uint8_t)(sizeof(frames) / sizeof(frames[0]));
    for (uint8_t i = 0u; i < n; i++) {
        for (uint8_t j = (uint8_t)(i + 1u); j < n; j++) {
            TEST_ASSERT_NOT_EQUAL(frames[i].data, frames[j].data);
        }
    }
}

/* address and instance are encoded independently */
void test_address_and_instance_encoding(void)
{
    DaliFrame f0 = dali_input_build_enable_instance(0u, 0u);
    DaliFrame f1 = dali_input_build_enable_instance(1u, 0u);
    DaliFrame f2 = dali_input_build_enable_instance(0u, 1u);

    TEST_ASSERT_NOT_EQUAL(f0.data, f1.data);
    TEST_ASSERT_NOT_EQUAL(f0.data, f2.data);
    TEST_ASSERT_NOT_EQUAL(f1.data, f2.data);

    /* addr=5, inst=2, opcode=0x62 */
    DaliFrame f5_2 = dali_input_build_enable_instance(5u, 2u);
    assert_instance_frame(f5_2, 5u, 2u, 0x62u);
}

/* ---------------------------------------------------------------------------
 * DT301 push-button type-specific
 * --------------------------------------------------------------------------*/

void test_pb_set_hysteresis_frame(void)
{
    DaliFrame f = dali_input_pb_build_set_hysteresis(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0xE0u);
}

void test_pb_set_deadtime_frame(void)
{
    DaliFrame f = dali_input_pb_build_set_deadtime(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0xE1u);
}

void test_pb_set_double_click_priority_frame(void)
{
    DaliFrame f = dali_input_pb_build_set_double_click_priority(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0xE2u);
}

void test_pb_set_repeat_period_frame(void)
{
    DaliFrame f = dali_input_pb_build_set_repeat_period(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0xE3u);
}

void test_pb_set_hold_timer_frame(void)
{
    DaliFrame f = dali_input_pb_build_set_hold_timer(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0xE4u);
}

/* ---------------------------------------------------------------------------
 * DT303 occupancy type-specific
 * --------------------------------------------------------------------------*/

void test_occ_set_deadtime_frame(void)
{
    DaliFrame f = dali_input_occ_build_set_deadtime(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0xE0u);
}

void test_occ_set_hold_timer_frame(void)
{
    DaliFrame f = dali_input_occ_build_set_hold_timer(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0xE1u);
}

/* ---------------------------------------------------------------------------
 * DT304 light sensor type-specific
 * --------------------------------------------------------------------------*/

void test_light_set_hysteresis_frame(void)
{
    DaliFrame f = dali_input_light_build_set_hysteresis(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0xE0u);
}

void test_light_set_deadband_frame(void)
{
    DaliFrame f = dali_input_light_build_set_deadband(5u, 0u);
    assert_instance_frame(f, 5u, 0u, 0xE1u);
}

/* ---------------------------------------------------------------------------
 * Cross-type isolation: same opcode is intentional for type-specific commands
 * but generic vs type-specific must differ
 * --------------------------------------------------------------------------*/

void test_generic_and_type_specific_use_different_opcodes(void)
{
    /* Generic SetEventPriority (0x61) != PB hysteresis (0xE0) */
    DaliFrame generic_prio = dali_input_build_set_event_priority(5u, 0u);
    DaliFrame pb_hys       = dali_input_pb_build_set_hysteresis(5u, 0u);
    TEST_ASSERT_NOT_EQUAL(generic_prio.data, pb_hys.data);

    /* Generic SetEventPriority (0x61) != light hysteresis (0xE0) */
    DaliFrame light_hys    = dali_input_light_build_set_hysteresis(5u, 0u);
    TEST_ASSERT_NOT_EQUAL(generic_prio.data, light_hys.data);

    /* DT303 deadtime == DT304 hysteresis (both 0xE0, same addr/inst) — both are type-specific */
    TEST_ASSERT_EQUAL(dali_input_occ_build_set_deadtime(5u, 0u).data,
                      dali_input_light_build_set_hysteresis(5u, 0u).data);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_event_priority_frame);
    RUN_TEST(test_enable_instance_frame);
    RUN_TEST(test_disable_instance_frame);
    RUN_TEST(test_set_primary_group_frame);
    RUN_TEST(test_set_instance_group1_frame);
    RUN_TEST(test_set_instance_group2_frame);
    RUN_TEST(test_set_event_scheme_frame);
    RUN_TEST(test_set_event_filter_frame);
    RUN_TEST(test_set_report_timer_frame);
    RUN_TEST(test_set_hysteresis_frame);
    RUN_TEST(test_set_deadtime_timer_frame);
    RUN_TEST(test_generic_opcodes_are_all_distinct);
    RUN_TEST(test_address_and_instance_encoding);
    RUN_TEST(test_pb_set_hysteresis_frame);
    RUN_TEST(test_pb_set_deadtime_frame);
    RUN_TEST(test_pb_set_double_click_priority_frame);
    RUN_TEST(test_pb_set_repeat_period_frame);
    RUN_TEST(test_pb_set_hold_timer_frame);
    RUN_TEST(test_occ_set_deadtime_frame);
    RUN_TEST(test_occ_set_hold_timer_frame);
    RUN_TEST(test_light_set_hysteresis_frame);
    RUN_TEST(test_light_set_deadband_frame);
    RUN_TEST(test_generic_and_type_specific_use_different_opcodes);
    return UNITY_END();
}
