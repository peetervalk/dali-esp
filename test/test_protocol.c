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

void test_broadcast_recall_min(void)
{
    DaliFrame f = dali_cmd_broadcast_recall_min();
    TEST_ASSERT_EQUAL_HEX32(0xFF06u, f.data);
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
 * Command metadata tests
 * --------------------------------------------------------------------------*/

void test_command_lookup_query_status(void)
{
    const DaliCommandInfo *cmd = dali_command_lookup(DALI_CMD_QUERY_STATUS);

    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("QUERY STATUS", cmd->name);
    TEST_ASSERT_EQUAL_UINT8(0x90u, cmd->opcode_first);
    TEST_ASSERT_EQUAL_UINT8(0x90u, cmd->opcode_last);
    TEST_ASSERT_EQUAL(DALI_CMD_FRAME_16BIT, cmd->frame_kind);
    TEST_ASSERT_EQUAL(DALI_RESP_STATUS, cmd->response_kind);
    TEST_ASSERT_FALSE(cmd->send_twice);
    TEST_ASSERT_TRUE(cmd->implemented);
}

void test_command_lookup_opcode_dapc_range(void)
{
    const DaliCommandInfo *cmd = dali_command_lookup_opcode(DALI_CMD_FRAME_DAPC, 0x80u);

    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL(DALI_CMD_DAPC, cmd->id);
    TEST_ASSERT_EQUAL_UINT8(0x00u, cmd->opcode_first);
    TEST_ASSERT_EQUAL_UINT8(0xFEu, cmd->opcode_last);
    TEST_ASSERT_EQUAL(DALI_RESP_NONE, cmd->response_kind);
}

void test_command_lookup_opcode_range_go_to_scene(void)
{
    const DaliCommandInfo *cmd = dali_command_lookup_opcode(DALI_CMD_FRAME_16BIT, 0x15u);

    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL(DALI_CMD_GO_TO_SCENE, cmd->id);
    TEST_ASSERT_EQUAL_UINT8(0x10u, cmd->opcode_first);
    TEST_ASSERT_EQUAL_UINT8(0x1Fu, cmd->opcode_last);
    TEST_ASSERT_TRUE(cmd->implemented);
}

void test_command_lookup_disambiguates_normal_and_special_opcode(void)
{
    const DaliCommandInfo *normal = dali_command_lookup_opcode(DALI_CMD_FRAME_16BIT, 0xA1u);
    const DaliCommandInfo *special = dali_command_lookup_opcode(DALI_CMD_FRAME_SPECIAL, 0xA1u);

    TEST_ASSERT_NOT_NULL(normal);
    TEST_ASSERT_NOT_NULL(special);
    TEST_ASSERT_EQUAL(DALI_CMD_QUERY_MAX_LEVEL, normal->id);
    TEST_ASSERT_EQUAL(DALI_CMD_TERMINATE, special->id);
}

static void assert_command_send_twice(DaliCommandId id)
{
    const DaliCommandInfo *cmd = dali_command_lookup(id);

    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_TRUE(cmd->send_twice);
}

void test_command_lookup_send_twice_metadata(void)
{
    const DaliCommandId config_commands[] = {
        DALI_CMD_RESET,
        DALI_CMD_STORE_ACTUAL_LEVEL_DTR0,
        DALI_CMD_SAVE_PERSISTENT_VARIABLES,
        DALI_CMD_SET_OPERATING_MODE_DTR0,
        DALI_CMD_RESET_MEMORY_BANK_DTR0,
        DALI_CMD_IDENTIFY_DEVICE,
        DALI_CMD_SET_MAX_LEVEL_DTR0,
        DALI_CMD_SET_MIN_LEVEL_DTR0,
        DALI_CMD_SET_SYSTEM_FAILURE_LEVEL_DTR0,
        DALI_CMD_SET_POWER_ON_LEVEL_DTR0,
        DALI_CMD_SET_FADE_TIME_DTR0,
        DALI_CMD_SET_FADE_RATE_DTR0,
        DALI_CMD_SET_EXTENDED_FADE_TIME_DTR0,
        DALI_CMD_SET_SCENE,
        DALI_CMD_REMOVE_FROM_SCENE,
        DALI_CMD_ADD_TO_GROUP,
        DALI_CMD_REMOVE_FROM_GROUP,
        DALI_CMD_SET_SHORT_ADDRESS_DTR0,
        DALI_CMD_ENABLE_WRITE_MEMORY,
        DALI_CMD_INITIALISE,
        DALI_CMD_RANDOMIZE,
    };

    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(config_commands) / sizeof(config_commands[0]));
         i++) {
        assert_command_send_twice(config_commands[i]);
    }

    const DaliCommandInfo *query_status = dali_command_lookup(DALI_CMD_QUERY_STATUS);
    TEST_ASSERT_NOT_NULL(query_status);
    TEST_ASSERT_FALSE(query_status->send_twice);
}

void test_command_lookup_output_level_helpers_are_implemented(void)
{
    const DaliCommandId output_commands[] = {
        DALI_CMD_UP,
        DALI_CMD_DOWN,
        DALI_CMD_STEP_UP,
        DALI_CMD_STEP_DOWN,
        DALI_CMD_STEP_DOWN_AND_OFF,
        DALI_CMD_ON_AND_STEP_UP,
        DALI_CMD_ENABLE_DAPC_SEQUENCE,
        DALI_CMD_GO_TO_LAST_ACTIVE_LEVEL,
        DALI_CMD_GO_TO_SCENE,
    };

    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(output_commands) / sizeof(output_commands[0]));
         i++) {
        const DaliCommandInfo *cmd = dali_command_lookup(output_commands[i]);

        TEST_ASSERT_NOT_NULL(cmd);
        TEST_ASSERT_EQUAL(DALI_RESP_NONE, cmd->response_kind);
        TEST_ASSERT_FALSE(cmd->send_twice);
        TEST_ASSERT_TRUE(cmd->implemented);
    }
}

void test_command_lookup_addressed_queries_are_implemented(void)
{
    const DaliCommandId query_commands[] = {
        DALI_CMD_QUERY_STATUS,
        DALI_CMD_QUERY_CONTROL_GEAR_PRESENT,
        DALI_CMD_QUERY_LAMP_FAILURE,
        DALI_CMD_QUERY_LAMP_POWER_ON,
        DALI_CMD_QUERY_LIMIT_ERROR,
        DALI_CMD_QUERY_RESET_STATE,
        DALI_CMD_QUERY_MISSING_SHORT_ADDRESS,
        DALI_CMD_QUERY_VERSION_NUMBER,
        DALI_CMD_QUERY_CONTENT_DTR0,
        DALI_CMD_QUERY_DEVICE_TYPE,
        DALI_CMD_QUERY_PHYSICAL_MINIMUM,
        DALI_CMD_QUERY_POWER_FAILURE,
        DALI_CMD_QUERY_CONTENT_DTR1,
        DALI_CMD_QUERY_CONTENT_DTR2,
        DALI_CMD_QUERY_OPERATING_MODE,
        DALI_CMD_QUERY_LIGHT_SOURCE_TYPE,
        DALI_CMD_QUERY_ACTUAL_LEVEL,
        DALI_CMD_QUERY_MAX_LEVEL,
        DALI_CMD_QUERY_MIN_LEVEL,
        DALI_CMD_QUERY_POWER_ON_LEVEL,
        DALI_CMD_QUERY_SYSTEM_FAILURE_LEVEL,
        DALI_CMD_QUERY_FADE_TIME_FADE_RATE,
        DALI_CMD_QUERY_MANUFACTURER_SPECIFIC_MODE,
        DALI_CMD_QUERY_NEXT_DEVICE_TYPE,
        DALI_CMD_QUERY_EXTENDED_FADE_TIME,
        DALI_CMD_QUERY_CONTROL_GEAR_FAILURE,
        DALI_CMD_QUERY_SCENE_LEVEL,
        DALI_CMD_QUERY_GROUPS_0_7,
        DALI_CMD_QUERY_GROUPS_8_15,
        DALI_CMD_QUERY_RANDOM_ADDRESS_H,
        DALI_CMD_QUERY_RANDOM_ADDRESS_M,
        DALI_CMD_QUERY_RANDOM_ADDRESS_L,
        DALI_CMD_READ_MEMORY_LOCATION,
        DALI_CMD_QUERY_EXTENDED_VERSION_NUMBER,
    };

    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(query_commands) / sizeof(query_commands[0]));
         i++) {
        const DaliCommandInfo *cmd = dali_command_lookup(query_commands[i]);

        TEST_ASSERT_NOT_NULL(cmd);
        TEST_ASSERT_EQUAL(DALI_CMD_FRAME_16BIT, cmd->frame_kind);
        TEST_ASSERT_NOT_EQUAL(DALI_RESP_NONE, cmd->response_kind);
        TEST_ASSERT_FALSE(cmd->send_twice);
        TEST_ASSERT_TRUE(cmd->implemented);
    }
}

void test_command_lookup_config_commands_are_implemented(void)
{
    const DaliCommandId config_commands[] = {
        DALI_CMD_RESET,
        DALI_CMD_STORE_ACTUAL_LEVEL_DTR0,
        DALI_CMD_SAVE_PERSISTENT_VARIABLES,
        DALI_CMD_SET_OPERATING_MODE_DTR0,
        DALI_CMD_RESET_MEMORY_BANK_DTR0,
        DALI_CMD_IDENTIFY_DEVICE,
        DALI_CMD_SET_MAX_LEVEL_DTR0,
        DALI_CMD_SET_MIN_LEVEL_DTR0,
        DALI_CMD_SET_SYSTEM_FAILURE_LEVEL_DTR0,
        DALI_CMD_SET_POWER_ON_LEVEL_DTR0,
        DALI_CMD_SET_FADE_TIME_DTR0,
        DALI_CMD_SET_FADE_RATE_DTR0,
        DALI_CMD_SET_EXTENDED_FADE_TIME_DTR0,
        DALI_CMD_SET_SCENE,
        DALI_CMD_REMOVE_FROM_SCENE,
        DALI_CMD_ADD_TO_GROUP,
        DALI_CMD_REMOVE_FROM_GROUP,
        DALI_CMD_SET_SHORT_ADDRESS_DTR0,
        DALI_CMD_ENABLE_WRITE_MEMORY,
    };

    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(config_commands) / sizeof(config_commands[0]));
         i++) {
        const DaliCommandInfo *cmd = dali_command_lookup(config_commands[i]);

        TEST_ASSERT_NOT_NULL(cmd);
        TEST_ASSERT_EQUAL(DALI_CMD_FRAME_16BIT, cmd->frame_kind);
        TEST_ASSERT_EQUAL(DALI_RESP_NONE, cmd->response_kind);
        TEST_ASSERT_TRUE(cmd->send_twice);
        TEST_ASSERT_TRUE(cmd->implemented);
    }
}

void test_command_lookup_dali2_input_value(void)
{
    const DaliCommandInfo *cmd = dali_command_lookup(DALI_CMD_QUERY_INPUT_VALUE);

    TEST_ASSERT_NOT_NULL(cmd);
    TEST_ASSERT_EQUAL_STRING("QUERY INPUT VALUE", cmd->name);
    TEST_ASSERT_EQUAL_UINT8(0x8Cu, cmd->opcode_first);
    TEST_ASSERT_EQUAL(DALI_CMD_FRAME_24BIT_INST, cmd->frame_kind);
    TEST_ASSERT_EQUAL(DALI_RESP_INPUT_VALUE_MSB, cmd->response_kind);
}

void test_command_metadata_table_covers_all_standard_ids(void)
{
    TEST_ASSERT_EQUAL_UINT8((uint8_t)DALI_CMD_COUNT, dali_command_count());

    for (uint8_t i = 0u; i < (uint8_t)DALI_CMD_COUNT; i++) {
        const DaliCommandInfo *cmd = dali_command_lookup((DaliCommandId)i);

        TEST_ASSERT_NOT_NULL(cmd);
        TEST_ASSERT_EQUAL((DaliCommandId)i, cmd->id);
    }
}

void test_command_lookup_keeps_vendor_specific_opcodes_out_of_standard_table(void)
{
    TEST_ASSERT_NULL(dali_command_lookup_opcode(DALI_CMD_FRAME_24BIT_INST, 0x46u));
}

void test_command_lookup_invalid_returns_null(void)
{
    TEST_ASSERT_NULL(dali_command_lookup((DaliCommandId)255u));
    TEST_ASSERT_NULL(dali_command_lookup_opcode(DALI_CMD_FRAME_24BIT_INST, 0xFFu));
}

void test_build_command_short_group_broadcast_dapc(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_build_command(DALI_ADDR_SHORT, 5u, DALI_CMD_DAPC, 128u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0A80u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_build_command(DALI_ADDR_GROUP, 0u, DALI_CMD_DAPC, 128u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8080u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_build_command(DALI_ADDR_BROADCAST, 0u, DALI_CMD_DAPC, 128u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFE80u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_build_command_fixed_opcode_and_range_opcode(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_build_command(DALI_ADDR_SHORT, 2u,
                                         DALI_CMD_RECALL_MAX_LEVEL, 0u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0505u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_build_command(DALI_ADDR_SHORT, 2u,
                                         DALI_CMD_GO_TO_SCENE, 5u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0515u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_build_command_rejects_invalid_args(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_command(DALI_ADDR_SHORT, 64u, DALI_CMD_OFF, 0u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_command(DALI_ADDR_GROUP, 16u, DALI_CMD_OFF, 0u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_command(DALI_ADDR_SHORT, 0u, DALI_CMD_DAPC, 255u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_command(DALI_ADDR_SHORT, 0u,
                                         DALI_CMD_RECALL_MAX_LEVEL, 1u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_command(DALI_ADDR_SHORT, 0u,
                                         DALI_CMD_GO_TO_SCENE, 16u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_command(DALI_ADDR_SHORT, 0u,
                                         DALI_CMD_QUERY_INPUT_VALUE, 0u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_command(DALI_ADDR_SHORT, 0u, DALI_CMD_OFF, 0u, NULL));
}

void test_build_instance_command_query_input_value(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_build_instance_command(5u, 0u, DALI_CMD_QUERY_INPUT_VALUE, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B008Cu, frame.data);
    TEST_ASSERT_EQUAL_UINT8(24u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_build_instance_command(5u, 0xFFu,
                                                  DALI_CMD_QUERY_RESOLUTION, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0BFF81u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(24u, frame.bit_length);
}

void test_build_instance_command_rejects_invalid_args(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_instance_command(64u, 0u,
                                                  DALI_CMD_QUERY_INPUT_VALUE, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_instance_command(5u, 32u,
                                                  DALI_CMD_QUERY_INPUT_VALUE, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_instance_command(5u, 0u,
                                                  DALI_CMD_QUERY_STATUS, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_build_instance_command(5u, 0u,
                                                  DALI_CMD_QUERY_INPUT_VALUE, NULL));
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

void test_parse_by_kind_status(void)
{
    DaliFrame reply = { .data = 0xAFu, .bit_length = 8u };
    DaliParsedResponse parsed;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_by_kind(DALI_RESP_STATUS, &reply, &parsed));
    TEST_ASSERT_EQUAL(DALI_RESP_STATUS, parsed.kind);
    TEST_ASSERT_EQUAL_HEX8(0xAFu, parsed.raw);
    TEST_ASSERT_TRUE(parsed.status.ballast_failure);
    TEST_ASSERT_TRUE(parsed.status.lamp_failure);
    TEST_ASSERT_TRUE(parsed.status.lamp_arc_power_on);
    TEST_ASSERT_TRUE(parsed.status.limit_error);
    TEST_ASSERT_FALSE(parsed.status.fade_running);
    TEST_ASSERT_TRUE(parsed.status.reset_state);
    TEST_ASSERT_FALSE(parsed.status.missing_short_address);
    TEST_ASSERT_TRUE(parsed.status.power_failure);
}

void test_parse_by_kind_yes_no_and_uint8(void)
{
    DaliFrame yes = { .data = 0xFFu, .bit_length = 8u };
    DaliFrame value = { .data = 0x80u, .bit_length = 8u };
    DaliParsedResponse parsed;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_by_kind(DALI_RESP_YES_NO, &yes, &parsed));
    TEST_ASSERT_TRUE(parsed.yes);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, parsed.raw);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_by_kind(DALI_RESP_UINT8, &value, &parsed));
    TEST_ASSERT_EQUAL_UINT8(128u, parsed.value);
    TEST_ASSERT_FALSE(parsed.yes);
}

void test_parse_command_response_uses_metadata(void)
{
    DaliFrame status = { .data = 0x02u, .bit_length = 8u };
    DaliFrame level = { .data = 0x7Fu, .bit_length = 8u };
    DaliParsedResponse parsed;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_command_response(DALI_CMD_QUERY_STATUS, &status, &parsed));
    TEST_ASSERT_EQUAL(DALI_RESP_STATUS, parsed.kind);
    TEST_ASSERT_TRUE(parsed.status.lamp_failure);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_command_response(DALI_CMD_QUERY_ACTUAL_LEVEL, &level, &parsed));
    TEST_ASSERT_EQUAL(DALI_RESP_UINT8, parsed.kind);
    TEST_ASSERT_EQUAL_UINT8(0x7Fu, parsed.value);
}

void test_parse_command_response_query_kinds(void)
{
    DaliFrame yes = { .data = DALI_YES_RESPONSE, .bit_length = DALI_BACKWARD_FRAME_BITS };
    DaliFrame bitset = { .data = 0xA5u, .bit_length = DALI_BACKWARD_FRAME_BITS };
    DaliFrame fade = { .data = 0x34u, .bit_length = DALI_BACKWARD_FRAME_BITS };
    DaliFrame memory = { .data = 0x5Au, .bit_length = DALI_BACKWARD_FRAME_BITS };
    DaliParsedResponse parsed;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_command_response(DALI_CMD_QUERY_CONTROL_GEAR_PRESENT,
                                                  &yes,
                                                  &parsed));
    TEST_ASSERT_EQUAL(DALI_RESP_YES_NO, parsed.kind);
    TEST_ASSERT_TRUE(parsed.yes);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_command_response(DALI_CMD_QUERY_GROUPS_0_7,
                                                  &bitset,
                                                  &parsed));
    TEST_ASSERT_EQUAL(DALI_RESP_BITSET8, parsed.kind);
    TEST_ASSERT_EQUAL_HEX8(0xA5u, parsed.bitset);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_command_response(DALI_CMD_QUERY_FADE_TIME_FADE_RATE,
                                                  &fade,
                                                  &parsed));
    TEST_ASSERT_EQUAL(DALI_RESP_FADE_TIME_RATE, parsed.kind);
    TEST_ASSERT_EQUAL_UINT8(0x03u, parsed.fade.fade_time);
    TEST_ASSERT_EQUAL_UINT8(0x04u, parsed.fade.fade_rate);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_command_response(DALI_CMD_READ_MEMORY_LOCATION,
                                                  &memory,
                                                  &parsed));
    TEST_ASSERT_EQUAL(DALI_RESP_MEMORY_BYTE, parsed.kind);
    TEST_ASSERT_EQUAL_HEX8(0x5Au, parsed.value);
}

void test_parse_by_kind_bitset_memory_and_input_value(void)
{
    DaliFrame reply = { .data = 0x55u, .bit_length = 8u };
    DaliParsedResponse parsed;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_by_kind(DALI_RESP_BITSET8, &reply, &parsed));
    TEST_ASSERT_EQUAL_HEX8(0x55u, parsed.bitset);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_by_kind(DALI_RESP_MEMORY_BYTE, &reply, &parsed));
    TEST_ASSERT_EQUAL_HEX8(0x55u, parsed.value);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_by_kind(DALI_RESP_INPUT_VALUE_MSB, &reply, &parsed));
    TEST_ASSERT_EQUAL_HEX8(0x55u, parsed.value);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_by_kind(DALI_RESP_INPUT_VALUE_LATCH, &reply, &parsed));
    TEST_ASSERT_EQUAL_HEX8(0x55u, parsed.value);
}

void test_parse_fade_time_rate_helper_and_response_kind(void)
{
    DaliFrame reply = { .data = 0xABu, .bit_length = 8u };
    DaliParsedResponse parsed;
    DaliFadeTimeRate fade;

    TEST_ASSERT_EQUAL(DALI_OK, dali_parse_fade_time_rate(0xABu, &fade));
    TEST_ASSERT_EQUAL_UINT8(0x0Au, fade.fade_time);
    TEST_ASSERT_EQUAL_UINT8(0x0Bu, fade.fade_rate);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_parse_fade_time_rate(0x00u, NULL));

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_by_kind(DALI_RESP_FADE_TIME_RATE, &reply, &parsed));
    TEST_ASSERT_EQUAL(DALI_RESP_FADE_TIME_RATE, parsed.kind);
    TEST_ASSERT_EQUAL_UINT8(0x0Au, parsed.fade.fade_time);
    TEST_ASSERT_EQUAL_UINT8(0x0Bu, parsed.fade.fade_rate);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_parse_command_response(DALI_CMD_QUERY_FADE_TIME_FADE_RATE,
                                                  &reply,
                                                  &parsed));
    TEST_ASSERT_EQUAL_UINT8(0x0Au, parsed.fade.fade_time);
    TEST_ASSERT_EQUAL_UINT8(0x0Bu, parsed.fade.fade_rate);
}

void test_input_value_accumulator_combines_16bit_and_multibyte_values(void)
{
    DaliInputValue value;
    DaliFrame lsb = { .data = 0x34u, .bit_length = 8u };
    uint16_t value16 = 0u;

    TEST_ASSERT_EQUAL(DALI_OK, dali_input_value_start(&value, 2u));
    TEST_ASSERT_FALSE(value.complete);
    TEST_ASSERT_EQUAL(DALI_OK, dali_input_value_push(&value, 0x12u));
    TEST_ASSERT_FALSE(value.complete);
    TEST_ASSERT_EQUAL(DALI_OK, dali_input_value_push_frame(&value, &lsb));
    TEST_ASSERT_TRUE(value.complete);
    TEST_ASSERT_EQUAL_UINT8(2u, value.byte_count);
    TEST_ASSERT_EQUAL_HEX32(0x1234u, value.value);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_input_value_push(&value, 0x56u));

    TEST_ASSERT_EQUAL(DALI_OK, dali_input_value_parse_16(0x12u, 0x34u, &value16));
    TEST_ASSERT_EQUAL_HEX16(0x1234u, value16);

    TEST_ASSERT_EQUAL(DALI_OK, dali_input_value_start(&value, 3u));
    TEST_ASSERT_EQUAL(DALI_OK, dali_input_value_push(&value, 0x01u));
    TEST_ASSERT_EQUAL(DALI_OK, dali_input_value_push(&value, 0x02u));
    TEST_ASSERT_EQUAL(DALI_OK, dali_input_value_push(&value, 0x03u));
    TEST_ASSERT_TRUE(value.complete);
    TEST_ASSERT_EQUAL_HEX32(0x010203u, value.value);
}

void test_input_value_accumulator_rejects_invalid_args(void)
{
    DaliInputValue value;
    DaliFrame wrong_bits = { .data = 0x12u, .bit_length = 16u };

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_input_value_start(NULL, 2u));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_input_value_start(&value, 0u));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_input_value_start(&value, 5u));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_input_value_push(NULL, 0x12u));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_input_value_parse_16(0u, 0u, NULL));

    TEST_ASSERT_EQUAL(DALI_OK, dali_input_value_start(&value, 2u));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_input_value_push_frame(&value, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_input_value_push_frame(&value, &wrong_bits));
}

void test_parse_rejects_invalid_args_and_none_response(void)
{
    DaliFrame reply = { .data = 0x00u, .bit_length = 8u };
    DaliFrame wrong_bits = { .data = 0x00u, .bit_length = 16u };
    DaliParsedResponse parsed;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_parse_by_kind(DALI_RESP_NONE, &reply, &parsed));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_parse_by_kind(DALI_RESP_UINT8, &wrong_bits, &parsed));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_parse_by_kind(DALI_RESP_UINT8, NULL, &parsed));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_parse_by_kind(DALI_RESP_UINT8, &reply, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_parse_command_response(DALI_CMD_OFF, &reply, &parsed));
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
    RUN_TEST(test_broadcast_recall_min);
    RUN_TEST(test_parse_response_returns_value);
    RUN_TEST(test_parse_response_null_returns_invalid);
    /* Command metadata */
    RUN_TEST(test_command_lookup_query_status);
    RUN_TEST(test_command_lookup_opcode_dapc_range);
    RUN_TEST(test_command_lookup_opcode_range_go_to_scene);
    RUN_TEST(test_command_lookup_disambiguates_normal_and_special_opcode);
    RUN_TEST(test_command_lookup_send_twice_metadata);
    RUN_TEST(test_command_lookup_output_level_helpers_are_implemented);
    RUN_TEST(test_command_lookup_addressed_queries_are_implemented);
    RUN_TEST(test_command_lookup_config_commands_are_implemented);
    RUN_TEST(test_command_lookup_dali2_input_value);
    RUN_TEST(test_command_metadata_table_covers_all_standard_ids);
    RUN_TEST(test_command_lookup_keeps_vendor_specific_opcodes_out_of_standard_table);
    RUN_TEST(test_command_lookup_invalid_returns_null);
    RUN_TEST(test_build_command_short_group_broadcast_dapc);
    RUN_TEST(test_build_command_fixed_opcode_and_range_opcode);
    RUN_TEST(test_build_command_rejects_invalid_args);
    RUN_TEST(test_build_instance_command_query_input_value);
    RUN_TEST(test_build_instance_command_rejects_invalid_args);
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
    RUN_TEST(test_parse_by_kind_status);
    RUN_TEST(test_parse_by_kind_yes_no_and_uint8);
    RUN_TEST(test_parse_command_response_uses_metadata);
    RUN_TEST(test_parse_command_response_query_kinds);
    RUN_TEST(test_parse_by_kind_bitset_memory_and_input_value);
    RUN_TEST(test_parse_fade_time_rate_helper_and_response_kind);
    RUN_TEST(test_input_value_accumulator_combines_16bit_and_multibyte_values);
    RUN_TEST(test_input_value_accumulator_rejects_invalid_args);
    RUN_TEST(test_parse_rejects_invalid_args_and_none_response);
    return UNITY_END();
}
