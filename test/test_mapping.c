#include "unity.h"
#include "dali_mapping.h"

void setUp(void) {}
void tearDown(void) {}

static DaliTarget target(DaliAddressType type, uint8_t address)
{
    return (DaliTarget){ .type = type, .address = address };
}

void test_validate_output_mapping_targets(void)
{
    DaliMappingEntry short_output = {
        .id            = 1u,
        .kind          = DALI_MAPPING_OUTPUT,
        .target        = target(DALI_ADDR_SHORT, 5u),
        .instance      = DALI_MAPPING_INSTANCE_UNUSED,
        .instance_type = DALI_MAPPING_INSTANCE_UNUSED,
    };
    DaliMappingEntry group_output = short_output;
    DaliMappingEntry bad_output = short_output;

    group_output.target = target(DALI_ADDR_GROUP, 2u);
    bad_output.target = target(DALI_ADDR_GROUP, 16u);

    TEST_ASSERT_EQUAL(DALI_OK, dali_mapping_validate_entry(&short_output));
    TEST_ASSERT_EQUAL(DALI_OK, dali_mapping_validate_entry(&group_output));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_mapping_validate_entry(&bad_output));

    bad_output = short_output;
    bad_output.instance = 0u;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_mapping_validate_entry(&bad_output));
}

void test_validate_input_instance_mapping(void)
{
    DaliMappingEntry input = {
        .id            = 2u,
        .kind          = DALI_MAPPING_INPUT_INSTANCE,
        .target        = target(DALI_ADDR_SHORT, 5u),
        .instance      = 3u,
        .instance_type = 0u,
    };
    DaliMappingEntry bad = input;

    TEST_ASSERT_EQUAL(DALI_OK, dali_mapping_validate_entry(&input));

    bad.target = target(DALI_ADDR_GROUP, 0u);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_mapping_validate_entry(&bad));

    bad = input;
    bad.instance = 32u;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_mapping_validate_entry(&bad));
}

void test_validate_table_rejects_duplicate_ids(void)
{
    const DaliMappingEntry entries[] = {
        {
            .id            = 1u,
            .kind          = DALI_MAPPING_OUTPUT,
            .target        = target(DALI_ADDR_SHORT, 5u),
            .instance      = DALI_MAPPING_INSTANCE_UNUSED,
            .instance_type = DALI_MAPPING_INSTANCE_UNUSED,
        },
        {
            .id            = 1u,
            .kind          = DALI_MAPPING_INPUT_INSTANCE,
            .target        = target(DALI_ADDR_SHORT, 8u),
            .instance      = 0u,
            .instance_type = 3u,
        },
    };
    DaliMappingTable table = { .entries = entries, .count = 2u };

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_mapping_validate_table(&table));
}

void test_find_by_id_returns_matching_entry(void)
{
    const DaliMappingEntry entries[] = {
        {
            .id            = 10u,
            .kind          = DALI_MAPPING_OUTPUT,
            .target        = target(DALI_ADDR_GROUP, 0u),
            .instance      = DALI_MAPPING_INSTANCE_UNUSED,
            .instance_type = DALI_MAPPING_INSTANCE_UNUSED,
        },
        {
            .id            = 20u,
            .kind          = DALI_MAPPING_INPUT_INSTANCE,
            .target        = target(DALI_ADDR_SHORT, 8u),
            .instance      = 1u,
            .instance_type = 3u,
        },
    };
    DaliMappingTable table = { .entries = entries, .count = 2u };

    const DaliMappingEntry *found = dali_mapping_find_by_id(&table, 20u);

    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_UINT8(20u, found->id);
    TEST_ASSERT_EQUAL(DALI_MAPPING_INPUT_INSTANCE, found->kind);
    TEST_ASSERT_NULL(dali_mapping_find_by_id(&table, 30u));
}

void test_invalid_table_inputs(void)
{
    DaliMappingTable bad = { .entries = NULL, .count = 1u };
    DaliMappingTable empty = { .entries = NULL, .count = 0u };

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_mapping_validate_entry(NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_mapping_validate_table(NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_mapping_validate_table(&bad));
    TEST_ASSERT_EQUAL(DALI_OK, dali_mapping_validate_table(&empty));
    TEST_ASSERT_NULL(dali_mapping_find_by_id(NULL, 0u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_validate_output_mapping_targets);
    RUN_TEST(test_validate_input_instance_mapping);
    RUN_TEST(test_validate_table_rejects_duplicate_ids);
    RUN_TEST(test_find_by_id_returns_matching_entry);
    RUN_TEST(test_invalid_table_inputs);
    return UNITY_END();
}
