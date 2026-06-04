#include "unity.h"
#include "dali_phy.h"

void setUp(void)  {}
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Manchester encode tests
 *
 * IEC 62386 rule:
 *   Bit '1': HIGH first half, LOW  second half  → {1, 0}
 *   Bit '0': LOW  first half, HIGH second half  → {0, 1}
 *   Start bit (always '1'):                     → {1, 0}
 *   Stop bits (2× full HIGH):                   → {1, 1, 1, 1}
 * --------------------------------------------------------------------------*/

void test_encode_single_zero_bit(void)
{
    /* 1-bit frame, data = 0 */
    DaliFrame f = { .data = 0x00u, .bit_length = 1u };
    uint8_t buf[16];
    uint8_t len = dali_phy_encode_manchester(&f, buf, sizeof(buf));

    /* (1 start + 1 data + 2 stop) * 2 = 8 half-bits */
    TEST_ASSERT_EQUAL_UINT8(8u, len);

    /* start bit: {1, 0} */
    TEST_ASSERT_EQUAL_UINT8(1u, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0u, buf[1]);

    /* data bit '0': {0, 1} */
    TEST_ASSERT_EQUAL_UINT8(0u, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[3]);

    /* stop bits: {1, 1, 1, 1} */
    TEST_ASSERT_EQUAL_UINT8(1u, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[5]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[6]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[7]);
}

void test_encode_single_one_bit(void)
{
    DaliFrame f = { .data = 0x01u, .bit_length = 1u };
    uint8_t buf[16];
    uint8_t len = dali_phy_encode_manchester(&f, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT8(8u, len);

    /* start: {1, 0} */
    TEST_ASSERT_EQUAL_UINT8(1u, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0u, buf[1]);

    /* data bit '1': {1, 0} */
    TEST_ASSERT_EQUAL_UINT8(1u, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0u, buf[3]);
}

void test_encode_16bit_dapc_frame(void)
{
    /*
     * Frame: DAPC address 0, level 128 → 0x0080, 16-bit
     * Expected: (1 + 16 + 2) * 2 = 38 half-bits
     */
    DaliFrame f = { .data = 0x0080u, .bit_length = 16u };
    uint8_t buf[64];
    uint8_t len = dali_phy_encode_manchester(&f, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT8(38u, len);

    /* Verify start bit */
    TEST_ASSERT_EQUAL_UINT8(1u, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0u, buf[1]);

    /* Verify last two stop bit half-pairs are all HIGH */
    TEST_ASSERT_EQUAL_UINT8(1u, buf[34]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[35]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[36]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[37]);
}

void test_encode_24bit_frame(void)
{
    /* 24-bit DALI-2 extended frame: (1 + 24 + 2) * 2 = 54 half-bits */
    DaliFrame f = { .data = 0x123456u, .bit_length = 24u };
    uint8_t buf[64];
    uint8_t len = dali_phy_encode_manchester(&f, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT8(54u, len);
}

void test_encode_msb_first(void)
{
    /*
     * Frame: 0b10 (2-bit) — MSB is '1', LSB is '0'.
     * Half-bits after start: [1,0] (bit '1') then [0,1] (bit '0')
     */
    DaliFrame f = { .data = 0x02u, .bit_length = 2u };
    uint8_t buf[16];
    dali_phy_encode_manchester(&f, buf, sizeof(buf));

    /* start: {1,0} at [0,1] */
    /* MSB bit 1 → {1,0} at [2,3] */
    TEST_ASSERT_EQUAL_UINT8(1u, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0u, buf[3]);
    /* LSB bit 0 → {0,1} at [4,5] */
    TEST_ASSERT_EQUAL_UINT8(0u, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(1u, buf[5]);
}

void test_encode_null_frame_returns_zero(void)
{
    uint8_t buf[16];
    TEST_ASSERT_EQUAL_UINT8(0u, dali_phy_encode_manchester(NULL, buf, sizeof(buf)));
}

void test_encode_buffer_too_small_returns_zero(void)
{
    DaliFrame f = { .data = 0x0080u, .bit_length = 16u };
    uint8_t buf[4];   /* way too small */
    TEST_ASSERT_EQUAL_UINT8(0u, dali_phy_encode_manchester(&f, buf, sizeof(buf)));
}

void test_encode_zero_bit_length_returns_zero(void)
{
    DaliFrame f = { .data = 0x00u, .bit_length = 0u };
    uint8_t buf[16];
    TEST_ASSERT_EQUAL_UINT8(0u, dali_phy_encode_manchester(&f, buf, sizeof(buf)));
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------*/
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_single_zero_bit);
    RUN_TEST(test_encode_single_one_bit);
    RUN_TEST(test_encode_16bit_dapc_frame);
    RUN_TEST(test_encode_24bit_frame);
    RUN_TEST(test_encode_msb_first);
    RUN_TEST(test_encode_null_frame_returns_zero);
    RUN_TEST(test_encode_buffer_too_small_returns_zero);
    RUN_TEST(test_encode_zero_bit_length_returns_zero);
    return UNITY_END();
}
