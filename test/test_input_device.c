#include "unity.h"
#include "dali_input_device.h"

void setUp(void) {}
void tearDown(void) {}

void test_build_query_number_of_instances(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_query_number_of_instances(5u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0BFE35u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(24u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_build_device_command(5u,
                                                DALI_CMD_QUERY_NUMBER_OF_INSTANCES,
                                                &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0BFE35u, frame.data);
}

void test_build_standard_instance_queries(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_query_instance_type(5u, 2u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B0280u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(24u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_query_resolution(5u, 2u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B0281u, frame.data);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_query_instance_error(5u, 2u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B0282u, frame.data);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_query_instance_status(5u, 2u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B0283u, frame.data);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_build_query_instance_enabled(5u, 2u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B0286u, frame.data);
}

void test_build_queries_reject_invalid_args(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_build_query_number_of_instances(64u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_build_query_number_of_instances(0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_build_query_instance_type(5u, 32u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_build_query_instance_type(5u, 0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_device_command(5u, DALI_CMD_QUERY_INSTANCE_TYPE, &frame));
}

void test_classify_standard_light_and_occupancy(void)
{
    DaliInputInstanceInfo info;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_classify_instance(0u,
                                                   DALI_INPUT_INSTANCE_TYPE_LIGHT,
                                                   &info));
    TEST_ASSERT_EQUAL_UINT8(0u, info.instance);
    TEST_ASSERT_TRUE(info.has_type);
    TEST_ASSERT_EQUAL_UINT8(DALI_INPUT_INSTANCE_TYPE_LIGHT, info.type);
    TEST_ASSERT_EQUAL(DALI_INPUT_ROLE_LIGHT, info.role);
    TEST_ASSERT_EQUAL(DALI_INPUT_ROLE_SOURCE_STANDARD_TYPE, info.role_source);
    TEST_ASSERT_EQUAL(DALI_INPUT_USABLE_STANDARD, info.usable);
    TEST_ASSERT_EQUAL_STRING("light", dali_input_role_name(info.role));
    TEST_ASSERT_EQUAL_STRING("standard", dali_input_usable_name(info.usable));

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_classify_instance(1u,
                                                   DALI_INPUT_INSTANCE_TYPE_OCCUPANCY,
                                                   &info));
    TEST_ASSERT_EQUAL(DALI_INPUT_ROLE_OCCUPANCY, info.role);
    TEST_ASSERT_EQUAL(DALI_INPUT_USABLE_STANDARD, info.usable);
    TEST_ASSERT_EQUAL_STRING("occupancy", dali_input_type_name(info.type));
}

void test_classify_generic_and_unknown_are_unverified(void)
{
    DaliInputInstanceInfo info;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_classify_instance(2u,
                                                   DALI_INPUT_INSTANCE_TYPE_GENERIC,
                                                   &info));
    TEST_ASSERT_EQUAL(DALI_INPUT_ROLE_GENERIC, info.role);
    TEST_ASSERT_EQUAL(DALI_INPUT_ROLE_SOURCE_STANDARD_TYPE, info.role_source);
    TEST_ASSERT_EQUAL(DALI_INPUT_USABLE_DISCOVERED_ONLY, info.usable);
    TEST_ASSERT_EQUAL_STRING("unverified", dali_input_usable_name(info.usable));

    TEST_ASSERT_EQUAL(DALI_OK, dali_input_classify_instance(3u, 99u, &info));
    TEST_ASSERT_EQUAL(DALI_INPUT_ROLE_UNKNOWN, info.role);
    TEST_ASSERT_EQUAL(DALI_INPUT_ROLE_SOURCE_UNKNOWN, info.role_source);
    TEST_ASSERT_EQUAL(DALI_INPUT_USABLE_DISCOVERED_ONLY, info.usable);
    TEST_ASSERT_EQUAL_STRING("unknown", dali_input_type_name(info.type));
}

void test_classify_rejects_invalid_args(void)
{
    DaliInputInstanceInfo info;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_classify_instance(DALI_INPUT_MAX_INSTANCES,
                                                   DALI_INPUT_INSTANCE_TYPE_LIGHT,
                                                   &info));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_classify_instance(0u,
                                                   DALI_INPUT_INSTANCE_TYPE_LIGHT,
                                                   NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_query_number_of_instances);
    RUN_TEST(test_build_standard_instance_queries);
    RUN_TEST(test_build_queries_reject_invalid_args);
    RUN_TEST(test_classify_standard_light_and_occupancy);
    RUN_TEST(test_classify_generic_and_unknown_are_unverified);
    RUN_TEST(test_classify_rejects_invalid_args);
    return UNITY_END();
}
