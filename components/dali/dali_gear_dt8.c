#include "dali_gear_dt8.h"
#include "dali_protocol.h"
#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Transport-dependent colour value read
 * --------------------------------------------------------------------------*/

_Static_assert(DALI_SEQUENCE_MAX_STEPS >= DALI_DT8_COLOUR_VALUE_SEQUENCE_STEPS,
               "colour value read must fit in one sequence");

/* Copy one 8-bit answer out of a completed sequence step. */
static DaliError dt8_u8_from_sequence(const DaliSequenceResult *result,
                                      uint8_t                   step,
                                      uint8_t                  *out)
{
    DaliFrame reply;
    if (!dali_sequence_result_reply(result, step, &reply) ||
        reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_MALFORMED;
    }
    *out = (uint8_t)(reply.data & 0xFFu);
    return DALI_OK;
}

DaliError dali_dt8_build_colour_value_sequence(uint8_t                    addr,
                                               DaliDt8ColourValueSelector selector,
                                               DaliSequence              *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliFrame qdtr0;
    if (dali_build_command(DALI_ADDR_SHORT, addr,
                           DALI_CMD_QUERY_CONTENT_DTR0, 0u, &qdtr0) != DALI_OK) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    /* Step 1: DTR0 = selector (broadcast special, no reply) */
    out->steps[DALI_DT8_COLOUR_VALUE_STEP_DTR0].frame =
        dali_dt8_build_dtr0_selector(selector);

    /* Step 2: ENABLE DEVICE TYPE 8 (no reply) */
    out->steps[DALI_DT8_COLOUR_VALUE_STEP_ENABLE].frame = dali_dt8_enable();

    /* Step 3: QueryColourValue — reply is the MSB, and the gear leaves the LSB
     * in DTR0. No retries: the step is answered under the ENABLE above, and it
     * destroys the selector this sequence just wrote. */
    DaliSequenceStep *query = &out->steps[DALI_DT8_COLOUR_VALUE_STEP_QUERY];
    query->frame       = dali_dt8_query_colour_value(addr);
    query->needs_reply = true;

    /* Step 4: QUERY CONTENT DTR0 — reads the LSB without changing it, so this
     * step alone may safely retry. */
    DaliSequenceStep *read = &out->steps[DALI_DT8_COLOUR_VALUE_STEP_DTR0_READ];
    read->frame        = qdtr0;
    read->needs_reply  = true;
    read->retries_left = DALI_DT8_QUERY_RETRIES_LEFT;

    out->step_count = DALI_DT8_COLOUR_VALUE_SEQUENCE_STEPS;
    return DALI_OK;
}

DaliError dali_dt8_colour_value_from_sequence(const DaliSequenceResult *result,
                                              uint16_t                 *out)
{
    if (result == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }
    if (result->result != DALI_OK) {
        return result->result;
    }

    uint8_t msb = 0u;
    DaliError err = dt8_u8_from_sequence(result,
                                         DALI_DT8_COLOUR_VALUE_STEP_QUERY,
                                         &msb);
    if (err != DALI_OK) {
        return err;
    }

    uint8_t lsb = 0u;
    err = dt8_u8_from_sequence(result,
                               DALI_DT8_COLOUR_VALUE_STEP_DTR0_READ,
                               &lsb);
    if (err != DALI_OK) {
        return err;
    }

    *out = (uint16_t)(((uint16_t)msb << 8u) | (uint16_t)lsb);
    return DALI_OK;
}

DaliError dali_dt8_read_colour_value_16(const DaliDt8Transport    *transport,
                                        uint8_t                    addr,
                                        DaliDt8ColourValueSelector selector,
                                        uint16_t                  *out)
{
    if (!dali_transport_valid(transport) || out == NULL ||
        addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliSequence seq;
    DaliError err = dali_dt8_build_colour_value_sequence(addr, selector, &seq);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceResult result;
    err = dali_transport_run_sequence(transport, &seq, &result);
    if (err != DALI_OK) {
        return err;
    }

    return dali_dt8_colour_value_from_sequence(&result, out);
}

/* ---------------------------------------------------------------------------
 * Internal frame builder
 * --------------------------------------------------------------------------*/

static DaliFrame build_dt8_frame(uint8_t addr, uint8_t opcode)
{
    return (DaliFrame){
        .data       = ((uint32_t)((addr << 1u) | 1u) << 8u) | opcode,
        .bit_length = DALI_FORWARD_FRAME_BITS,
    };
}

/* ---------------------------------------------------------------------------
 * Response parsers
 * --------------------------------------------------------------------------*/

DaliError dali_dt8_parse_gear_features(uint8_t raw, DaliDt8GearFeatures *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    out->xy_capable        = (raw & DALI_DT8_FEATURE_XY_CAPABLE)        != 0u;
    out->tc_capable        = (raw & DALI_DT8_FEATURE_TC_CAPABLE)        != 0u;
    out->primary_n_capable = (raw & DALI_DT8_FEATURE_PRIMARY_N_CAPABLE) != 0u;
    out->rgbwaf_capable    = (raw & DALI_DT8_FEATURE_RGBWAF_CAPABLE)    != 0u;
    out->auto_calibration  = (raw & DALI_DT8_FEATURE_AUTO_CALIBRATION)  != 0u;
    return DALI_OK;
}

DaliError dali_dt8_parse_colour_status(uint8_t raw, DaliDt8ColourStatus *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    out->xy_active        = (raw & DALI_DT8_STATUS_XY_ACTIVE)        != 0u;
    out->tc_active        = (raw & DALI_DT8_STATUS_TC_ACTIVE)        != 0u;
    out->primary_n_active = (raw & DALI_DT8_STATUS_PRIMARY_N_ACTIVE) != 0u;
    out->rgbwaf_active    = (raw & DALI_DT8_STATUS_RGBWAF_ACTIVE)    != 0u;
    out->tc_out_of_range  = (raw & DALI_DT8_STATUS_TC_OUT_OF_RANGE)  != 0u;
    return DALI_OK;
}

/* ---------------------------------------------------------------------------
 * 16-bit DTR encoding helpers
 * --------------------------------------------------------------------------*/

DaliError dali_dt8_encode_16(uint16_t value,
                              DaliFrame *dtr0_out,
                              DaliFrame *dtr1_out)
{
    if (dtr0_out == NULL || dtr1_out == NULL) {
        return DALI_ERR_INVALID;
    }
    *dtr0_out = dali_cmd_dtr0_data((uint8_t)(value & 0xFFu));
    *dtr1_out = dali_cmd_dtr1_data((uint8_t)(value >> 8u));
    return DALI_OK;
}

DaliFrame dali_dt8_build_dtr0_selector(DaliDt8ColourValueSelector selector)
{
    return dali_cmd_dtr0_data((uint8_t)selector);
}

/* ---------------------------------------------------------------------------
 * Colour temperature conversions
 * --------------------------------------------------------------------------*/

uint16_t dali_dt8_kelvin_to_mirek(uint16_t kelvin)
{
    if (kelvin < 1u) {
        kelvin = 1u;
    }
    uint32_t mirek = 1000000UL / kelvin;
    return (mirek > 0xFFFFu) ? (uint16_t)0xFFFFu : (uint16_t)mirek;
}

uint16_t dali_dt8_mirek_to_kelvin(uint16_t mirek)
{
    if (mirek < 1u) {
        mirek = 1u;
    }
    uint32_t kelvin = 1000000UL / mirek;
    return (kelvin > 0xFFFFu) ? (uint16_t)0xFFFFu : (uint16_t)kelvin;
}

/* ---------------------------------------------------------------------------
 * Enable
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt8_enable(void)
{
    return dali_cmd_enable_device_type(8u);
}

/* ---------------------------------------------------------------------------
 * Temporary register command frames (XY mode)
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt8_set_temporary_x_coordinate(uint8_t addr)
{
    return build_dt8_frame(addr, 0xE0u);
}

DaliFrame dali_dt8_set_temporary_y_coordinate(uint8_t addr)
{
    return build_dt8_frame(addr, 0xE1u);
}

DaliFrame dali_dt8_x_coordinate_step_up(uint8_t addr)
{
    return build_dt8_frame(addr, 0xE3u);
}

DaliFrame dali_dt8_x_coordinate_step_down(uint8_t addr)
{
    return build_dt8_frame(addr, 0xE4u);
}

DaliFrame dali_dt8_y_coordinate_step_up(uint8_t addr)
{
    return build_dt8_frame(addr, 0xE5u);
}

DaliFrame dali_dt8_y_coordinate_step_down(uint8_t addr)
{
    return build_dt8_frame(addr, 0xE6u);
}

/* ---------------------------------------------------------------------------
 * Temporary register command frames (Tc mode)
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt8_set_temporary_colour_temperature(uint8_t addr)
{
    return build_dt8_frame(addr, 0xE7u);
}

DaliFrame dali_dt8_colour_temperature_step_cooler(uint8_t addr)
{
    return build_dt8_frame(addr, 0xE8u);
}

DaliFrame dali_dt8_colour_temperature_step_warmer(uint8_t addr)
{
    return build_dt8_frame(addr, 0xE9u);
}

/* ---------------------------------------------------------------------------
 * Temporary register command frames (RGBWAF mode)
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt8_set_temporary_primary_n_dim_level(uint8_t addr)
{
    return build_dt8_frame(addr, 0xEAu);
}

DaliFrame dali_dt8_set_temporary_rgb_dim_level(uint8_t addr)
{
    return build_dt8_frame(addr, 0xEBu);
}

DaliFrame dali_dt8_set_temporary_waf_dim_level(uint8_t addr)
{
    return build_dt8_frame(addr, 0xECu);
}

DaliFrame dali_dt8_set_temporary_rgbwaf_control(uint8_t addr)
{
    return build_dt8_frame(addr, 0xEDu);
}

/* ---------------------------------------------------------------------------
 * Activate and utility
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt8_activate(uint8_t addr)
{
    return build_dt8_frame(addr, 0xE2u);
}

DaliFrame dali_dt8_copy_report_to_temporary(uint8_t addr)
{
    return build_dt8_frame(addr, 0xEEu);
}

/* ---------------------------------------------------------------------------
 * Configuration command frames (send_twice)
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt8_store_ty_primary_n(uint8_t addr)
{
    return build_dt8_frame(addr, 0xF0u);
}

DaliFrame dali_dt8_store_xy_coordinate_primary_n(uint8_t addr)
{
    return build_dt8_frame(addr, 0xF1u);
}

DaliFrame dali_dt8_store_colour_temperature_tc_limit(uint8_t addr)
{
    return build_dt8_frame(addr, 0xF2u);
}

DaliFrame dali_dt8_store_gear_features_status(uint8_t addr)
{
    return build_dt8_frame(addr, 0xF3u);
}

DaliFrame dali_dt8_assign_colour_to_linked_channel(uint8_t addr)
{
    return build_dt8_frame(addr, 0xF5u);
}

DaliFrame dali_dt8_start_auto_calibration(uint8_t addr)
{
    return build_dt8_frame(addr, 0xF6u);
}

/* ---------------------------------------------------------------------------
 * Query command frames
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt8_query_gear_features_status(uint8_t addr)
{
    return build_dt8_frame(addr, 0xF7u);
}

DaliFrame dali_dt8_query_colour_status(uint8_t addr)
{
    return build_dt8_frame(addr, 0xF8u);
}

DaliFrame dali_dt8_query_colour_type_features(uint8_t addr)
{
    return build_dt8_frame(addr, 0xF9u);
}

DaliFrame dali_dt8_query_colour_value(uint8_t addr)
{
    return build_dt8_frame(addr, 0xFAu);
}

DaliFrame dali_dt8_query_rgbwaf_control(uint8_t addr)
{
    return build_dt8_frame(addr, 0xFBu);
}

DaliFrame dali_dt8_query_assigned_colour(uint8_t addr)
{
    return build_dt8_frame(addr, 0xFCu);
}

DaliFrame dali_dt8_query_extended_version_number(uint8_t addr)
{
    return build_dt8_frame(addr, 0xFFu);
}
