#include "dali_lunatone.h"

#include <stddef.h>

#define LUNATONE_CMD(id, name, opcode, resp) { id, name, opcode, resp }

static const DaliLunatoneCommandInfo s_lunatone_commands[] = {
    LUNATONE_CMD(DALI_LUNATONE_QUERY_VALUE_MULTIPLICATOR,
                 "LUNATONE QUERY VALUE MULTIPLICATOR", 0x40u, DALI_RESP_UINT8),
    LUNATONE_CMD(DALI_LUNATONE_QUERY_VALUE_DIVISOR,
                 "LUNATONE QUERY VALUE DIVISOR", 0x41u, DALI_RESP_UINT8),
    LUNATONE_CMD(DALI_LUNATONE_QUERY_OFFSET_MSB,
                 "LUNATONE QUERY OFFSET MSB", 0x42u, DALI_RESP_UINT8),
    LUNATONE_CMD(DALI_LUNATONE_QUERY_OFFSET_LSB,
                 "LUNATONE QUERY OFFSET LSB", 0x43u, DALI_RESP_UINT8),
    LUNATONE_CMD(DALI_LUNATONE_QUERY_OFFSET_MULTIPLICATOR,
                 "LUNATONE QUERY OFFSET MULTIPLICATOR", 0x44u, DALI_RESP_UINT8),
    LUNATONE_CMD(DALI_LUNATONE_QUERY_OFFSET_DIVISOR,
                 "LUNATONE QUERY OFFSET DIVISOR", 0x45u, DALI_RESP_UINT8),
    LUNATONE_CMD(DALI_LUNATONE_QUERY_UNIT,
                 "LUNATONE QUERY UNIT", 0x46u, DALI_RESP_UINT8),
};

#define LUNATONE_COMMAND_COUNT \
    ((uint8_t)(sizeof(s_lunatone_commands) / sizeof(s_lunatone_commands[0])))

#undef LUNATONE_CMD

static bool lunatone_instance_valid(uint8_t instance)
{
    return instance < DALI_INSTANCE_COUNT || instance == DALI_ALL_INSTANCES;
}

const DaliLunatoneCommandInfo *dali_lunatone_command_lookup(DaliLunatoneCommandId id)
{
    for (uint8_t i = 0u; i < LUNATONE_COMMAND_COUNT; i++) {
        if (s_lunatone_commands[i].id == id) {
            return &s_lunatone_commands[i];
        }
    }
    return NULL;
}

const DaliLunatoneCommandInfo *dali_lunatone_command_lookup_opcode(uint8_t opcode)
{
    for (uint8_t i = 0u; i < LUNATONE_COMMAND_COUNT; i++) {
        if (s_lunatone_commands[i].opcode == opcode) {
            return &s_lunatone_commands[i];
        }
    }
    return NULL;
}

uint8_t dali_lunatone_command_count(void)
{
    return LUNATONE_COMMAND_COUNT;
}

DaliError dali_lunatone_build_instance_command(uint8_t addr,
                                               uint8_t instance,
                                               DaliLunatoneCommandId id,
                                               DaliFrame *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT ||
        !lunatone_instance_valid(instance)) {
        return DALI_ERR_INVALID;
    }

    const DaliLunatoneCommandInfo *cmd = dali_lunatone_command_lookup(id);
    if (cmd == NULL) {
        return DALI_ERR_INVALID;
    }

    *out = dali_cmd_instance(addr, instance, cmd->opcode);
    return DALI_OK;
}
