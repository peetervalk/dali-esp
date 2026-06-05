#include "unity.h"
#include "dali_lunatone.h"
#include "dali_steinel.h"

void setUp(void) {}
void tearDown(void) {}

void test_lunatone_lookup_unit_query_is_vendor_specific(void)
{
    const DaliLunatoneCommandInfo *cmd =
        dali_lunatone_command_lookup(DALI_LUNATONE_QUERY_UNIT);

    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("LUNATONE QUERY UNIT", cmd->name);
    TEST_ASSERT_EQUAL_UINT8(0x46u, cmd->opcode);
    TEST_ASSERT_EQUAL(DALI_RESP_UINT8, cmd->response_kind);
    TEST_ASSERT_EQUAL_PTR(cmd, dali_lunatone_command_lookup_opcode(0x46u));

    TEST_ASSERT_NULL(dali_command_lookup_opcode(DALI_CMD_FRAME_24BIT_INST, 0x46u));
}

void test_lunatone_build_instance_query_unit(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_lunatone_build_instance_command(5u, 0u,
                                                           DALI_LUNATONE_QUERY_UNIT,
                                                           &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B0046u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(24u, frame.bit_length);
}

void test_lunatone_build_instance_rejects_invalid_args(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_lunatone_build_instance_command(64u, 0u,
                                                           DALI_LUNATONE_QUERY_UNIT,
                                                           &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_lunatone_build_instance_command(5u, 32u,
                                                           DALI_LUNATONE_QUERY_UNIT,
                                                           &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_lunatone_build_instance_command(5u, 0u,
                                                           DALI_LUNATONE_QUERY_UNIT,
                                                           NULL));
    TEST_ASSERT_NULL(dali_lunatone_command_lookup_opcode(0x8Cu));
}

void test_steinel_hf360_profile_expected_instances(void)
{
    const DaliSteinelInstanceInfo *brightness =
        dali_steinel_hf360_instance_lookup(DALI_STEINEL_HF360_INSTANCE_BRIGHTNESS);
    const DaliSteinelInstanceInfo *humidity =
        dali_steinel_hf360_instance_lookup(DALI_STEINEL_HF360_INSTANCE_HUMIDITY);

    TEST_ASSERT_NOT_NULL(brightness);
    TEST_ASSERT_EQUAL_UINT8(0u, brightness->instance);
    TEST_ASSERT_EQUAL_UINT8(4u, brightness->type);
    TEST_ASSERT_EQUAL_STRING("brightness", brightness->name);

    TEST_ASSERT_NOT_NULL(humidity);
    TEST_ASSERT_EQUAL_UINT8(3u, humidity->instance);
    TEST_ASSERT_EQUAL_UINT8(0u, humidity->type);

    TEST_ASSERT_TRUE(dali_steinel_hf360_expected_instance(1u, 3u));
    TEST_ASSERT_FALSE(dali_steinel_hf360_expected_instance(1u, 4u));
    TEST_ASSERT_NULL(dali_steinel_hf360_instance_lookup(4u));
}

void test_steinel_temperature_and_humidity_conversions(void)
{
    TEST_ASSERT_EQUAL_INT32(-50, dali_steinel_temperature_deci_c(0u));
    TEST_ASSERT_EQUAL_INT32(200, dali_steinel_temperature_deci_c(250u));
    TEST_ASSERT_EQUAL_UINT32(500u, dali_steinel_humidity_deci_percent(100u));

    TEST_ASSERT_FLOAT_WITHIN(0.01f, -5.0f, dali_steinel_temperature_c(0u));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, dali_steinel_temperature_c(250u));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, dali_steinel_humidity_percent(100u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lunatone_lookup_unit_query_is_vendor_specific);
    RUN_TEST(test_lunatone_build_instance_query_unit);
    RUN_TEST(test_lunatone_build_instance_rejects_invalid_args);
    RUN_TEST(test_steinel_hf360_profile_expected_instances);
    RUN_TEST(test_steinel_temperature_and_humidity_conversions);
    return UNITY_END();
}
