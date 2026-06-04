#include "unity.h"
#include "dali_protocol.h"

void setUp(void)  {}
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Frame builder tests
 * --------------------------------------------------------------------------*/

void test_dapc_address_0_level_128(void)
{
    /* AGENTS.md reference: 0x0080, 16-bit */
    DaliFrame f = dali_cmd_dapc(0u, 128u);
    TEST_ASSERT_EQUAL_HEX32(0x0080u, f.data);
    TEST_ASSERT_EQUAL_UINT8(16u, f.bit_length);
}

void test_dapc_address_5_level_254(void)
{
    /* addr 5: addr_byte = (5 << 1) | 0 = 0x0A */
    DaliFrame f = dali_cmd_dapc(5u, 254u);
    TEST_ASSERT_EQUAL_HEX32(0x0AFEu, f.data);
    TEST_ASSERT_EQUAL_UINT8(16u, f.bit_length);
}

void test_query_status_address_5(void)
{
    /* AGENTS.md reference: 0x0B90, 16-bit */
    /* addr 5 command: addr_byte = (5 << 1) | 1 = 0x0B */
    DaliFrame f = dali_cmd_query_status(5u);
    TEST_ASSERT_EQUAL_HEX32(0x0B90u, f.data);
    TEST_ASSERT_EQUAL_UINT8(16u, f.bit_length);
}

void test_recall_max_address_0(void)
{
    /* addr 0, cmd byte 0x05, addr_byte = (0 << 1) | 1 = 0x01 */
    DaliFrame f = dali_cmd_recall_max(0u);
    TEST_ASSERT_EQUAL_HEX32(0x0105u, f.data);
    TEST_ASSERT_EQUAL_UINT8(16u, f.bit_length);
}

void test_off_address_0(void)
{
    /* addr 0, cmd 0x00, addr_byte = 0x01 */
    DaliFrame f = dali_cmd_off(0u);
    TEST_ASSERT_EQUAL_HEX32(0x0100u, f.data);
    TEST_ASSERT_EQUAL_UINT8(16u, f.bit_length);
}

void test_broadcast_off(void)
{
    /* broadcast addr_byte = 0xFF, cmd = 0x00 */
    DaliFrame f = dali_cmd_broadcast_off();
    TEST_ASSERT_EQUAL_HEX32(0xFF00u, f.data);
    TEST_ASSERT_EQUAL_UINT8(16u, f.bit_length);
}

void test_broadcast_recall_max(void)
{
    DaliFrame f = dali_cmd_broadcast_recall_max();
    TEST_ASSERT_EQUAL_HEX32(0xFF05u, f.data);
    TEST_ASSERT_EQUAL_UINT8(16u, f.bit_length);
}

void test_parse_response_returns_value(void)
{
    uint8_t val = 0;
    DaliError err = dali_parse_response(0xAFu, &val);
    TEST_ASSERT_EQUAL(DALI_OK, err);
    TEST_ASSERT_EQUAL_HEX8(0xAFu, val);
}

void test_parse_response_null_returns_invalid(void)
{
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_parse_response(0x00u, NULL));
}

/* ---------------------------------------------------------------------------
 * DALI-2 24-bit instance command builder tests
 * --------------------------------------------------------------------------*/

void test_instance_short_addr_0_all_instances(void)
{
    /* addr 0: addr_byte = (0 << 1) | 1 = 0x01
     * instance=0xFF, cmd=0x80
     * Expected: (0x01 << 16) | (0xFF << 8) | 0x80 = 0x01FF80 */
    DaliFrame f = dali_cmd_instance(0u, 0xFFu, 0x80u);
    TEST_ASSERT_EQUAL_HEX32(0x01FF80u, f.data);
    TEST_ASSERT_EQUAL_UINT8(24u, f.bit_length);
}

void test_instance_short_addr_5_instance_0(void)
{
    /* addr 5: addr_byte = (5 << 1) | 1 = 0x0B
     * instance=0, cmd=0x65
     * Expected: (0x0B << 16) | (0x00 << 8) | 0x65 = 0x0B0065 */
    DaliFrame f = dali_cmd_instance(5u, 0u, 0x65u);
    TEST_ASSERT_EQUAL_HEX32(0x0B0065u, f.data);
    TEST_ASSERT_EQUAL_UINT8(24u, f.bit_length);
}

void test_instance_group_0_all_instances(void)
{
    /* group 0: addr_byte = 0x80 | (0 << 1) | 1 = 0x81
     * Expected: (0x81 << 16) | (0xFF << 8) | 0x80 = 0x81FF80 */
    DaliFrame f = dali_cmd_instance_group(0u, 0xFFu, 0x80u);
    TEST_ASSERT_EQUAL_HEX32(0x81FF80u, f.data);
    TEST_ASSERT_EQUAL_UINT8(24u, f.bit_length);
}

void test_instance_group_15_specific_instance(void)
{
    /* group 15: addr_byte = 0x80 | (15 << 1) | 1 = 0x80 | 0x1E | 0x01 = 0x9F
     * instance=2, cmd=0xA0
     * Expected: (0x9F << 16) | (0x02 << 8) | 0xA0 = 0x9F02A0 */
    DaliFrame f = dali_cmd_instance_group(15u, 2u, 0xA0u);
    TEST_ASSERT_EQUAL_HEX32(0x9F02A0u, f.data);
    TEST_ASSERT_EQUAL_UINT8(24u, f.bit_length);
}

void test_instance_broadcast_all_instances(void)
{
    /* broadcast addr_byte = 0xFF
     * Expected: (0xFF << 16) | (0xFF << 8) | 0x80 = 0xFFFF80 */
    DaliFrame f = dali_cmd_instance_broadcast(0xFFu, 0x80u);
    TEST_ASSERT_EQUAL_HEX32(0xFFFF80u, f.data);
    TEST_ASSERT_EQUAL_UINT8(24u, f.bit_length);
}

void test_instance_broadcast_specific_instance(void)
{
    /* broadcast, instance=1, cmd=0x81
     * Expected: (0xFF << 16) | (0x01 << 8) | 0x81 = 0xFF0181 */
    DaliFrame f = dali_cmd_instance_broadcast(1u, 0x81u);
    TEST_ASSERT_EQUAL_HEX32(0xFF0181u, f.data);
    TEST_ASSERT_EQUAL_UINT8(24u, f.bit_length);
}

/* ---------------------------------------------------------------------------
 * YES/NO response tests
 * --------------------------------------------------------------------------*/

void test_is_yes_0xff(void)
{
    TEST_ASSERT_TRUE(dali_is_yes(0xFFu));
}

void test_is_yes_0x00(void)
{
    TEST_ASSERT_FALSE(dali_is_yes(0x00u));
}

void test_is_yes_0xfe(void)
{
    /* Only 0xFF is YES; 0xFE is NO */
    TEST_ASSERT_FALSE(dali_is_yes(0xFEu));
}

/* ---------------------------------------------------------------------------
 * QUERY STATUS parse tests
 * --------------------------------------------------------------------------*/

void test_parse_status_all_clear(void)
{
    DaliStatus s;
    TEST_ASSERT_EQUAL(DALI_OK, dali_parse_status(0x00u, &s));
    TEST_ASSERT_FALSE(s.ballast_failure);
    TEST_ASSERT_FALSE(s.lamp_failure);
    TEST_ASSERT_FALSE(s.lamp_arc_power_on);
    TEST_ASSERT_FALSE(s.limit_error);
    TEST_ASSERT_FALSE(s.fade_running);
    TEST_ASSERT_FALSE(s.reset_state);
    TEST_ASSERT_FALSE(s.missing_short_address);
    TEST_ASSERT_FALSE(s.power_failure);
}

void test_parse_status_all_set(void)
{
    DaliStatus s;
    TEST_ASSERT_EQUAL(DALI_OK, dali_parse_status(0xFFu, &s));
    TEST_ASSERT_TRUE(s.ballast_failure);
    TEST_ASSERT_TRUE(s.lamp_failure);
    TEST_ASSERT_TRUE(s.lamp_arc_power_on);
    TEST_ASSERT_TRUE(s.limit_error);
    TEST_ASSERT_TRUE(s.fade_running);
    TEST_ASSERT_TRUE(s.reset_state);
    TEST_ASSERT_TRUE(s.missing_short_address);
    TEST_ASSERT_TRUE(s.power_failure);
}

void test_parse_status_lamp_failure(void)
{
    DaliStatus s;
    TEST_ASSERT_EQUAL(DALI_OK, dali_parse_status(0x02u, &s));  /* bit 1 */
    TEST_ASSERT_FALSE(s.ballast_failure);
    TEST_ASSERT_TRUE(s.lamp_failure);
    TEST_ASSERT_FALSE(s.lamp_arc_power_on);
    TEST_ASSERT_FALSE(s.limit_error);
    TEST_ASSERT_FALSE(s.fade_running);
    TEST_ASSERT_FALSE(s.reset_state);
    TEST_ASSERT_FALSE(s.missing_short_address);
    TEST_ASSERT_FALSE(s.power_failure);
}

void test_parse_status_power_failure(void)
{
    DaliStatus s;
    TEST_ASSERT_EQUAL(DALI_OK, dali_parse_status(0x80u, &s));  /* bit 7 */
    TEST_ASSERT_FALSE(s.ballast_failure);
    TEST_ASSERT_FALSE(s.lamp_failure);
    TEST_ASSERT_FALSE(s.lamp_arc_power_on);
    TEST_ASSERT_FALSE(s.limit_error);
    TEST_ASSERT_FALSE(s.fade_running);
    TEST_ASSERT_FALSE(s.reset_state);
    TEST_ASSERT_FALSE(s.missing_short_address);
    TEST_ASSERT_TRUE(s.power_failure);
}

void test_parse_status_null_returns_invalid(void)
{
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_parse_status(0xFFu, NULL));
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------*/
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dapc_address_0_level_128);
    RUN_TEST(test_dapc_address_5_level_254);
    RUN_TEST(test_query_status_address_5);
    RUN_TEST(test_recall_max_address_0);
    RUN_TEST(test_off_address_0);
    RUN_TEST(test_broadcast_off);
    RUN_TEST(test_broadcast_recall_max);
    RUN_TEST(test_parse_response_returns_value);
    RUN_TEST(test_parse_response_null_returns_invalid);
    /* 24-bit instance frame builders */
    RUN_TEST(test_instance_short_addr_0_all_instances);
    RUN_TEST(test_instance_short_addr_5_instance_0);
    RUN_TEST(test_instance_group_0_all_instances);
    RUN_TEST(test_instance_group_15_specific_instance);
    RUN_TEST(test_instance_broadcast_all_instances);
    RUN_TEST(test_instance_broadcast_specific_instance);
    /* YES/NO response */
    RUN_TEST(test_is_yes_0xff);
    RUN_TEST(test_is_yes_0x00);
    RUN_TEST(test_is_yes_0xfe);
    /* QUERY STATUS parsing */
    RUN_TEST(test_parse_status_all_clear);
    RUN_TEST(test_parse_status_all_set);
    RUN_TEST(test_parse_status_lamp_failure);
    RUN_TEST(test_parse_status_power_failure);
    RUN_TEST(test_parse_status_null_returns_invalid);
    return UNITY_END();
}
