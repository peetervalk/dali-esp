/*
 * test_gear_dt8.c — unit tests for dali_gear_dt8 (IEC 62386-209)
 */

#include "unity.h"
#include "dali_gear_dt8.h"
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

void test_enable_matches_protocol_enable_device_type_8(void)
{
    DaliFrame got      = dali_dt8_enable();
    DaliFrame expected = dali_cmd_enable_device_type(8u);
    TEST_ASSERT_EQUAL_UINT32(expected.data,       got.data);
    TEST_ASSERT_EQUAL_UINT8 (expected.bit_length, got.bit_length);
}

void test_enable_differs_from_dt6_enable(void)
{
    /* Different device type → different frame */
    DaliFrame dt6 = dali_cmd_enable_device_type(6u);
    DaliFrame dt8 = dali_dt8_enable();
    TEST_ASSERT_NOT_EQUAL(dt6.data, dt8.data);
}

/* ---------------------------------------------------------------------------
 * Opcode coverage — all 26 command frames
 * --------------------------------------------------------------------------*/

void test_temporary_xy_opcodes(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xE0u, frame_opcode(dali_dt8_set_temporary_x_coordinate(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE1u, frame_opcode(dali_dt8_set_temporary_y_coordinate(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE3u, frame_opcode(dali_dt8_x_coordinate_step_up(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE4u, frame_opcode(dali_dt8_x_coordinate_step_down(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE5u, frame_opcode(dali_dt8_y_coordinate_step_up(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE6u, frame_opcode(dali_dt8_y_coordinate_step_down(0u)));
}

void test_temporary_tc_opcodes(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xE7u, frame_opcode(dali_dt8_set_temporary_colour_temperature(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE8u, frame_opcode(dali_dt8_colour_temperature_step_cooler(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xE9u, frame_opcode(dali_dt8_colour_temperature_step_warmer(0u)));
}

void test_temporary_rgbwaf_opcodes(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xEAu, frame_opcode(dali_dt8_set_temporary_primary_n_dim_level(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xEBu, frame_opcode(dali_dt8_set_temporary_rgb_dim_level(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xECu, frame_opcode(dali_dt8_set_temporary_waf_dim_level(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xEDu, frame_opcode(dali_dt8_set_temporary_rgbwaf_control(0u)));
}

void test_activate_and_utility_opcodes(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xE2u, frame_opcode(dali_dt8_activate(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xEEu, frame_opcode(dali_dt8_copy_report_to_temporary(0u)));
}

void test_config_opcodes(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xF0u, frame_opcode(dali_dt8_store_ty_primary_n(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF1u, frame_opcode(dali_dt8_store_xy_coordinate_primary_n(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF2u, frame_opcode(dali_dt8_store_colour_temperature_tc_limit(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF3u, frame_opcode(dali_dt8_store_gear_features_status(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF5u, frame_opcode(dali_dt8_assign_colour_to_linked_channel(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF6u, frame_opcode(dali_dt8_start_auto_calibration(0u)));
}

void test_query_opcodes(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xF7u, frame_opcode(dali_dt8_query_gear_features_status(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF8u, frame_opcode(dali_dt8_query_colour_status(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xF9u, frame_opcode(dali_dt8_query_colour_type_features(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xFAu, frame_opcode(dali_dt8_query_colour_value(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xFBu, frame_opcode(dali_dt8_query_rgbwaf_control(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xFCu, frame_opcode(dali_dt8_query_assigned_colour(0u)));
    TEST_ASSERT_EQUAL_HEX8(0xFFu, frame_opcode(dali_dt8_query_extended_version_number(0u)));
}

void test_all_frames_are_16bit(void)
{
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt8_set_temporary_x_coordinate(0u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt8_activate(0u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt8_set_temporary_rgb_dim_level(0u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt8_store_gear_features_status(0u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt8_query_gear_features_status(0u).bit_length);
    TEST_ASSERT_EQUAL_UINT8(16u, dali_dt8_query_colour_value(0u).bit_length);
}

/* ---------------------------------------------------------------------------
 * Address encoding
 * --------------------------------------------------------------------------*/

void test_address_encoding(void)
{
    for (uint8_t addr = 0u; addr < 4u; addr++) {
        TEST_ASSERT_EQUAL_HEX8(addr_byte(addr),
                               frame_addr_byte(dali_dt8_set_temporary_colour_temperature(addr)));
        TEST_ASSERT_EQUAL_HEX8(addr_byte(addr),
                               frame_addr_byte(dali_dt8_query_gear_features_status(addr)));
        TEST_ASSERT_EQUAL_HEX8(addr_byte(addr),
                               frame_addr_byte(dali_dt8_activate(addr)));
    }
}

void test_address_max(void)
{
    DaliFrame f = dali_dt8_query_colour_status(DALI_MAX_SHORT_ADDRESS);
    TEST_ASSERT_EQUAL_HEX8(addr_byte(DALI_MAX_SHORT_ADDRESS), frame_addr_byte(f));
    TEST_ASSERT_EQUAL_HEX8(0xF8u, frame_opcode(f));
}

/* ---------------------------------------------------------------------------
 * encode_16 helper
 * --------------------------------------------------------------------------*/

void test_encode_16_null_returns_invalid(void)
{
    DaliFrame d, h;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, dali_dt8_encode_16(0x1234u, NULL, &h));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, dali_dt8_encode_16(0x1234u, &d, NULL));
}

void test_encode_16_splits_correctly(void)
{
    DaliFrame dtr0, dtr1;
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_dt8_encode_16(0xABCDu, &dtr0, &dtr1));

    DaliFrame expected_dtr0 = dali_cmd_dtr0_data(0xCDu);
    DaliFrame expected_dtr1 = dali_cmd_dtr1_data(0xABu);
    TEST_ASSERT_EQUAL_UINT32(expected_dtr0.data, dtr0.data);
    TEST_ASSERT_EQUAL_UINT32(expected_dtr1.data, dtr1.data);
}

void test_encode_16_zero(void)
{
    DaliFrame dtr0, dtr1;
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_dt8_encode_16(0x0000u, &dtr0, &dtr1));
    TEST_ASSERT_EQUAL_UINT32(dali_cmd_dtr0_data(0x00u).data, dtr0.data);
    TEST_ASSERT_EQUAL_UINT32(dali_cmd_dtr1_data(0x00u).data, dtr1.data);
}

void test_encode_16_max(void)
{
    DaliFrame dtr0, dtr1;
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_dt8_encode_16(0xFFFFu, &dtr0, &dtr1));
    TEST_ASSERT_EQUAL_UINT32(dali_cmd_dtr0_data(0xFFu).data, dtr0.data);
    TEST_ASSERT_EQUAL_UINT32(dali_cmd_dtr1_data(0xFFu).data, dtr1.data);
}

void test_encode_16_low_byte_only(void)
{
    DaliFrame dtr0, dtr1;
    dali_dt8_encode_16(0x00BCu, &dtr0, &dtr1);
    TEST_ASSERT_EQUAL_UINT32(dali_cmd_dtr0_data(0xBCu).data, dtr0.data);
    TEST_ASSERT_EQUAL_UINT32(dali_cmd_dtr1_data(0x00u).data, dtr1.data);
}

/* ---------------------------------------------------------------------------
 * build_dtr0_selector
 * --------------------------------------------------------------------------*/

void test_build_dtr0_selector_matches_dtr0_data(void)
{
    DaliFrame f = dali_dt8_build_dtr0_selector(DALI_DT8_VALUE_COLOUR_TEMP_TC);
    DaliFrame expected = dali_cmd_dtr0_data((uint8_t)DALI_DT8_VALUE_COLOUR_TEMP_TC);
    TEST_ASSERT_EQUAL_UINT32(expected.data, f.data);
}

void test_build_dtr0_selector_all_values(void)
{
    TEST_ASSERT_EQUAL_UINT32(
        dali_cmd_dtr0_data(0u).data,
        dali_dt8_build_dtr0_selector(DALI_DT8_VALUE_X_COORDINATE).data);
    TEST_ASSERT_EQUAL_UINT32(
        dali_cmd_dtr0_data(14u).data,
        dali_dt8_build_dtr0_selector(DALI_DT8_VALUE_FREE_COLOUR).data);
}

/* ---------------------------------------------------------------------------
 * Colour temperature conversions
 * --------------------------------------------------------------------------*/

void test_kelvin_to_mirek_known_values(void)
{
    /* 4000K ≈ 250 Mirek (1000000/4000 = 250.0) */
    TEST_ASSERT_EQUAL_UINT16(250u, dali_dt8_kelvin_to_mirek(4000u));
    /* 2700K ≈ 370 Mirek (1000000/2700 ≈ 370.37, truncated to 370) */
    TEST_ASSERT_EQUAL_UINT16(370u, dali_dt8_kelvin_to_mirek(2700u));
    /* 6500K ≈ 153 Mirek (1000000/6500 ≈ 153.84, truncated to 153) */
    TEST_ASSERT_EQUAL_UINT16(153u, dali_dt8_kelvin_to_mirek(6500u));
}

void test_mirek_to_kelvin_known_values(void)
{
    /* 250 Mirek = 4000K exactly */
    TEST_ASSERT_EQUAL_UINT16(4000u, dali_dt8_mirek_to_kelvin(250u));
    /* 154 Mirek = 6493K (1000000/154 ≈ 6493.5, truncated to 6493) */
    TEST_ASSERT_EQUAL_UINT16(6493u, dali_dt8_mirek_to_kelvin(154u));
}

void test_kelvin_to_mirek_clamps_zero(void)
{
    /* kelvin=0 must not divide by zero */
    uint16_t result = dali_dt8_kelvin_to_mirek(0u);
    TEST_ASSERT_GREATER_THAN_UINT16(0u, result);
}

void test_mirek_to_kelvin_clamps_zero(void)
{
    /* mirek=0 must not divide by zero */
    uint16_t result = dali_dt8_mirek_to_kelvin(0u);
    TEST_ASSERT_GREATER_THAN_UINT16(0u, result);
}

void test_round_trip_colour_temperature(void)
{
    /*
     * Round-trip error = Mirek truncation error * (1e6 / Mirek^2).
     * At 2700K (370 Mirek) that's ~7K per Mirek step; we allow 10K tolerance.
     * 4000K and 5000K divide evenly (250 and 200 Mirek) so error is 0.
     * Very high CCTs have larger errors and are excluded here.
     */
    uint16_t kelvins[] = {2700u, 3000u, 4000u, 5000u};
    for (size_t i = 0u; i < sizeof(kelvins) / sizeof(kelvins[0]); i++) {
        uint16_t k      = kelvins[i];
        uint16_t mirek  = dali_dt8_kelvin_to_mirek(k);
        uint16_t back   = dali_dt8_mirek_to_kelvin(mirek);
        TEST_ASSERT_INT_WITHIN(10, (int)k, (int)back);
    }
}

/* ---------------------------------------------------------------------------
 * parse_gear_features
 * --------------------------------------------------------------------------*/

void test_parse_gear_features_null_returns_invalid(void)
{
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, dali_dt8_parse_gear_features(0xFFu, NULL));
}

void test_parse_gear_features_all_zero(void)
{
    DaliDt8GearFeatures f;
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_dt8_parse_gear_features(0x00u, &f));
    TEST_ASSERT_FALSE(f.xy_capable);
    TEST_ASSERT_FALSE(f.tc_capable);
    TEST_ASSERT_FALSE(f.primary_n_capable);
    TEST_ASSERT_FALSE(f.rgbwaf_capable);
    TEST_ASSERT_FALSE(f.auto_calibration);
}

void test_parse_gear_features_all_ones(void)
{
    DaliDt8GearFeatures f;
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_dt8_parse_gear_features(0xFFu, &f));
    TEST_ASSERT_TRUE(f.xy_capable);
    TEST_ASSERT_TRUE(f.tc_capable);
    TEST_ASSERT_TRUE(f.primary_n_capable);
    TEST_ASSERT_TRUE(f.rgbwaf_capable);
    TEST_ASSERT_TRUE(f.auto_calibration);
}

void test_parse_gear_features_individual_bits(void)
{
    DaliDt8GearFeatures f;

    dali_dt8_parse_gear_features(DALI_DT8_FEATURE_XY_CAPABLE, &f);
    TEST_ASSERT_TRUE (f.xy_capable);
    TEST_ASSERT_FALSE(f.tc_capable);

    dali_dt8_parse_gear_features(DALI_DT8_FEATURE_TC_CAPABLE, &f);
    TEST_ASSERT_FALSE(f.xy_capable);
    TEST_ASSERT_TRUE (f.tc_capable);
    TEST_ASSERT_FALSE(f.primary_n_capable);

    dali_dt8_parse_gear_features(DALI_DT8_FEATURE_PRIMARY_N_CAPABLE, &f);
    TEST_ASSERT_TRUE (f.primary_n_capable);
    TEST_ASSERT_FALSE(f.rgbwaf_capable);

    dali_dt8_parse_gear_features(DALI_DT8_FEATURE_RGBWAF_CAPABLE, &f);
    TEST_ASSERT_TRUE (f.rgbwaf_capable);
    TEST_ASSERT_FALSE(f.auto_calibration);

    dali_dt8_parse_gear_features(DALI_DT8_FEATURE_AUTO_CALIBRATION, &f);
    TEST_ASSERT_TRUE (f.auto_calibration);
    TEST_ASSERT_FALSE(f.rgbwaf_capable);
}

void test_parse_gear_features_common_tc_only_device(void)
{
    /* A typical tunable-white LED: Tc capable but not XY or RGBWAF */
    DaliDt8GearFeatures f;
    dali_dt8_parse_gear_features(DALI_DT8_FEATURE_TC_CAPABLE, &f);
    TEST_ASSERT_FALSE(f.xy_capable);
    TEST_ASSERT_TRUE (f.tc_capable);
    TEST_ASSERT_FALSE(f.primary_n_capable);
    TEST_ASSERT_FALSE(f.rgbwaf_capable);
}

/* ---------------------------------------------------------------------------
 * parse_colour_status
 * --------------------------------------------------------------------------*/

void test_parse_colour_status_null_returns_invalid(void)
{
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, dali_dt8_parse_colour_status(0x00u, NULL));
}

void test_parse_colour_status_all_zero(void)
{
    DaliDt8ColourStatus s;
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_dt8_parse_colour_status(0x00u, &s));
    TEST_ASSERT_FALSE(s.xy_active);
    TEST_ASSERT_FALSE(s.tc_active);
    TEST_ASSERT_FALSE(s.primary_n_active);
    TEST_ASSERT_FALSE(s.rgbwaf_active);
    TEST_ASSERT_FALSE(s.tc_out_of_range);
}

void test_parse_colour_status_individual_bits(void)
{
    DaliDt8ColourStatus s;

    dali_dt8_parse_colour_status(DALI_DT8_STATUS_XY_ACTIVE, &s);
    TEST_ASSERT_TRUE (s.xy_active);
    TEST_ASSERT_FALSE(s.tc_active);

    dali_dt8_parse_colour_status(DALI_DT8_STATUS_TC_ACTIVE, &s);
    TEST_ASSERT_FALSE(s.xy_active);
    TEST_ASSERT_TRUE (s.tc_active);
    TEST_ASSERT_FALSE(s.primary_n_active);

    dali_dt8_parse_colour_status(DALI_DT8_STATUS_PRIMARY_N_ACTIVE, &s);
    TEST_ASSERT_TRUE (s.primary_n_active);
    TEST_ASSERT_FALSE(s.rgbwaf_active);

    dali_dt8_parse_colour_status(DALI_DT8_STATUS_RGBWAF_ACTIVE, &s);
    TEST_ASSERT_TRUE (s.rgbwaf_active);
    TEST_ASSERT_FALSE(s.tc_out_of_range);

    dali_dt8_parse_colour_status(DALI_DT8_STATUS_TC_OUT_OF_RANGE, &s);
    TEST_ASSERT_TRUE (s.tc_out_of_range);
    TEST_ASSERT_FALSE(s.rgbwaf_active);
}

void test_parse_colour_status_tc_with_out_of_range(void)
{
    DaliDt8ColourStatus s;
    uint8_t raw = (uint8_t)(DALI_DT8_STATUS_TC_ACTIVE | DALI_DT8_STATUS_TC_OUT_OF_RANGE);
    dali_dt8_parse_colour_status(raw, &s);
    TEST_ASSERT_TRUE(s.tc_active);
    TEST_ASSERT_TRUE(s.tc_out_of_range);
    TEST_ASSERT_FALSE(s.xy_active);
}

/* ---------------------------------------------------------------------------
 * dali_dt8_read_colour_value_16 — mock transport
 * --------------------------------------------------------------------------*/

typedef struct {
    uint32_t  data;
    uint8_t   bit_length;
    bool      no_reply;
    uint8_t   reply;
    DaliError err;
    bool      consumed;
} Dt8MockEntry;

static Dt8MockEntry s_dt8_script[8];
static uint8_t s_dt8_count;

static DaliError dt8_mock_transact(const DaliFrame *frame,
                                   bool             needs_reply,
                                   uint8_t          retries_left,
                                   bool             send_twice,
                                   DaliFrame       *reply_out,
                                   void            *ctx)
{
    (void)retries_left;
    (void)ctx;
    TEST_ASSERT_FALSE(send_twice);

    for (uint8_t i = 0u; i < s_dt8_count; i++) {
        if (!s_dt8_script[i].consumed &&
            s_dt8_script[i].data == frame->data &&
            s_dt8_script[i].bit_length == frame->bit_length) {
            s_dt8_script[i].consumed = true;
            if (!needs_reply) {
                return DALI_OK;
            }
            TEST_ASSERT_NOT_NULL(reply_out);
            if (s_dt8_script[i].err != DALI_OK) {
                return s_dt8_script[i].err;
            }
            *reply_out = (DaliFrame){
                .data = s_dt8_script[i].reply,
                .bit_length = DALI_BACKWARD_FRAME_BITS,
            };
            return DALI_OK;
        }
    }
    return DALI_ERR_TIMEOUT;
}

static void dt8_mock_reset(void)
{
    memset(s_dt8_script, 0, sizeof(s_dt8_script));
    s_dt8_count = 0u;
}

static void dt8_add_no_reply(uint32_t data, uint8_t bit_length)
{
    TEST_ASSERT_LESS_THAN_UINT8((uint8_t)(sizeof(s_dt8_script) / sizeof(s_dt8_script[0])),
                                s_dt8_count);
    s_dt8_script[s_dt8_count++] = (Dt8MockEntry){
        .data = data, .bit_length = bit_length, .no_reply = true,
    };
}

static void dt8_add_reply(uint32_t data, uint8_t bit_length, uint8_t reply)
{
    TEST_ASSERT_LESS_THAN_UINT8((uint8_t)(sizeof(s_dt8_script) / sizeof(s_dt8_script[0])),
                                s_dt8_count);
    s_dt8_script[s_dt8_count++] = (Dt8MockEntry){
        .data = data, .bit_length = bit_length, .reply = reply, .err = DALI_OK,
    };
}

static void dt8_add_error(uint32_t data, uint8_t bit_length, DaliError err)
{
    TEST_ASSERT_LESS_THAN_UINT8((uint8_t)(sizeof(s_dt8_script) / sizeof(s_dt8_script[0])),
                                s_dt8_count);
    s_dt8_script[s_dt8_count++] = (Dt8MockEntry){
        .data = data, .bit_length = bit_length, .err = err,
    };
}

static DaliDt8Transport dt8_transport(void)
{
    return (DaliDt8Transport){ .transact = dt8_mock_transact, .ctx = NULL };
}

/* ---------------------------------------------------------------------------
 * dali_dt8_read_colour_value_16 tests
 * --------------------------------------------------------------------------*/

void test_read_colour_value_16_rejects_invalid_args(void)
{
    DaliDt8Transport t = dt8_transport();
    uint16_t out = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_dt8_read_colour_value_16(NULL, 5u, DALI_DT8_VALUE_X_COORDINATE, &out));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_dt8_read_colour_value_16(&t, DALI_SHORT_ADDRESS_COUNT,
                                                        DALI_DT8_VALUE_X_COORDINATE, &out));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_dt8_read_colour_value_16(&t, 5u, DALI_DT8_VALUE_X_COORDINATE, NULL));
}

void test_read_colour_value_16_returns_combined_word(void)
{
    /* Sequence for addr=5, selector=X_COORDINATE:
     *   1. DTR0 = 0 (selector for X)       — no reply
     *   2. ENABLE DT8                       — no reply
     *   3. QueryColourValue at addr 5       — reply = 0xAB (MSB)
     *   4. QUERY_CONTENT_DTR0 at addr 5    — reply = 0xCD (LSB)
     *   result = 0xABCD */
    dt8_mock_reset();

    DaliFrame dtr0_sel = dali_dt8_build_dtr0_selector(DALI_DT8_VALUE_X_COORDINATE);
    DaliFrame enable   = dali_dt8_enable();
    DaliFrame qcv      = dali_dt8_query_colour_value(5u);
    /* QUERY_CONTENT_DTR0 at addr 5: address_byte = 0x0B, opcode = 0x98 */
    uint32_t qdtr0_data = (uint32_t)(0x0Bu << 8u) | 0x98u;

    dt8_add_no_reply(dtr0_sel.data, dtr0_sel.bit_length);
    dt8_add_no_reply(enable.data,   enable.bit_length);
    dt8_add_reply   (qcv.data,      qcv.bit_length,    0xABu);
    dt8_add_reply   (qdtr0_data,    DALI_FORWARD_FRAME_BITS, 0xCDu);

    DaliDt8Transport t = dt8_transport();
    uint16_t result = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_dt8_read_colour_value_16(&t, 5u,
                                                        DALI_DT8_VALUE_X_COORDINATE, &result));
    TEST_ASSERT_EQUAL_HEX16(0xABCDu, result);
}

void test_read_colour_value_16_uses_selector_for_dtr0(void)
{
    /* Selector=COLOUR_TEMP_TC (=2) must set DTR0=2, not DTR0=0 */
    dt8_mock_reset();

    DaliFrame dtr0_sel = dali_dt8_build_dtr0_selector(DALI_DT8_VALUE_COLOUR_TEMP_TC);
    DaliFrame enable   = dali_dt8_enable();
    DaliFrame qcv      = dali_dt8_query_colour_value(0u);
    uint32_t qdtr0_data = (uint32_t)(0x01u << 8u) | 0x98u;  /* addr 0: byte = 0x01 */

    dt8_add_no_reply(dtr0_sel.data, dtr0_sel.bit_length);
    dt8_add_no_reply(enable.data,   enable.bit_length);
    dt8_add_reply   (qcv.data,      qcv.bit_length,          0x15u);
    dt8_add_reply   (qdtr0_data,    DALI_FORWARD_FRAME_BITS, 0x00u);

    DaliDt8Transport t = dt8_transport();
    uint16_t result = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_dt8_read_colour_value_16(&t, 0u,
                                                        DALI_DT8_VALUE_COLOUR_TEMP_TC, &result));
    TEST_ASSERT_EQUAL_HEX16(0x1500u, result);
}

void test_read_colour_value_16_propagates_query_error(void)
{
    dt8_mock_reset();

    DaliFrame dtr0_sel = dali_dt8_build_dtr0_selector(DALI_DT8_VALUE_X_COORDINATE);
    DaliFrame enable   = dali_dt8_enable();
    DaliFrame qcv      = dali_dt8_query_colour_value(5u);

    dt8_add_no_reply(dtr0_sel.data, dtr0_sel.bit_length);
    dt8_add_no_reply(enable.data,   enable.bit_length);
    dt8_add_error   (qcv.data,      qcv.bit_length, DALI_ERR_TIMEOUT);

    DaliDt8Transport t = dt8_transport();
    uint16_t result = 0xFFFFu;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_TIMEOUT,
                          dali_dt8_read_colour_value_16(&t, 5u,
                                                        DALI_DT8_VALUE_X_COORDINATE, &result));
}

/* ---------------------------------------------------------------------------
 * Runner
 * --------------------------------------------------------------------------*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_enable_matches_protocol_enable_device_type_8);
    RUN_TEST(test_enable_differs_from_dt6_enable);

    RUN_TEST(test_temporary_xy_opcodes);
    RUN_TEST(test_temporary_tc_opcodes);
    RUN_TEST(test_temporary_rgbwaf_opcodes);
    RUN_TEST(test_activate_and_utility_opcodes);
    RUN_TEST(test_config_opcodes);
    RUN_TEST(test_query_opcodes);
    RUN_TEST(test_all_frames_are_16bit);

    RUN_TEST(test_address_encoding);
    RUN_TEST(test_address_max);

    RUN_TEST(test_encode_16_null_returns_invalid);
    RUN_TEST(test_encode_16_splits_correctly);
    RUN_TEST(test_encode_16_zero);
    RUN_TEST(test_encode_16_max);
    RUN_TEST(test_encode_16_low_byte_only);

    RUN_TEST(test_build_dtr0_selector_matches_dtr0_data);
    RUN_TEST(test_build_dtr0_selector_all_values);

    RUN_TEST(test_kelvin_to_mirek_known_values);
    RUN_TEST(test_mirek_to_kelvin_known_values);
    RUN_TEST(test_kelvin_to_mirek_clamps_zero);
    RUN_TEST(test_mirek_to_kelvin_clamps_zero);
    RUN_TEST(test_round_trip_colour_temperature);

    RUN_TEST(test_parse_gear_features_null_returns_invalid);
    RUN_TEST(test_parse_gear_features_all_zero);
    RUN_TEST(test_parse_gear_features_all_ones);
    RUN_TEST(test_parse_gear_features_individual_bits);
    RUN_TEST(test_parse_gear_features_common_tc_only_device);

    RUN_TEST(test_parse_colour_status_null_returns_invalid);
    RUN_TEST(test_parse_colour_status_all_zero);
    RUN_TEST(test_parse_colour_status_individual_bits);
    RUN_TEST(test_parse_colour_status_tc_with_out_of_range);

    RUN_TEST(test_read_colour_value_16_rejects_invalid_args);
    RUN_TEST(test_read_colour_value_16_returns_combined_word);
    RUN_TEST(test_read_colour_value_16_uses_selector_for_dtr0);
    RUN_TEST(test_read_colour_value_16_propagates_query_error);

    return UNITY_END();
}
