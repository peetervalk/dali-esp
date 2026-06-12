#include "unity.h"
#include "dali_phy.h"
#include "dali_frame.h"

void setUp(void)  {}
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Helper: derive edge-interval sequence from a Manchester half-bit buffer.
 *
 * hb[0] is the first half of the start bit (always LOW).
 * The first edge is at index 0 (idle HIGH into start LOW).
 * An edge occurs wherever hb[i] != hb[i-1].
 * Each interval is (number of half-bit periods) × DALI_HALF_BIT_US µs.
 *
 * Returns the number of intervals written to out[], or 0 on overflow.
 * --------------------------------------------------------------------------*/
static uint8_t half_bits_to_intervals(const uint8_t *hb, uint8_t hb_len,
                                       uint32_t *out, uint8_t out_max)
{
    uint8_t n        = 0u;
    uint8_t last_idx = 0u;   /* index of the first edge (leading start edge) */

    for (uint8_t i = 1u; i < hb_len; i++) {
        if (hb[i] != hb[i - 1u]) {
            if (n >= out_max) { return 0u; }
            out[n++] = (uint32_t)(i - last_idx) * (uint32_t)DALI_HALF_BIT_US;
            last_idx = i;
        }
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Helper: encode → derive intervals → decode → assert match.
 * --------------------------------------------------------------------------*/
static void assert_round_trip(uint32_t data_val, uint8_t bit_length)
{
    DaliFrame enc = { .data = data_val, .bit_length = bit_length };
    uint8_t   hb[64];
    uint32_t  ivs[64];

    uint8_t hb_len = dali_phy_encode_manchester(&enc, hb, (uint8_t)sizeof(hb));
    TEST_ASSERT_NOT_EQUAL_UINT8(0u, hb_len);

    uint8_t n_ivs = half_bits_to_intervals(hb, hb_len, ivs, 64u);
    TEST_ASSERT_NOT_EQUAL_UINT8(0u, n_ivs);

    DaliFrame dec;
    DaliError err = dali_phy_decode_manchester(ivs, n_ivs, &dec);
    TEST_ASSERT_EQUAL_INT(DALI_OK, err);
    TEST_ASSERT_EQUAL_UINT8(bit_length, dec.bit_length);
    TEST_ASSERT_EQUAL_UINT32(data_val, dec.data);
}

void test_decode_lunatone_broadcast_off_capture(void)
{
    const uint32_t ivs[] = {
        379u, 454u, 378u, 455u, 378u, 455u, 379u, 455u,
        378u, 455u, 378u, 455u, 378u, 455u, 378u, 455u,
        378u, 872u, 378u, 455u, 378u, 455u, 378u, 455u,
        378u, 455u, 378u, 455u, 379u, 454u, 379u, 455u,
        378u,
    };
    const uint8_t levels[] = {
        0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u,
        0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u,
        0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u,
        0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u,
        0u, 1u,
    };
    DaliFrame dec;
    DaliError err = dali_phy_decode_manchester_edges(
        ivs, (uint8_t)(sizeof(ivs) / sizeof(ivs[0])),
        levels, (uint8_t)(sizeof(levels) / sizeof(levels[0])),
        &dec);

    TEST_ASSERT_EQUAL_INT(DALI_OK, err);
    TEST_ASSERT_EQUAL_UINT8(16u, dec.bit_length);
    TEST_ASSERT_EQUAL_UINT32(0xFF00u, dec.data);
}

/* ---------------------------------------------------------------------------
 * Argument validation
 * --------------------------------------------------------------------------*/

void test_decode_null_intervals_returns_invalid(void)
{
    DaliFrame f;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_phy_decode_manchester(NULL, 4u, &f));
}

void test_decode_null_frame_out_returns_invalid(void)
{
    uint32_t ivs[4] = { 417u, 417u, 417u, 417u };
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_phy_decode_manchester(ivs, 4u, NULL));
}

void test_decode_zero_count_returns_invalid(void)
{
    uint32_t ivs[4] = { 417u, 417u, 417u, 417u };
    DaliFrame f;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_phy_decode_manchester(ivs, 0u, &f));
}

/* ---------------------------------------------------------------------------
 * Malformed input
 * --------------------------------------------------------------------------*/

void test_decode_interval_too_short_returns_malformed(void)
{
    /* 1 µs is far outside both SHORT (312–521) and LONG (625–1041) windows */
    uint32_t ivs[1] = { 1u };
    DaliFrame f;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_MALFORMED,
                          dali_phy_decode_manchester(ivs, 1u, &f));
}

void test_decode_interval_between_windows_returns_malformed(void)
{
    /* 580 µs: too long for SHORT (max 521), too short for LONG (min 625) */
    uint32_t ivs[1] = { 580u };
    DaliFrame f;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_MALFORMED,
                          dali_phy_decode_manchester(ivs, 1u, &f));
}

void test_decode_interval_too_long_returns_malformed(void)
{
    /* 2000 µs: beyond the LONG window maximum (1041 µs) */
    uint32_t ivs[1] = { 2000u };
    DaliFrame f;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_MALFORMED,
                          dali_phy_decode_manchester(ivs, 1u, &f));
}

/* ---------------------------------------------------------------------------
 * Round-trip decode tests — 8-bit backward / response frames
 * --------------------------------------------------------------------------*/

void test_decode_8bit_all_zeros(void)        { assert_round_trip(0x00u, 8u); }
void test_decode_8bit_all_ones(void)         { assert_round_trip(0xFFu, 8u); }
void test_decode_8bit_alternating_AA(void)   { assert_round_trip(0xAAu, 8u); }
void test_decode_8bit_alternating_55(void)   { assert_round_trip(0x55u, 8u); }
void test_decode_8bit_response_0xAF(void)    { assert_round_trip(0xAFu, 8u); }

/* ---------------------------------------------------------------------------
 * Round-trip decode tests — 16-bit standard DALI forward frames
 * --------------------------------------------------------------------------*/

void test_decode_16bit_dapc_addr0_lvl128(void)  { assert_round_trip(0x0080u, 16u); }
void test_decode_16bit_query_status_addr5(void) { assert_round_trip(0x0B90u, 16u); }
void test_decode_16bit_broadcast_off(void)      { assert_round_trip(0xFF00u, 16u); }
void test_decode_16bit_all_zeros(void)          { assert_round_trip(0x0000u, 16u); }
void test_decode_16bit_all_ones(void)           { assert_round_trip(0xFFFFu, 16u); }
void test_decode_16bit_alternating_AAAA(void)   { assert_round_trip(0xAAAAu, 16u); }
void test_decode_16bit_alternating_5555(void)   { assert_round_trip(0x5555u, 16u); }

/* ---------------------------------------------------------------------------
 * Round-trip decode tests — 24-bit DALI-2 extended frames
 * --------------------------------------------------------------------------*/

void test_decode_24bit_example(void)          { assert_round_trip(0x123456u, 24u); }
void test_decode_24bit_all_zeros(void)        { assert_round_trip(0x000000u, 24u); }
void test_decode_24bit_all_ones(void)         { assert_round_trip(0xFFFFFFu, 24u); }
void test_decode_24bit_alternating_AA(void)   { assert_round_trip(0xAAAAAAu, 24u); }
void test_decode_24bit_alternating_55(void)   { assert_round_trip(0x555555u, 24u); }

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------*/
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_decode_null_intervals_returns_invalid);
    RUN_TEST(test_decode_null_frame_out_returns_invalid);
    RUN_TEST(test_decode_zero_count_returns_invalid);
    RUN_TEST(test_decode_interval_too_short_returns_malformed);
    RUN_TEST(test_decode_interval_between_windows_returns_malformed);
    RUN_TEST(test_decode_interval_too_long_returns_malformed);
    RUN_TEST(test_decode_lunatone_broadcast_off_capture);

    RUN_TEST(test_decode_8bit_all_zeros);
    RUN_TEST(test_decode_8bit_all_ones);
    RUN_TEST(test_decode_8bit_alternating_AA);
    RUN_TEST(test_decode_8bit_alternating_55);
    RUN_TEST(test_decode_8bit_response_0xAF);

    RUN_TEST(test_decode_16bit_dapc_addr0_lvl128);
    RUN_TEST(test_decode_16bit_query_status_addr5);
    RUN_TEST(test_decode_16bit_broadcast_off);
    RUN_TEST(test_decode_16bit_all_zeros);
    RUN_TEST(test_decode_16bit_all_ones);
    RUN_TEST(test_decode_16bit_alternating_AAAA);
    RUN_TEST(test_decode_16bit_alternating_5555);

    RUN_TEST(test_decode_24bit_example);
    RUN_TEST(test_decode_24bit_all_zeros);
    RUN_TEST(test_decode_24bit_all_ones);
    RUN_TEST(test_decode_24bit_alternating_AA);
    RUN_TEST(test_decode_24bit_alternating_55);

    return UNITY_END();
}
