#include "dali_protocol.h"

#define CMD(id, name, first, last, frame, resp, twice, implemented) \
    { id, name, first, last, frame, resp, twice, implemented }

static const DaliCommandInfo s_command_table[] = {
    CMD(DALI_CMD_DAPC, "DAPC", 0x00u, 0xFEu, DALI_CMD_FRAME_DAPC, DALI_RESP_NONE, false, true),

    CMD(DALI_CMD_OFF, "OFF", 0x00u, 0x00u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_UP, "UP", 0x01u, 0x01u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_DOWN, "DOWN", 0x02u, 0x02u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_STEP_UP, "STEP UP", 0x03u, 0x03u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_STEP_DOWN, "STEP DOWN", 0x04u, 0x04u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_RECALL_MAX_LEVEL, "RECALL MAX LEVEL", 0x05u, 0x05u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_RECALL_MIN_LEVEL, "RECALL MIN LEVEL", 0x06u, 0x06u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_STEP_DOWN_AND_OFF, "STEP DOWN AND OFF", 0x07u, 0x07u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_ON_AND_STEP_UP, "ON AND STEP UP", 0x08u, 0x08u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_ENABLE_DAPC_SEQUENCE, "ENABLE DAPC SEQUENCE", 0x09u, 0x09u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_GO_TO_LAST_ACTIVE_LEVEL, "GO TO LAST ACTIVE LEVEL", 0x0Au, 0x0Au, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),
    CMD(DALI_CMD_GO_TO_SCENE, "GO TO SCENE", 0x10u, 0x1Fu, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, false, true),

    CMD(DALI_CMD_RESET, "RESET", 0x20u, 0x20u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_STORE_ACTUAL_LEVEL_DTR0, "STORE ACTUAL LEVEL IN DTR0", 0x21u, 0x21u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SAVE_PERSISTENT_VARIABLES, "SAVE PERSISTENT VARIABLES", 0x22u, 0x22u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SET_OPERATING_MODE_DTR0, "SET OPERATING MODE DTR0", 0x23u, 0x23u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_RESET_MEMORY_BANK_DTR0, "RESET MEMORY BANK DTR0", 0x24u, 0x24u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_IDENTIFY_DEVICE, "IDENTIFY DEVICE", 0x25u, 0x25u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SET_MAX_LEVEL_DTR0, "SET MAX LEVEL DTR0", 0x2Au, 0x2Au, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SET_MIN_LEVEL_DTR0, "SET MIN LEVEL DTR0", 0x2Bu, 0x2Bu, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SET_SYSTEM_FAILURE_LEVEL_DTR0, "SET SYSTEM FAILURE LEVEL DTR0", 0x2Cu, 0x2Cu, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SET_POWER_ON_LEVEL_DTR0, "SET POWER ON LEVEL DTR0", 0x2Du, 0x2Du, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SET_FADE_TIME_DTR0, "SET FADE TIME DTR0", 0x2Eu, 0x2Eu, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SET_FADE_RATE_DTR0, "SET FADE RATE DTR0", 0x2Fu, 0x2Fu, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SET_EXTENDED_FADE_TIME_DTR0, "SET EXTENDED FADE TIME DTR0", 0x30u, 0x30u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SET_SCENE, "SET SCENE", 0x40u, 0x4Fu, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_REMOVE_FROM_SCENE, "REMOVE FROM SCENE", 0x50u, 0x5Fu, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_ADD_TO_GROUP, "ADD TO GROUP", 0x60u, 0x6Fu, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_REMOVE_FROM_GROUP, "REMOVE FROM GROUP", 0x70u, 0x7Fu, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_SET_SHORT_ADDRESS_DTR0, "SET SHORT ADDRESS DTR0", 0x80u, 0x80u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),
    CMD(DALI_CMD_ENABLE_WRITE_MEMORY, "ENABLE WRITE MEMORY", 0x81u, 0x81u, DALI_CMD_FRAME_16BIT, DALI_RESP_NONE, true, true),

    CMD(DALI_CMD_QUERY_STATUS, "QUERY STATUS", 0x90u, 0x90u, DALI_CMD_FRAME_16BIT, DALI_RESP_STATUS, false, true),
    CMD(DALI_CMD_QUERY_CONTROL_GEAR_PRESENT, "QUERY CONTROL GEAR PRESENT", 0x91u, 0x91u, DALI_CMD_FRAME_16BIT, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_LAMP_FAILURE, "QUERY LAMP FAILURE", 0x92u, 0x92u, DALI_CMD_FRAME_16BIT, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_LAMP_POWER_ON, "QUERY LAMP POWER ON", 0x93u, 0x93u, DALI_CMD_FRAME_16BIT, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_LIMIT_ERROR, "QUERY LIMIT ERROR", 0x94u, 0x94u, DALI_CMD_FRAME_16BIT, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_RESET_STATE, "QUERY RESET STATE", 0x95u, 0x95u, DALI_CMD_FRAME_16BIT, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_MISSING_SHORT_ADDRESS, "QUERY MISSING SHORT ADDRESS", 0x96u, 0x96u, DALI_CMD_FRAME_16BIT, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_VERSION_NUMBER, "QUERY VERSION NUMBER", 0x97u, 0x97u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_CONTENT_DTR0, "QUERY CONTENT DTR0", 0x98u, 0x98u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_DEVICE_TYPE, "QUERY DEVICE TYPE", 0x99u, 0x99u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_PHYSICAL_MINIMUM, "QUERY PHYSICAL MINIMUM", 0x9Au, 0x9Au, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_POWER_FAILURE, "QUERY POWER FAILURE", 0x9Bu, 0x9Bu, DALI_CMD_FRAME_16BIT, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_CONTENT_DTR1, "QUERY CONTENT DTR1", 0x9Cu, 0x9Cu, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_CONTENT_DTR2, "QUERY CONTENT DTR2", 0x9Du, 0x9Du, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_OPERATING_MODE, "QUERY OPERATING MODE", 0x9Eu, 0x9Eu, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_LIGHT_SOURCE_TYPE, "QUERY LIGHT SOURCE TYPE", 0x9Fu, 0x9Fu, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_ACTUAL_LEVEL, "QUERY ACTUAL LEVEL", 0xA0u, 0xA0u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_MAX_LEVEL, "QUERY MAX LEVEL", 0xA1u, 0xA1u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_MIN_LEVEL, "QUERY MIN LEVEL", 0xA2u, 0xA2u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_POWER_ON_LEVEL, "QUERY POWER ON LEVEL", 0xA3u, 0xA3u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_SYSTEM_FAILURE_LEVEL, "QUERY SYSTEM FAILURE LEVEL", 0xA4u, 0xA4u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_FADE_TIME_FADE_RATE, "QUERY FADE TIME/FADE RATE", 0xA5u, 0xA5u, DALI_CMD_FRAME_16BIT, DALI_RESP_FADE_TIME_RATE, false, true),
    CMD(DALI_CMD_QUERY_MANUFACTURER_SPECIFIC_MODE, "QUERY MANUFACTURER SPECIFIC MODE", 0xA6u, 0xA6u, DALI_CMD_FRAME_16BIT, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_NEXT_DEVICE_TYPE, "QUERY NEXT DEVICE TYPE", 0xA7u, 0xA7u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_EXTENDED_FADE_TIME, "QUERY EXTENDED FADE TIME", 0xA8u, 0xA8u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_CONTROL_GEAR_FAILURE, "QUERY CONTROL GEAR FAILURE", 0xAAu, 0xAAu, DALI_CMD_FRAME_16BIT, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_SCENE_LEVEL, "QUERY SCENE LEVEL", 0xB0u, 0xBFu, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_GROUPS_0_7, "QUERY GROUPS 0-7", 0xC0u, 0xC0u, DALI_CMD_FRAME_16BIT, DALI_RESP_BITSET8, false, true),
    CMD(DALI_CMD_QUERY_GROUPS_8_15, "QUERY GROUPS 8-15", 0xC1u, 0xC1u, DALI_CMD_FRAME_16BIT, DALI_RESP_BITSET8, false, true),
    CMD(DALI_CMD_QUERY_RANDOM_ADDRESS_H, "QUERY RANDOM ADDRESS H", 0xC2u, 0xC2u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_RANDOM_ADDRESS_M, "QUERY RANDOM ADDRESS M", 0xC3u, 0xC3u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_RANDOM_ADDRESS_L, "QUERY RANDOM ADDRESS L", 0xC4u, 0xC4u, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_READ_MEMORY_LOCATION, "READ MEMORY LOCATION", 0xC5u, 0xC5u, DALI_CMD_FRAME_16BIT, DALI_RESP_MEMORY_BYTE, false, true),
    CMD(DALI_CMD_QUERY_EXTENDED_VERSION_NUMBER, "QUERY EXTENDED VERSION NUMBER", 0xFFu, 0xFFu, DALI_CMD_FRAME_16BIT, DALI_RESP_UINT8, false, true),

    CMD(DALI_CMD_TERMINATE, "TERMINATE", 0xA1u, 0xA1u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_DTR0_DATA, "DTR0 DATA", 0xA3u, 0xA3u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_INITIALISE, "INITIALISE", 0xA5u, 0xA5u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, true, false),
    CMD(DALI_CMD_RANDOMIZE, "RANDOMIZE", 0xA7u, 0xA7u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, true, false),
    CMD(DALI_CMD_COMPARE, "COMPARE", 0xA9u, 0xA9u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_YES_NO, false, false),
    CMD(DALI_CMD_WITHDRAW, "WITHDRAW", 0xABu, 0xABu, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_PING, "PING", 0xADu, 0xADu, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_SEARCH_ADDRH, "SEARCH ADDRH", 0xB1u, 0xB1u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_SEARCH_ADDRM, "SEARCH ADDRM", 0xB3u, 0xB3u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_SEARCH_ADDRL, "SEARCH ADDRL", 0xB5u, 0xB5u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_PROGRAM_SHORT_ADDRESS, "PROGRAM SHORT ADDRESS", 0xB7u, 0xB7u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_VERIFY_SHORT_ADDRESS, "VERIFY SHORT ADDRESS", 0xB9u, 0xB9u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_YES_NO, false, false),
    CMD(DALI_CMD_QUERY_SHORT_ADDRESS, "QUERY SHORT ADDRESS", 0xBBu, 0xBBu, DALI_CMD_FRAME_SPECIAL, DALI_RESP_UINT8, false, false),
    CMD(DALI_CMD_ENABLE_DEVICE_TYPE, "ENABLE DEVICE TYPE", 0xC1u, 0xC1u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_DTR1_DATA, "DTR1 DATA", 0xC3u, 0xC3u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_DTR2_DATA, "DTR2 DATA", 0xC5u, 0xC5u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),
    CMD(DALI_CMD_WRITE_MEMORY_LOCATION, "WRITE MEMORY LOCATION", 0xC7u, 0xC7u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_MEMORY_BYTE, false, false),
    CMD(DALI_CMD_WRITE_MEMORY_LOCATION_NO_REPLY, "WRITE MEMORY LOCATION NO REPLY", 0xC9u, 0xC9u, DALI_CMD_FRAME_SPECIAL, DALI_RESP_NONE, false, false),

    CMD(DALI_CMD_QUERY_NUMBER_OF_INSTANCES, "QUERY NUMBER OF INSTANCES", 0x35u, 0x35u, DALI_CMD_FRAME_24BIT_DEV, DALI_RESP_UINT8, false, true),

    CMD(DALI_CMD_QUERY_INSTANCE_TYPE, "QUERY INSTANCE TYPE", 0x80u, 0x80u, DALI_CMD_FRAME_24BIT_INST, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_RESOLUTION, "QUERY RESOLUTION", 0x81u, 0x81u, DALI_CMD_FRAME_24BIT_INST, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_INSTANCE_ERROR, "QUERY INSTANCE ERROR", 0x82u, 0x82u, DALI_CMD_FRAME_24BIT_INST, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_INSTANCE_STATUS, "QUERY INSTANCE STATUS", 0x83u, 0x83u, DALI_CMD_FRAME_24BIT_INST, DALI_RESP_UINT8, false, true),
    CMD(DALI_CMD_QUERY_INSTANCE_ENABLED, "QUERY INSTANCE ENABLED", 0x86u, 0x86u, DALI_CMD_FRAME_24BIT_INST, DALI_RESP_YES_NO, false, true),
    CMD(DALI_CMD_QUERY_INPUT_VALUE, "QUERY INPUT VALUE", 0x8Cu, 0x8Cu, DALI_CMD_FRAME_24BIT_INST, DALI_RESP_INPUT_VALUE_MSB, false, false),
    CMD(DALI_CMD_QUERY_INPUT_VALUE_LATCH, "QUERY INPUT VALUE LATCH", 0x8Du, 0x8Du, DALI_CMD_FRAME_24BIT_INST, DALI_RESP_INPUT_VALUE_LATCH, false, false),
};

#define DALI_COMMAND_TABLE_COUNT ((uint8_t)(sizeof(s_command_table) / sizeof(s_command_table[0])))
#undef CMD

/*
 * DALI address byte encoding (IEC 62386-102):
 *
 *   Short address command:  0AAAAAAS  (A = addr bits, S = selector)
 *     S=0: DAPC (direct arc power control)
 *     S=1: command
 *   Broadcast command:      1111111S
 *
 * For short address:  addr_byte = (addr << 1) | S
 * For broadcast:      addr_byte = 0xFF (S=1) or 0xFE (S=0)
 */

const DaliCommandInfo *dali_command_lookup(DaliCommandId id)
{
    for (uint8_t i = 0u; i < DALI_COMMAND_TABLE_COUNT; i++) {
        if (s_command_table[i].id == id) {
            return &s_command_table[i];
        }
    }
    return NULL;
}

const DaliCommandInfo *dali_command_lookup_opcode(DaliCommandFrameKind frame_kind,
                                                  uint8_t opcode)
{
    for (uint8_t i = 0u; i < DALI_COMMAND_TABLE_COUNT; i++) {
        const DaliCommandInfo *cmd = &s_command_table[i];
        if (cmd->frame_kind == frame_kind &&
            opcode >= cmd->opcode_first &&
            opcode <= cmd->opcode_last) {
            return cmd;
        }
    }
    return NULL;
}

uint8_t dali_command_count(void)
{
    return DALI_COMMAND_TABLE_COUNT;
}

static uint8_t short_addr_byte(uint8_t addr, uint8_t selector)
{
    return (uint8_t)(((addr & DALI_MAX_SHORT_ADDRESS) << 1u) | (selector & 0x01u));
}

static DaliFrame make_frame16(uint8_t addr_byte, uint8_t cmd_byte)
{
    DaliFrame f;
    f.data       = ((uint32_t)addr_byte << 8u) | (uint32_t)cmd_byte;
    f.bit_length = DALI_FORWARD_FRAME_BITS;
    return f;
}

static DaliFrame make_frame24(uint8_t addr_byte, uint8_t inst_byte, uint8_t cmd_byte)
{
    DaliFrame f;
    f.data       = ((uint32_t)addr_byte << 16u) | ((uint32_t)inst_byte << 8u) | (uint32_t)cmd_byte;
    f.bit_length = DALI_EXTENDED_FRAME_BITS;
    return f;
}

static uint8_t group_addr_byte(uint8_t group, uint8_t selector)
{
    /* Group address byte: 1 0 0 G3 G2 G1 G0 S
     * S=0: DAPC; S=1: command */
    return (uint8_t)(0x80u | ((group & DALI_MAX_GROUP) << 1u) | (selector & 0x01u));
}

static DaliError target_addr_byte(DaliAddressType type,
                                  uint8_t address,
                                  uint8_t selector,
                                  uint8_t *addr_out)
{
    if (addr_out == NULL) {
        return DALI_ERR_INVALID;
    }

    switch (type) {
        case DALI_ADDR_SHORT:
            if (address >= DALI_SHORT_ADDRESS_COUNT) { return DALI_ERR_INVALID; }
            *addr_out = short_addr_byte(address, selector);
            return DALI_OK;

        case DALI_ADDR_GROUP:
            if (address >= DALI_GROUP_COUNT) { return DALI_ERR_INVALID; }
            *addr_out = group_addr_byte(address, selector);
            return DALI_OK;

        case DALI_ADDR_BROADCAST:
            *addr_out = selector == 0u ? DALI_BROADCAST_DAPC_ADDRESS
                                       : DALI_BROADCAST_COMMAND_ADDRESS;
            return DALI_OK;

        default:
            return DALI_ERR_INVALID;
    }
}

static bool instance_valid(uint8_t instance)
{
    return instance < DALI_INSTANCE_COUNT || instance == DALI_ALL_INSTANCES;
}

static DaliError command_opcode(const DaliCommandInfo *cmd,
                                uint8_t param,
                                uint8_t *opcode_out)
{
    if (cmd == NULL || opcode_out == NULL) {
        return DALI_ERR_INVALID;
    }

    if (cmd->opcode_first == cmd->opcode_last) {
        if (param != 0u) {
            return DALI_ERR_INVALID;
        }
        *opcode_out = cmd->opcode_first;
        return DALI_OK;
    }

    if ((uint32_t)cmd->opcode_first + (uint32_t)param > (uint32_t)cmd->opcode_last) {
        return DALI_ERR_INVALID;
    }
    *opcode_out = (uint8_t)(cmd->opcode_first + param);
    return DALI_OK;
}

DaliError dali_build_command(DaliAddressType type,
                             uint8_t address,
                             DaliCommandId id,
                             uint8_t param,
                             DaliFrame *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }

    const DaliCommandInfo *cmd = dali_command_lookup(id);
    if (cmd == NULL) {
        return DALI_ERR_INVALID;
    }

    uint8_t addr_byte;
    uint8_t opcode;

    if (cmd->frame_kind == DALI_CMD_FRAME_DAPC) {
        if (param > DALI_DAPC_MAX_LEVEL) {
            return DALI_ERR_INVALID;
        }
        DaliError err = target_addr_byte(type, address, 0u, &addr_byte);
        if (err != DALI_OK) {
            return err;
        }
        *out = make_frame16(addr_byte, param);
        return DALI_OK;
    }

    if (cmd->frame_kind != DALI_CMD_FRAME_16BIT) {
        return DALI_ERR_INVALID;
    }

    DaliError err = command_opcode(cmd, param, &opcode);
    if (err != DALI_OK) {
        return err;
    }
    err = target_addr_byte(type, address, 1u, &addr_byte);
    if (err != DALI_OK) {
        return err;
    }
    *out = make_frame16(addr_byte, opcode);
    return DALI_OK;
}

DaliError dali_build_instance_command(uint8_t addr,
                                      uint8_t instance,
                                      DaliCommandId id,
                                      DaliFrame *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT || !instance_valid(instance)) {
        return DALI_ERR_INVALID;
    }

    const DaliCommandInfo *cmd = dali_command_lookup(id);
    if (cmd == NULL || cmd->frame_kind != DALI_CMD_FRAME_24BIT_INST ||
        cmd->opcode_first != cmd->opcode_last) {
        return DALI_ERR_INVALID;
    }

    *out = dali_cmd_instance(addr, instance, cmd->opcode_first);
    return DALI_OK;
}

DaliError dali_build_device_command(uint8_t addr,
                                    DaliCommandId id,
                                    DaliFrame *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    const DaliCommandInfo *cmd = dali_command_lookup(id);
    if (cmd == NULL || cmd->frame_kind != DALI_CMD_FRAME_24BIT_DEV ||
        cmd->opcode_first != cmd->opcode_last) {
        return DALI_ERR_INVALID;
    }

    *out = dali_cmd_device(addr, cmd->opcode_first);
    return DALI_OK;
}

/* ---------------------------------------------------------------------------
 * 16-bit frame builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_cmd_dapc(uint8_t addr, uint8_t level)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_SHORT, addr, DALI_CMD_DAPC, level, &f);
    return f;
}

DaliFrame dali_cmd_group_dapc(uint8_t group, uint8_t level)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_GROUP, group, DALI_CMD_DAPC, level, &f);
    return f;
}

DaliFrame dali_cmd_broadcast_dapc(uint8_t level)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_BROADCAST, 0u, DALI_CMD_DAPC, level, &f);
    return f;
}

DaliFrame dali_cmd_recall_max(uint8_t addr)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_SHORT, addr, DALI_CMD_RECALL_MAX_LEVEL, 0u, &f);
    return f;
}

DaliFrame dali_cmd_group_recall_max(uint8_t group)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_GROUP, group, DALI_CMD_RECALL_MAX_LEVEL, 0u, &f);
    return f;
}

DaliFrame dali_cmd_recall_min(uint8_t addr)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_SHORT, addr, DALI_CMD_RECALL_MIN_LEVEL, 0u, &f);
    return f;
}

DaliFrame dali_cmd_group_recall_min(uint8_t group)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_GROUP, group, DALI_CMD_RECALL_MIN_LEVEL, 0u, &f);
    return f;
}

DaliFrame dali_cmd_broadcast_recall_min(void)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_BROADCAST, 0u, DALI_CMD_RECALL_MIN_LEVEL, 0u, &f);
    return f;
}

DaliFrame dali_cmd_off(uint8_t addr)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_SHORT, addr, DALI_CMD_OFF, 0u, &f);
    return f;
}

DaliFrame dali_cmd_group_off(uint8_t group)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_GROUP, group, DALI_CMD_OFF, 0u, &f);
    return f;
}

DaliFrame dali_cmd_query_status(uint8_t addr)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_SHORT, addr, DALI_CMD_QUERY_STATUS, 0u, &f);
    return f;
}

DaliFrame dali_cmd_group_query_status(uint8_t group)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_GROUP, group, DALI_CMD_QUERY_STATUS, 0u, &f);
    return f;
}

DaliFrame dali_cmd_query_actual_level(uint8_t addr)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_SHORT, addr, DALI_CMD_QUERY_ACTUAL_LEVEL, 0u, &f);
    return f;
}

DaliFrame dali_cmd_broadcast_off(void)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_BROADCAST, 0u, DALI_CMD_OFF, 0u, &f);
    return f;
}

DaliFrame dali_cmd_broadcast_recall_max(void)
{
    DaliFrame f = {0u, 0u};
    (void)dali_build_command(DALI_ADDR_BROADCAST, 0u, DALI_CMD_RECALL_MAX_LEVEL, 0u, &f);
    return f;
}

/* ---------------------------------------------------------------------------
 * DALI-2 24-bit instance command builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_cmd_instance(uint8_t addr, uint8_t instance, uint8_t cmd)
{
    return make_frame24(short_addr_byte(addr, 1u), instance, cmd);
}

DaliFrame dali_cmd_device(uint8_t addr, uint8_t cmd)
{
    return make_frame24(short_addr_byte(addr, 1u), DALI_DEVICE_INSTANCE, cmd);
}

DaliFrame dali_cmd_instance_group(uint8_t group, uint8_t instance, uint8_t cmd)
{
    return make_frame24(group_addr_byte(group, 1u), instance, cmd);
}

DaliFrame dali_cmd_instance_broadcast(uint8_t instance, uint8_t cmd)
{
    return make_frame24(DALI_BROADCAST_COMMAND_ADDRESS, instance, cmd);
}

/* ---------------------------------------------------------------------------
 * Response parsing
 * --------------------------------------------------------------------------*/

DaliError dali_parse_response(uint8_t raw, uint8_t *value_out)
{
    if (value_out == NULL) {
        return DALI_ERR_INVALID;
    }
    *value_out = raw;
    return DALI_OK;
}

bool dali_is_yes(uint8_t raw)
{
    return raw == DALI_YES_RESPONSE;
}

DaliError dali_parse_status(uint8_t raw, DaliStatus *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    out->ballast_failure       = (raw & 0x01u) != 0u;
    out->lamp_failure          = (raw & 0x02u) != 0u;
    out->lamp_arc_power_on     = (raw & 0x04u) != 0u;
    out->limit_error           = (raw & 0x08u) != 0u;
    out->fade_running          = (raw & 0x10u) != 0u;
    out->reset_state           = (raw & 0x20u) != 0u;
    out->missing_short_address = (raw & 0x40u) != 0u;
    out->power_failure         = (raw & 0x80u) != 0u;
    return DALI_OK;
}

DaliError dali_parse_fade_time_rate(uint8_t raw, DaliFadeTimeRate *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    out->fade_time = (uint8_t)((raw >> 4u) & 0x0Fu);
    out->fade_rate = (uint8_t)(raw & 0x0Fu);
    return DALI_OK;
}

DaliError dali_input_value_start(DaliInputValue *out, uint8_t expected_bytes)
{
    if (out == NULL || expected_bytes == 0u || expected_bytes > 4u) {
        return DALI_ERR_INVALID;
    }

    *out = (DaliInputValue){
        .value          = 0u,
        .byte_count     = 0u,
        .expected_bytes = expected_bytes,
        .complete       = false,
    };
    return DALI_OK;
}

DaliError dali_input_value_push(DaliInputValue *value, uint8_t byte)
{
    if (value == NULL || value->expected_bytes == 0u ||
        value->expected_bytes > 4u ||
        value->byte_count >= value->expected_bytes) {
        return DALI_ERR_INVALID;
    }

    value->value = (value->value << 8u) | (uint32_t)byte;
    value->byte_count++;
    value->complete = value->byte_count == value->expected_bytes;
    return DALI_OK;
}

DaliError dali_input_value_push_frame(DaliInputValue *value, const DaliFrame *frame)
{
    if (frame == NULL || frame->bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_INVALID;
    }
    return dali_input_value_push(value, (uint8_t)(frame->data & 0xFFu));
}

DaliError dali_input_value_parse_16(uint8_t msb, uint8_t lsb, uint16_t *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    *out = (uint16_t)(((uint16_t)msb << 8u) | (uint16_t)lsb);
    return DALI_OK;
}

DaliError dali_parse_by_kind(DaliResponseKind kind,
                             const DaliFrame *frame,
                             DaliParsedResponse *out)
{
    if (frame == NULL || out == NULL || frame->bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_INVALID;
    }

    uint8_t raw = (uint8_t)(frame->data & 0xFFu);
    *out = (DaliParsedResponse){
        .kind   = kind,
        .raw    = raw,
        .value  = raw,
        .bitset = raw,
        .yes    = false,
        .status = {0},
        .fade   = {0u, 0u},
    };

    switch (kind) {
        case DALI_RESP_NONE:
            return DALI_ERR_INVALID;

        case DALI_RESP_YES_NO:
            out->yes = dali_is_yes(raw);
            return DALI_OK;

        case DALI_RESP_UINT8:
        case DALI_RESP_BITSET8:
        case DALI_RESP_MEMORY_BYTE:
        case DALI_RESP_INPUT_VALUE_MSB:
        case DALI_RESP_INPUT_VALUE_LATCH:
            return DALI_OK;

        case DALI_RESP_STATUS:
            return dali_parse_status(raw, &out->status);

        case DALI_RESP_FADE_TIME_RATE:
            return dali_parse_fade_time_rate(raw, &out->fade);

        default:
            return DALI_ERR_INVALID;
    }
}

DaliError dali_parse_command_response(DaliCommandId id,
                                      const DaliFrame *frame,
                                      DaliParsedResponse *out)
{
    const DaliCommandInfo *cmd = dali_command_lookup(id);
    if (cmd == NULL) {
        return DALI_ERR_INVALID;
    }
    return dali_parse_by_kind(cmd->response_kind, frame, out);
}
