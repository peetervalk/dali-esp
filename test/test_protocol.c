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
    return UNITY_END();
}
