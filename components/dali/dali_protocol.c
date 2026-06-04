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

static DaliFrame make_frame24(uint8_t addr_byte, uint8_t inst_byte, uint8_t cmd_byte)
{
    DaliFrame f;
    f.data       = ((uint32_t)addr_byte << 16u) | ((uint32_t)inst_byte << 8u) | (uint32_t)cmd_byte;
    f.bit_length = 24u;
    return f;
}

static uint8_t group_addr_byte(uint8_t group)
{
    /* Group instance command address byte: 1 0 0 G3 G2 G1 G0 1
     * Bit 7=1 (group/broadcast), bits 4-1 = group 0-15, bit 0=1 (command) */
    return (uint8_t)(0x80u | ((group & 0x0Fu) << 1u) | 0x01u);
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
 * DALI-2 24-bit instance command builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_cmd_instance(uint8_t addr, uint8_t instance, uint8_t cmd)
{
    return make_frame24(short_addr_byte(addr, 1u), instance, cmd);
}

DaliFrame dali_cmd_instance_group(uint8_t group, uint8_t instance, uint8_t cmd)
{
    return make_frame24(group_addr_byte(group), instance, cmd);
}

DaliFrame dali_cmd_instance_broadcast(uint8_t instance, uint8_t cmd)
{
    return make_frame24(0xFFu, instance, cmd);
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
    return raw == 0xFFu;
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
