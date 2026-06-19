/*
 * test_gear_dt6.c — unit tests for dali_gear_dt6 (IEC 62386-207)
 */

#include "unity.h"
#include "dali_gear_dt6.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------*/

static uint8_t addr_byte(uint8_t addr)
{
    return (uint8_t)((addr << 1u) | 1u);
}

static uint8_t frame_addr_byte(DaliFrame f)
{
    return (uint8_t)((f.data >> 8u) & 0xFFu);
}

static uint8_t frame_opcode(DaliFrame f)
{
    return (uint8_t)(f.data & 0xFFu);
}

/* ---------------------------------------------------------------------------
 * Enable frame
 * --------------------------------------------------------------------------*/

void test_enable_matches_protocol_enable_device_type_6(void)
{
    DaliFrame got      = dali_dt6_enable();
    DaliFrame expected = dali_cmd_enable_device_type(6u);
    TEST_ASSERT_EQUAL_UINT32(expected.data,       got.data);
    TEST_ASSERT_EQUAL_UINT8 (expected.bit_length, got.bit_length);
}

void test_enable_is_16bit(void)
{
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt6_enable().bit_length);
}

/* ---------------------------------------------------------------------------
 * Configuration command frames — opcode and address encoding
 * --------------------------------------------------------------------------*/

void test_config_opcodes(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xE0u, frame_opcode(dali_dt6_reference_system_power(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE1u, frame_opcode(dali_dt6_enable_current_protector(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE2u, frame_opcode(dali_dt6_disable_current_protector(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE3u, frame_opcode(dali_dt6_select_dimming_curve(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE4u, frame_opcode(dali_dt6_store_dtr_as_fast_fade_time(0u)));
}

void test_config_frames_are_16bit(void)
{
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt6_reference_system_power(0u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt6_select_dimming_curve(0u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt6_store_dtr_as_fast_fade_time(0u).bit_length);
}

void test_config_address_encoding(void)
{
    for (uint8_t addr = 0u; addr < 4u; addr++) {
        TEST_ASSERT_EQUAL_HEX8(addr_byte(addr),
                               frame_addr_byte(dali_dt6_reference_system_power(addr)));
        TEST_ASSERT_EQUAL_HEX8(addr_byte(addr),
                               frame_addr_byte(dali_dt6_select_dimming_curve(addr)));
    }
}

void test_config_different_addresses_produce_different_frames(void)
{
    DaliFrame f0 = dali_dt6_reference_system_power(0u);
    DaliFrame f5 = dali_dt6_reference_system_power(5u);
    TEST_ASSERT_NOT_EQUAL(f0.data, f5.data);
    /* opcodes must be the same */
    TEST_ASSERT_EQUAL_HEX8(frame_opcode(f0), frame_opcode(f5));
}

/* ---------------------------------------------------------------------------
 * Query command frames — opcode and address encoding
 * --------------------------------------------------------------------------*/

void test_query_opcodes(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xEDu, frame_opcode(dali_dt6_query_gear_type(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xEEu, frame_opcode(dali_dt6_query_dimming_curve(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xEFu, frame_opcode(dali_dt6_query_possible_operating_modes(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF0u, frame_opcode(dali_dt6_query_features(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF1u, frame_opcode(dali_dt6_query_failure_status(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF2u, frame_opcode(dali_dt6_query_short_circuit(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF3u, frame_opcode(dali_dt6_query_open_circuit(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF4u, frame_opcode(dali_dt6_query_load_decrease(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF5u, frame_opcode(dali_dt6_query_load_increase(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF6u, frame_opcode(dali_dt6_query_current_protector_active(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF7u, frame_opcode(dali_dt6_query_thermal_shutdown(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF8u, frame_opcode(dali_dt6_query_thermal_overload(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF9u, frame_opcode(dali_dt6_query_reference_running(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xFAu, frame_opcode(dali_dt6_query_reference_measurement_failed(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xFBu, frame_opcode(dali_dt6_query_current_protector_enabled(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xFCu, frame_opcode(dali_dt6_query_operating_mode(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xFDu, frame_opcode(dali_dt6_query_fast_fade_time(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xFEu, frame_opcode(dali_dt6_query_min_fast_fade_time(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xFFu, frame_opcode(dali_dt6_query_extended_version_number(0u)));
}

void test_query_frames_are_16bit(void)
{
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt6_query_failure_status(0u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt6_query_features(0u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt6_query_gear_type(0u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt6_query_extended_version_number(0u).bit_length);
}

void test_query_address_encoding(void)
{
    for (uint8_t addr = 0u; addr < 4u; addr++) {
        TEST_ASSERT_EQUAL_HEX8(addr_byte(addr),
                               frame_addr_byte(dali_dt6_query_failure_status(addr)));
        TEST_ASSERT_EQUAL_HEX8(addr_byte(addr),
                               frame_addr_byte(dali_dt6_query_features(addr)));
        TEST_ASSERT_EQUAL_HEX8(addr_byte(addr),
                               frame_addr_byte(dali_dt6_query_gear_type(addr)));
    }
}

void test_query_max_address(void)
{
    DaliFrame f = dali_dt6_query_failure_status(DALI_MAX_SHORT_ADDRESS);
    TEST_ASSERT_EQUAL_HEX8(addr_byte(DALI_MAX_SHORT_ADDRESS), frame_addr_byte(f));
    TEST_ASSERT_EQUAL_HEX8(0xF1u, frame_opcode(f));
}

void test_config_and_query_same_addr_differ_by_opcode(void)
{
    uint8_t addr = 7u;
    DaliFrame config = dali_dt6_reference_system_power(addr);
    DaliFrame query  = dali_dt6_query_failure_status(addr);
    /* Same address byte, different opcode */
    TEST_ASSERT_EQUAL_HEX8(frame_addr_byte(config), frame_addr_byte(query));
    TEST_ASSERT_NOT_EQUAL  (frame_opcode(config),    frame_opcode(query));
}

/* ---------------------------------------------------------------------------
 * parse_failure_status
 * --------------------------------------------------------------------------*/

void test_parse_failure_status_null_returns_invalid(void)
{
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_dt6_parse_failure_status(0xFFu, NULL));
}

void test_parse_failure_status_all_zero_no_faults(void)
{
    DaliDt6FailureStatus s;
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_dt6_parse_failure_status(0x00u, &s));
    TEST_ASSERT_FALSE(s.led_short_circuit);
    TEST_ASSERT_FALSE(s.led_open_circuit);
    TEST_ASSERT_FALSE(s.load_decrease);
    TEST_ASSERT_FALSE(s.load_increase);
    TEST_ASSERT_FALSE(s.current_protector_active);
    TEST_ASSERT_FALSE(s.thermal_shutdown);
    TEST_ASSERT_FALSE(s.thermal_overload);
    TEST_ASSERT_FALSE(s.reference_measurement_failed);
}

void test_parse_failure_status_all_ones_all_faults(void)
{
    DaliDt6FailureStatus s;
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_dt6_parse_failure_status(0xFFu, &s));
    TEST_ASSERT_TRUE(s.led_short_circuit);
    TEST_ASSERT_TRUE(s.led_open_circuit);
    TEST_ASSERT_TRUE(s.load_decrease);
    TEST_ASSERT_TRUE(s.load_increase);
    TEST_ASSERT_TRUE(s.current_protector_active);
    TEST_ASSERT_TRUE(s.thermal_shutdown);
    TEST_ASSERT_TRUE(s.thermal_overload);
    TEST_ASSERT_TRUE(s.reference_measurement_failed);
}

void test_parse_failure_status_individual_bits(void)
{
    DaliDt6FailureStatus s;

    dali_dt6_parse_failure_status(0x01u, &s);
    TEST_ASSERT_TRUE (s.led_short_circuit);
    TEST_ASSERT_FALSE(s.led_open_circuit);

    dali_dt6_parse_failure_status(0x02u, &s);
    TEST_ASSERT_FALSE(s.led_short_circuit);
    TEST_ASSERT_TRUE (s.led_open_circuit);

    dali_dt6_parse_failure_status(0x04u, &s);
    TEST_ASSERT_TRUE (s.load_decrease);
    TEST_ASSERT_FALSE(s.load_increase);

    dali_dt6_parse_failure_status(0x08u, &s);
    TEST_ASSERT_FALSE(s.load_decrease);
    TEST_ASSERT_TRUE (s.load_increase);

    dali_dt6_parse_failure_status(0x10u, &s);
    TEST_ASSERT_TRUE (s.current_protector_active);
    TEST_ASSERT_FALSE(s.thermal_shutdown);

    dali_dt6_parse_failure_status(0x20u, &s);
    TEST_ASSERT_TRUE (s.thermal_shutdown);
    TEST_ASSERT_FALSE(s.thermal_overload);

    dali_dt6_parse_failure_status(0x40u, &s);
    TEST_ASSERT_TRUE (s.thermal_overload);
    TEST_ASSERT_FALSE(s.reference_measurement_failed);

    dali_dt6_parse_failure_status(0x80u, &s);
    TEST_ASSERT_TRUE (s.reference_measurement_failed);
    TEST_ASSERT_FALSE(s.thermal_overload);
}

void test_parse_failure_status_mixed(void)
{
    DaliDt6FailureStatus s;
    /* short circuit + thermal shutdown */
    dali_dt6_parse_failure_status(0x21u, &s);
    TEST_ASSERT_TRUE (s.led_short_circuit);
    TEST_ASSERT_FALSE(s.led_open_circuit);
    TEST_ASSERT_TRUE (s.thermal_shutdown);
    TEST_ASSERT_FALSE(s.thermal_overload);
}

/* ---------------------------------------------------------------------------
 * Runner
 * --------------------------------------------------------------------------*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_enable_matches_protocol_enable_device_type_6);
    RUN_TEST(test_enable_is_16bit);

    RUN_TEST(test_config_opcodes);
    RUN_TEST(test_config_frames_are_16bit);
    RUN_TEST(test_config_address_encoding);
    RUN_TEST(test_config_different_addresses_produce_different_frames);

    RUN_TEST(test_query_opcodes);
    RUN_TEST(test_query_frames_are_16bit);
    RUN_TEST(test_query_address_encoding);
    RUN_TEST(test_query_max_address);
    RUN_TEST(test_config_and_query_same_addr_differ_by_opcode);

    RUN_TEST(test_parse_failure_status_null_returns_invalid);
    RUN_TEST(test_parse_failure_status_all_zero_no_faults);
    RUN_TEST(test_parse_failure_status_all_ones_all_faults);
    RUN_TEST(test_parse_failure_status_individual_bits);
    RUN_TEST(test_parse_failure_status_mixed);

    return UNITY_END();
}
