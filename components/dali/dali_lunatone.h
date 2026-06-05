#pragma once

/*
 * dali_lunatone.h - Lunatone-specific DALI-2 sensor helpers
 *
 * These commands are documented by Lunatone for their sensor instances. They
 * are 24-bit instance frames, but they are not generic IEC 62386-103 commands.
 */

#include "dali_protocol.h"

typedef enum {
    DALI_LUNATONE_QUERY_VALUE_MULTIPLICATOR = 0,
    DALI_LUNATONE_QUERY_VALUE_DIVISOR,
    DALI_LUNATONE_QUERY_OFFSET_MSB,
    DALI_LUNATONE_QUERY_OFFSET_LSB,
    DALI_LUNATONE_QUERY_OFFSET_MULTIPLICATOR,
    DALI_LUNATONE_QUERY_OFFSET_DIVISOR,
    DALI_LUNATONE_QUERY_UNIT,
} DaliLunatoneCommandId;

typedef struct {
    DaliLunatoneCommandId id;
    const char           *name;
    uint8_t               opcode;
    DaliResponseKind      response_kind;
} DaliLunatoneCommandInfo;

const DaliLunatoneCommandInfo *dali_lunatone_command_lookup(DaliLunatoneCommandId id);
const DaliLunatoneCommandInfo *dali_lunatone_command_lookup_opcode(uint8_t opcode);
uint8_t dali_lunatone_command_count(void);

DaliError dali_lunatone_build_instance_command(uint8_t addr,
                                               uint8_t instance,
                                               DaliLunatoneCommandId id,
                                               DaliFrame *out);
