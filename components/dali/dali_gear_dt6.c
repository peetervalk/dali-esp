#include "dali_gear_dt6.h"

#include <stddef.h>

/* Build an addressed 16-bit DT6 command frame.
 * Format: address_byte = (addr << 1) | 1, opcode in low byte. */
static DaliFrame build_dt6_frame(uint8_t addr, uint8_t opcode)
{
    return (DaliFrame){
        .data       = ((uint32_t)((addr << 1u) | 1u) << 8u) | opcode,
        .bit_length = DALI_FORWARD_FRAME_BITS,
    };
}

DaliFrame dali_dt6_enable(void)
{
    return dali_cmd_enable_device_type(6u);
}

/* Configuration commands (IEC 62386-207 §9.2.3) */
DaliFrame dali_dt6_reference_system_power(uint8_t addr)      { return build_dt6_frame(addr, 0xE0u); }
DaliFrame dali_dt6_enable_current_protector(uint8_t addr)    { return build_dt6_frame(addr, 0xE1u); }
DaliFrame dali_dt6_disable_current_protector(uint8_t addr)   { return build_dt6_frame(addr, 0xE2u); }
DaliFrame dali_dt6_select_dimming_curve(uint8_t addr)        { return build_dt6_frame(addr, 0xE3u); }
DaliFrame dali_dt6_store_dtr_as_fast_fade_time(uint8_t addr) { return build_dt6_frame(addr, 0xE4u); }

/* Query commands (IEC 62386-207 §9.2.4) */
DaliFrame dali_dt6_query_gear_type(uint8_t addr)                   { return build_dt6_frame(addr, 0xEDu); }
DaliFrame dali_dt6_query_dimming_curve(uint8_t addr)               { return build_dt6_frame(addr, 0xEEu); }
DaliFrame dali_dt6_query_possible_operating_modes(uint8_t addr)    { return build_dt6_frame(addr, 0xEFu); }
DaliFrame dali_dt6_query_features(uint8_t addr)                    { return build_dt6_frame(addr, 0xF0u); }
DaliFrame dali_dt6_query_failure_status(uint8_t addr)              { return build_dt6_frame(addr, 0xF1u); }
DaliFrame dali_dt6_query_short_circuit(uint8_t addr)               { return build_dt6_frame(addr, 0xF2u); }
DaliFrame dali_dt6_query_open_circuit(uint8_t addr)                { return build_dt6_frame(addr, 0xF3u); }
DaliFrame dali_dt6_query_load_decrease(uint8_t addr)               { return build_dt6_frame(addr, 0xF4u); }
DaliFrame dali_dt6_query_load_increase(uint8_t addr)               { return build_dt6_frame(addr, 0xF5u); }
DaliFrame dali_dt6_query_current_protector_active(uint8_t addr)    { return build_dt6_frame(addr, 0xF6u); }
DaliFrame dali_dt6_query_thermal_shutdown(uint8_t addr)            { return build_dt6_frame(addr, 0xF7u); }
DaliFrame dali_dt6_query_thermal_overload(uint8_t addr)            { return build_dt6_frame(addr, 0xF8u); }
DaliFrame dali_dt6_query_reference_running(uint8_t addr)           { return build_dt6_frame(addr, 0xF9u); }
DaliFrame dali_dt6_query_reference_measurement_failed(uint8_t addr){ return build_dt6_frame(addr, 0xFAu); }
DaliFrame dali_dt6_query_current_protector_enabled(uint8_t addr)   { return build_dt6_frame(addr, 0xFBu); }
DaliFrame dali_dt6_query_operating_mode(uint8_t addr)              { return build_dt6_frame(addr, 0xFCu); }
DaliFrame dali_dt6_query_fast_fade_time(uint8_t addr)              { return build_dt6_frame(addr, 0xFDu); }
DaliFrame dali_dt6_query_min_fast_fade_time(uint8_t addr)          { return build_dt6_frame(addr, 0xFEu); }
DaliFrame dali_dt6_query_extended_version_number(uint8_t addr)     { return build_dt6_frame(addr, 0xFFu); }

DaliError dali_dt6_parse_failure_status(uint8_t raw, DaliDt6FailureStatus *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    out->led_short_circuit            = (raw & (1u << 0u)) != 0u;
    out->led_open_circuit             = (raw & (1u << 1u)) != 0u;
    out->load_decrease                = (raw & (1u << 2u)) != 0u;
    out->load_increase                = (raw & (1u << 3u)) != 0u;
    out->current_protector_active     = (raw & (1u << 4u)) != 0u;
    out->thermal_shutdown             = (raw & (1u << 5u)) != 0u;
    out->thermal_overload             = (raw & (1u << 6u)) != 0u;
    out->reference_measurement_failed = (raw & (1u << 7u)) != 0u;
    return DALI_OK;
}

DaliError dali_dt6_build_command_sequence(DaliFrame      command,
                                          bool           send_twice,
                                          bool           expects_reply,
                                          const uint8_t *dtr,
                                          uint8_t        dtr_count,
                                          DaliSequence  *out)
{
    if (out == NULL ||
        command.bit_length != DALI_FORWARD_FRAME_BITS ||
        dtr_count > DALI_DT6_MAX_DTR_BYTES ||
        (dtr_count > 0u && dtr == NULL)) {
        return DALI_ERR_INVALID;
    }

    *out = (DaliSequence){0};

    uint8_t step = 0u;
    for (uint8_t i = 0u; i < dtr_count; i++) {
        DaliFrame frame;
        DaliError err = dali_build_dtr_data((DaliDtrRegister)i, dtr[i], &frame);
        if (err != DALI_OK) {
            return err;
        }
        out->steps[step++] = (DaliSequenceStep){ .frame = frame };
    }

    out->steps[step++] = (DaliSequenceStep){ .frame = dali_dt6_enable() };
    out->steps[step++] = (DaliSequenceStep){
        .frame       = command,
        .needs_reply = expects_reply,
        .send_twice  = send_twice,
    };

    out->step_count = step;
    return DALI_OK;
}
