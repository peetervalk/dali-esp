#include "dali_protocol.h"

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

static uint8_t short_addr_byte(uint8_t addr, uint8_t selector)
{
    return (uint8_t)((addr << 1u) | (selector & 0x01u));
}

static DaliFrame make_frame16(uint8_t addr_byte, uint8_t cmd_byte)
{
    DaliFrame f;
    f.data       = ((uint32_t)addr_byte << 8u) | (uint32_t)cmd_byte;
    f.bit_length = 16u;
    return f;
}

/* ---------------------------------------------------------------------------
 * 16-bit frame builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_cmd_dapc(uint8_t addr, uint8_t level)
{
    return make_frame16(short_addr_byte(addr, 0u), level);
}

DaliFrame dali_cmd_recall_max(uint8_t addr)
{
    return make_frame16(short_addr_byte(addr, 1u), 0x05u);
}

DaliFrame dali_cmd_recall_min(uint8_t addr)
{
    return make_frame16(short_addr_byte(addr, 1u), 0x06u);
}

DaliFrame dali_cmd_off(uint8_t addr)
{
    return make_frame16(short_addr_byte(addr, 1u), 0x00u);
}

DaliFrame dali_cmd_query_status(uint8_t addr)
{
    return make_frame16(short_addr_byte(addr, 1u), 0x90u);
}

DaliFrame dali_cmd_query_actual_level(uint8_t addr)
{
    return make_frame16(short_addr_byte(addr, 1u), 0xA0u);
}

DaliFrame dali_cmd_broadcast_off(void)
{
    return make_frame16(0xFFu, 0x00u);
}

DaliFrame dali_cmd_broadcast_recall_max(void)
{
    return make_frame16(0xFFu, 0x05u);
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
