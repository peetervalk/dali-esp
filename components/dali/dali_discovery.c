#include "dali_discovery.h"
#include "dali_memory.h"
#include "dali_gear_dt6.h"
#include "dali_gear_dt8.h"

#include <string.h>

/* DALI-2 QUERY DEVICE TYPE / QUERY NEXT DEVICE TYPE sentinel replies. */
#define DALI_DISCOVERY_QUERY_RETRIES_LEFT      1u

_Static_assert(DALI_SEQUENCE_MAX_STEPS >= DALI_DISCOVERY_DEVICE_TYPES_SEQUENCE_STEPS,
               "device-type enumeration must fit in one sequence");
_Static_assert(DALI_SEQUENCE_MAX_STEPS >= DALI_DISCOVERY_PROFILE_DT6_STEPS,
               "gear profile must fit in one sequence");

typedef DaliError (*DaliDiscoveryInputQueryBuilder)(uint8_t addr,
                                                    uint8_t instance,
                                                    DaliFrame *out);

static void discovery_store_device_type(DaliDiscoveryDeviceInfo *device,
                                        uint8_t type);

static bool transport_valid(const DaliDiscoveryTransport *transport)
{
    return transport != NULL && transport->transact != NULL;
}

/* Send ENABLE DEVICE TYPE N and its query as one group, return the byte. */
static DaliError discovery_query_dt_typed(const DaliDiscoveryTransport *transport,
                                          uint8_t device_type,
                                          const DaliFrame *query,
                                          uint8_t *out)
{
    if (!transport_valid(transport) || out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliSequence seq;
    DaliError err = dali_discovery_build_device_type_query_sequence(device_type,
                                                                    query,
                                                                    &seq);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceResult result;
    err = dali_transport_run_sequence_atomic(transport, &seq, &result);
    if (err != DALI_OK) {
        return err;
    }
    return dali_discovery_u8_from_sequence(&result, DALI_DISCOVERY_DT_STEP_QUERY, out);
}

static DaliError discovery_query_dt6(const DaliDiscoveryTransport *transport,
                                     const DaliFrame *query,
                                     uint8_t *out)
{
    return discovery_query_dt_typed(transport, 6u, query, out);
}

static DaliError discovery_query_dt8(const DaliDiscoveryTransport *transport,
                                     const DaliFrame *query,
                                     uint8_t *out)
{
    return discovery_query_dt_typed(transport, 8u, query, out);
}

bool dali_discovery_has_device_type(const DaliDiscoveryDeviceInfo *device, uint8_t type)
{
    if (device == NULL || !device->has_device_type) {
        return false;
    }
    for (uint8_t i = 0u; i < device->device_type_count; i++) {
        if (device->device_types[i] == type) {
            return true;
        }
    }
    return false;
}

static bool scan_error_is_absent(DaliError err)
{
    return err == DALI_ERR_TIMEOUT || err == DALI_ERR_MALFORMED;
}

/*
 * Broadcast START QUIESCENT MODE, then wait for any event frame already on the
 * wire to finish.
 *
 * Send-twice, no reply, address byte 0xFF: every control device, control gear
 * untouched. Nothing acknowledges it, so a return of DALI_OK means the frame
 * was transmitted and says nothing about whether anything heard it.
 */
DaliError dali_discovery_quiescence_start(
    const DaliDiscoveryTransport *transport,
    bool *transmitted_out)
{
    if (!transport_valid(transport) || transmitted_out == NULL) {
        return DALI_ERR_INVALID;
    }
    *transmitted_out = false;

    DaliFrame frame;
    DaliError err = dali_build_device_broadcast_command(
        DALI_CMD_START_QUIESCENT_MODE, &frame);
    if (err != DALI_OK) {
        return err;
    }

    err = transport->transact(&frame, false, 0u, true, NULL, transport->ctx);
    if (err != DALI_OK) {
        return err;
    }
    *transmitted_out = true;

    return dali_transport_delay_ms(transport,
                                   DALI_DISCOVERY_QUIESCENT_SETTLE_MS);
}

/*
 * Broadcast STOP QUIESCENT MODE through the cleanup path.
 *
 * cleanup selects the transport that ignores a latched front-end cancellation,
 * because the alternative is an installation whose sensors stay silent after an
 * aborted walk. No settle follows: the caller is on its way out.
 */
DaliError dali_discovery_quiescence_release(
    const DaliDiscoveryTransport *transport)
{
    DaliFrame frame;
    DaliError err = dali_build_device_broadcast_command(
        DALI_CMD_STOP_QUIESCENT_MODE, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_transport_transact_cleanup(transport, &frame, false, 0u, true,
                                           NULL);
}

DaliError dali_discovery_inventory_reset(DaliDiscoveryInventory *inventory)
{
    if (inventory == NULL) {
        return DALI_ERR_INVALID;
    }

    memset(inventory, 0, sizeof(*inventory));
    return DALI_OK;
}

const DaliDiscoveryDeviceInfo *dali_discovery_inventory_get(
    const DaliDiscoveryInventory *inventory,
    uint8_t addr)
{
    if (inventory == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return NULL;
    }
    return &inventory->devices[addr];
}

DaliError dali_discovery_inventory_store_status(DaliDiscoveryInventory *inventory,
                                                uint8_t addr,
                                                uint8_t status)
{
    if (inventory == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliDiscoveryDeviceInfo *device = &inventory->devices[addr];
    if (!device->present) {
        inventory->found_count++;
    }
    device->present = true;
    device->has_status = true;
    device->has_control_gear = true;
    device->status = status;
    return DALI_OK;
}

DaliError dali_discovery_inventory_store_groups(DaliDiscoveryInventory *inventory,
                                                uint8_t addr,
                                                uint16_t groups)
{
    if (inventory == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliDiscoveryDeviceInfo *device = &inventory->devices[addr];
    device->has_groups = true;
    device->groups = groups;
    return DALI_OK;
}

bool dali_discovery_inventory_has_complete_group_data(
    const DaliDiscoveryInventory *inventory)
{
    if (inventory == NULL) {
        return false;
    }

    bool found_present_device = false;
    bool found_control_gear = false;
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device = &inventory->devices[addr];
        if (device->present) {
            found_present_device = true;
        }
        if (device->present && device->has_control_gear) {
            found_control_gear = true;
            if (!device->has_groups) {
                return false;
            }
        }
    }
    /* An input-only response proves the bus is alive and there was no observed
     * gear to enrich. A wholly empty scan is ambiguous with a disconnected bus
     * and must not erase a known-good snapshot. The integration additionally
     * checks that every previously known member was observed as gear. */
    return found_control_gear || found_present_device;
}

DaliError dali_discovery_inventory_update_input_device(
    DaliDiscoveryInventory *inventory,
    const DaliDiscoveryInputDevice *input_device)
{
    if (inventory == NULL ||
        input_device == NULL ||
        input_device->device.address >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    uint8_t addr = input_device->device.address;
    DaliDiscoveryDeviceInfo *device = &inventory->devices[addr];
    if (!device->present) {
        inventory->found_count++;
    }
    device->present = true;
    device->has_input_device = true;
    device->has_instance_count = input_device->device.has_instance_count;
    device->instance_count = input_device->device.instance_count;
    return DALI_OK;
}

/* ---------------------------------------------------------------------------
 * Sequence builders and result readers
 * --------------------------------------------------------------------------*/

DaliError dali_discovery_build_device_type_query_sequence(uint8_t device_type,
                                                          const DaliFrame *query,
                                                          DaliSequence *out)
{
    if (query == NULL || out == NULL ||
        query->bit_length != DALI_FORWARD_FRAME_BITS) {
        return DALI_ERR_INVALID;
    }

    /* The protocol layer owns the valid device-type range; a rejected type
     * comes back as a zeroed frame. */
    DaliFrame enable = dali_cmd_enable_device_type(device_type);
    if (enable.bit_length != DALI_FORWARD_FRAME_BITS) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->steps[DALI_DISCOVERY_DT_STEP_ENABLE].frame = enable;

    /* No retries: ENABLE DEVICE TYPE is consumed by the command that follows
     * it, so a lone retransmission of the query would be answered under the
     * device's default type. Retry the pair instead. */
    out->steps[DALI_DISCOVERY_DT_STEP_QUERY].frame       = *query;
    out->steps[DALI_DISCOVERY_DT_STEP_QUERY].needs_reply = true;

    out->step_count = DALI_DISCOVERY_DT_SEQUENCE_STEPS;
    return DALI_OK;
}

DaliError dali_discovery_build_profile_sequence(uint8_t addr,
                                                bool query_dt6_curve,
                                                DaliSequence *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliTarget target = { .type = DALI_ADDR_SHORT, .address = addr };
    DaliFrame min_query;
    DaliError err = dali_control_build_query(target, DALI_CMD_QUERY_MIN_LEVEL,
                                             0u, &min_query);
    if (err != DALI_OK) {
        return err;
    }
    DaliFrame max_query;
    err = dali_control_build_query(target, DALI_CMD_QUERY_MAX_LEVEL,
                                   0u, &max_query);
    if (err != DALI_OK) {
        return err;
    }

    memset(out, 0, sizeof(*out));

    DaliSequenceStep *min_step =
        &out->steps[DALI_DISCOVERY_PROFILE_STEP_MIN_LEVEL];
    min_step->frame = min_query;
    min_step->needs_reply = true;
    min_step->retries_left = DALI_DISCOVERY_QUERY_RETRIES_LEFT;

    DaliSequenceStep *max_step =
        &out->steps[DALI_DISCOVERY_PROFILE_STEP_MAX_LEVEL];
    max_step->frame = max_query;
    max_step->needs_reply = true;
    max_step->retries_left = DALI_DISCOVERY_QUERY_RETRIES_LEFT;

    if (!query_dt6_curve) {
        out->step_count = DALI_DISCOVERY_PROFILE_LIMIT_STEPS;
        return DALI_OK;
    }

    /* ENABLE DEVICE TYPE applies only to the command immediately following
     * it. Keep the pair adjacent and give neither step a local retry. */
    out->steps[DALI_DISCOVERY_PROFILE_STEP_DT6_ENABLE].frame = dali_dt6_enable();
    DaliSequenceStep *curve_step =
        &out->steps[DALI_DISCOVERY_PROFILE_STEP_CURVE];
    curve_step->frame = dali_dt6_query_dimming_curve(addr);
    curve_step->needs_reply = true;

    out->step_count = DALI_DISCOVERY_PROFILE_DT6_STEPS;
    return DALI_OK;
}

DaliError dali_discovery_build_groups_sequence(uint8_t addr, DaliSequence *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliTarget target = { .type = DALI_ADDR_SHORT, .address = addr };
    DaliFrame low_query;
    DaliError err = dali_control_build_query(target, DALI_CMD_QUERY_GROUPS_0_7,
                                             0u, &low_query);
    if (err != DALI_OK) {
        return err;
    }
    DaliFrame high_query;
    err = dali_control_build_query(target, DALI_CMD_QUERY_GROUPS_8_15,
                                   0u, &high_query);
    if (err != DALI_OK) {
        return err;
    }

    memset(out, 0, sizeof(*out));

    /* Both queries are read-only and independent, so a lone step may retry. */
    DaliSequenceStep *low = &out->steps[DALI_DISCOVERY_GROUPS_STEP_0_7];
    low->frame        = low_query;
    low->needs_reply  = true;
    low->retries_left = DALI_DISCOVERY_QUERY_RETRIES_LEFT;

    DaliSequenceStep *high = &out->steps[DALI_DISCOVERY_GROUPS_STEP_8_15];
    high->frame        = high_query;
    high->needs_reply  = true;
    high->retries_left = DALI_DISCOVERY_QUERY_RETRIES_LEFT;

    out->step_count = DALI_DISCOVERY_GROUPS_SEQUENCE_STEPS;
    return DALI_OK;
}

DaliError dali_discovery_build_device_types_sequence(uint8_t addr,
                                                     DaliSequence *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliTarget target = { .type = DALI_ADDR_SHORT, .address = addr };
    DaliFrame first_query;
    DaliError err = dali_control_build_query(target, DALI_CMD_QUERY_DEVICE_TYPE,
                                             0u, &first_query);
    if (err != DALI_OK) {
        return err;
    }
    DaliFrame next_query;
    err = dali_control_build_query(target, DALI_CMD_QUERY_NEXT_DEVICE_TYPE,
                                   0u, &next_query);
    if (err != DALI_OK) {
        return err;
    }

    memset(out, 0, sizeof(*out));

    /* No retries anywhere: QUERY NEXT DEVICE TYPE advances the device's
     * enumeration, so a repeated step would skip a type. The leading
     * QUERY DEVICE TYPE restarts the answer sequence, which is only meaningful
     * as the first step of this block, so it does not retry alone either. */
    out->steps[DALI_DISCOVERY_DEVICE_TYPES_STEP_FIRST].frame       = first_query;
    out->steps[DALI_DISCOVERY_DEVICE_TYPES_STEP_FIRST].needs_reply = true;

    for (uint8_t i = 0u; i < DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS; i++) {
        DaliSequenceStep *step = &out->steps[1u + i];
        step->frame       = next_query;
        step->needs_reply = true;
    }

    out->step_count = DALI_DISCOVERY_DEVICE_TYPES_SEQUENCE_STEPS;
    return DALI_OK;
}

DaliError dali_discovery_u8_from_sequence(const DaliSequenceResult *result,
                                          uint8_t step,
                                          uint8_t *out)
{
    if (result == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }
    if (result->result != DALI_OK) {
        return result->result;
    }

    DaliFrame reply;
    if (!dali_sequence_result_reply(result, step, &reply) ||
        reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_MALFORMED;
    }

    *out = (uint8_t)(reply.data & 0xFFu);
    return DALI_OK;
}

DaliError dali_discovery_groups_from_sequence(const DaliSequenceResult *result,
                                              uint16_t *groups_out)
{
    if (groups_out == NULL) {
        return DALI_ERR_INVALID;
    }

    uint8_t low = 0u;
    DaliError err = dali_discovery_u8_from_sequence(result,
                                                    DALI_DISCOVERY_GROUPS_STEP_0_7,
                                                    &low);
    if (err != DALI_OK) {
        return err;
    }

    uint8_t high = 0u;
    err = dali_discovery_u8_from_sequence(result,
                                          DALI_DISCOVERY_GROUPS_STEP_8_15,
                                          &high);
    if (err != DALI_OK) {
        return err;
    }

    *groups_out = (uint16_t)low | ((uint16_t)high << 8u);
    return DALI_OK;
}

/* Read one enumeration step, treating anything unusable as end-of-list. */
static bool device_types_step_value(const DaliSequenceResult *result,
                                    uint8_t step,
                                    uint8_t *out)
{
    DaliFrame reply;
    if (!dali_sequence_result_reply(result, step, &reply) ||
        reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return false;
    }
    *out = (uint8_t)(reply.data & 0xFFu);
    return true;
}

DaliError dali_discovery_device_types_from_sequence(const DaliSequenceResult *result,
                                                    DaliDiscoveryDeviceInfo *device)
{
    if (result == NULL || device == NULL) {
        return DALI_ERR_INVALID;
    }

    /* Deliberately not gated on result->result: a sequence that failed part-way
     * still carries the replies collected before the failing step, and those
     * types are as good as any. */
    uint8_t first = 0u;
    if (!device_types_step_value(result,
                                 DALI_DISCOVERY_DEVICE_TYPES_STEP_FIRST,
                                 &first) ||
        first == DALI_DISCOVERY_DEVICE_TYPE_NONE_OR_END) {
        return DALI_ERR_MALFORMED;
    }

    if (first != DALI_DISCOVERY_DEVICE_TYPE_MULTIPLE) {
        /* The device answered a single type after all. */
        discovery_store_device_type(device, first);
        return DALI_OK;
    }

    for (uint8_t i = 0u; i < DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS; i++) {
        uint8_t type = 0u;
        if (!device_types_step_value(result, (uint8_t)(1u + i), &type) ||
            type == DALI_DISCOVERY_DEVICE_TYPE_NONE_OR_END ||
            type == DALI_DISCOVERY_DEVICE_TYPE_MULTIPLE) {
            break;
        }
        /* The list must ascend; a repeat or a step backwards means the answer
         * sequence lost its place. */
        if (device->device_type_count > 0u &&
            type <= device->device_types[device->device_type_count - 1u]) {
            break;
        }

        discovery_store_device_type(device, type);
        if (device->device_types_truncated) {
            break;
        }
    }

    return device->device_type_count > 0u ? DALI_OK : DALI_ERR_MALFORMED;
}

/* Read one profile reply and preserve the error from a failed required step. */
static DaliError profile_step_u8(const DaliSequenceResult *result,
                                 uint8_t step,
                                 uint8_t *out)
{
    if (result == NULL || out == NULL || step >= DALI_SEQUENCE_MAX_STEPS) {
        return DALI_ERR_INVALID;
    }

    DaliFrame reply;
    if (dali_sequence_result_reply(result, step, &reply) &&
        reply.bit_length == DALI_BACKWARD_FRAME_BITS) {
        *out = (uint8_t)(reply.data & 0xFFu);
        return DALI_OK;
    }

    if (result->result != DALI_OK &&
        result->failed_step != DALI_SEQUENCE_NO_FAILED_STEP &&
        result->failed_step <= step) {
        return result->result;
    }
    return DALI_ERR_MALFORMED;
}

DaliError dali_discovery_profile_from_sequence(const DaliSequenceResult *result,
                                               bool query_dt6_curve,
                                               DaliDiscoveryGearProfile *profile)
{
    if (result == NULL || profile == NULL) {
        return DALI_ERR_INVALID;
    }

    uint8_t min_level = 0u;
    DaliError err = profile_step_u8(result,
                                    DALI_DISCOVERY_PROFILE_STEP_MIN_LEVEL,
                                    &min_level);
    if (err != DALI_OK) {
        return err;
    }

    uint8_t max_level = 0u;
    err = profile_step_u8(result,
                          DALI_DISCOVERY_PROFILE_STEP_MAX_LEVEL,
                          &max_level);
    if (err != DALI_OK) {
        return err;
    }

    /* 0 is OFF and 255 is MASK, not an addressable on-level. Commit neither
     * limit unless the pair is both legal and internally consistent. */
    if (min_level == 0u || min_level == DALI_DAPC_MASK_LEVEL ||
        max_level == 0u || max_level == DALI_DAPC_MASK_LEVEL ||
        min_level > max_level) {
        return DALI_ERR_MALFORMED;
    }

    /* The curve degrades on its own. QUERY DIMMING CURVE is a DT6 extension,
     * and gear that claims DT6 without answering it would otherwise discard a
     * perfectly good MIN/MAX pair — which is exactly the gear whose MIN matters
     * most. An unreadable curve keeps the last known one, or falls back to the
     * standard curve the whole 102 dimming model defaults to. */
    DaliDimCurve curve = DALI_DIM_CURVE_STANDARD;
    bool has_curve = true;
    if (query_dt6_curve) {
        uint8_t raw_curve = 0u;
        if (profile_step_u8(result, DALI_DISCOVERY_PROFILE_STEP_CURVE,
                            &raw_curve) == DALI_OK &&
            raw_curve <= DALI_DIM_CURVE_LINEAR) {
            curve = (DaliDimCurve)raw_curve;
        } else if (profile->has_dimming_curve) {
            curve = profile->dimming_curve;
        } else {
            has_curve = false;
        }
    }

    DaliDiscoveryGearProfile candidate = {
        .has_level_limits = true,
        .min_level = min_level,
        .max_level = max_level,
        .has_dimming_curve = has_curve,
        .dimming_curve = curve,
    };
    *profile = candidate;
    return DALI_OK;
}

DaliError dali_discovery_query_u8(const DaliDiscoveryTransport *transport,
                                  const DaliFrame *frame,
                                  uint8_t *out)
{
    if (!transport_valid(transport) || frame == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame reply = {0u, 0u};
    DaliError err = transport->transact(frame,
                                        true,
                                        DALI_DISCOVERY_QUERY_RETRIES_LEFT,
                                        false,
                                        &reply,
                                        transport->ctx);
    if (err != DALI_OK) {
        return err;
    }
    if (reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_MALFORMED;
    }

    *out = (uint8_t)(reply.data & 0xFFu);
    return DALI_OK;
}

DaliError dali_discovery_query_status(const DaliDiscoveryTransport *transport,
                                      uint8_t addr,
                                      uint8_t *status_out)
{
    if (!transport_valid(transport) || status_out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliTarget target = {
        .type = DALI_ADDR_SHORT,
        .address = addr,
    };
    DaliError err = dali_control_build_query_status(target, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_discovery_query_u8(transport, &frame, status_out);
}

static DaliError discovery_query_simple(const DaliDiscoveryTransport *transport,
                                        uint8_t addr,
                                        DaliCommandId id,
                                        uint8_t *out)
{
    DaliFrame frame;
    DaliTarget target = { .type = DALI_ADDR_SHORT, .address = addr };
    DaliError err = dali_control_build_query(target, id, 0u, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_discovery_query_u8(transport, &frame, out);
}

DaliError dali_discovery_query_groups(const DaliDiscoveryTransport *transport,
                                      uint8_t addr,
                                      uint16_t *groups_out)
{
    if (!transport_valid(transport) || groups_out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliSequence seq;
    DaliError err = dali_discovery_build_groups_sequence(addr, &seq);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceResult result;
    err = dali_transport_run_sequence_atomic(transport, &seq, &result);
    if (err != DALI_OK) {
        return err;
    }

    return dali_discovery_groups_from_sequence(&result, groups_out);
}

DaliError dali_discovery_query_device_type(const DaliDiscoveryTransport *transport,
                                           uint8_t addr,
                                           uint8_t *type_out)
{
    if (!transport_valid(transport) || type_out == NULL) {
        return DALI_ERR_INVALID;
    }
    return discovery_query_simple(transport, addr, DALI_CMD_QUERY_DEVICE_TYPE, type_out);
}

DaliError dali_discovery_query_version(const DaliDiscoveryTransport *transport,
                                       uint8_t addr,
                                       uint8_t *version_out)
{
    if (!transport_valid(transport) || version_out == NULL) {
        return DALI_ERR_INVALID;
    }
    return discovery_query_simple(transport, addr, DALI_CMD_QUERY_VERSION_NUMBER, version_out);
}

DaliError dali_discovery_query_actual_level(const DaliDiscoveryTransport *transport,
                                            uint8_t addr,
                                            uint8_t *level_out)
{
    if (!transport_valid(transport) || level_out == NULL) {
        return DALI_ERR_INVALID;
    }
    return discovery_query_simple(transport, addr, DALI_CMD_QUERY_ACTUAL_LEVEL, level_out);
}

static DaliError discovery_query_level_limit(const DaliDiscoveryTransport *transport,
                                             uint8_t addr,
                                             DaliCommandId command,
                                             uint8_t *level_out)
{
    if (!transport_valid(transport) || level_out == NULL) {
        return DALI_ERR_INVALID;
    }

    uint8_t level = 0u;
    DaliError err = discovery_query_simple(transport, addr, command, &level);
    if (err != DALI_OK) {
        return err;
    }
    if (level == 0u || level == DALI_DAPC_MASK_LEVEL) {
        return DALI_ERR_MALFORMED;
    }

    *level_out = level;
    return DALI_OK;
}

DaliError dali_discovery_query_min_level(const DaliDiscoveryTransport *transport,
                                         uint8_t addr,
                                         uint8_t *level_out)
{
    return discovery_query_level_limit(transport, addr, DALI_CMD_QUERY_MIN_LEVEL,
                                       level_out);
}

DaliError dali_discovery_query_max_level(const DaliDiscoveryTransport *transport,
                                         uint8_t addr,
                                         uint8_t *level_out)
{
    return discovery_query_level_limit(transport, addr, DALI_CMD_QUERY_MAX_LEVEL,
                                       level_out);
}

DaliError dali_discovery_query_dt6_dimming_curve(
    const DaliDiscoveryTransport *transport,
    uint8_t addr,
    DaliDimCurve *curve_out)
{
    if (!transport_valid(transport) || curve_out == NULL ||
        addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliFrame query = dali_dt6_query_dimming_curve(addr);
    uint8_t raw_curve = 0u;
    DaliError err = discovery_query_dt6(transport, &query, &raw_curve);
    if (err != DALI_OK) {
        return err;
    }
    if (raw_curve > DALI_DIM_CURVE_LINEAR) {
        return DALI_ERR_MALFORMED;
    }

    *curve_out = (DaliDimCurve)raw_curve;
    return DALI_OK;
}

const char *dali_discovery_device_type_name(uint8_t type)
{
    switch (type) {
    case 0:   return "fluorescent";
    case 1:   return "emergency";
    case 2:   return "HID";
    case 3:   return "halogen-LV";
    case 4:   return "incandescent";
    case 5:   return "DC-controlled";
    case 6:   return "LED";
    case 7:   return "switching";
    case 8:   return "colour";
    default:  return "unknown";
    }
}

static void discovery_store_device_type(DaliDiscoveryDeviceInfo *device,
                                        uint8_t type)
{
    if (device->device_type_count >= DALI_DISCOVERY_MAX_DEVICE_TYPES) {
        device->device_types_truncated = true;
        return;
    }

    device->device_types[device->device_type_count++] = type;
    if (!device->has_device_type) {
        device->has_device_type = true;
        device->device_type = type;
    }
}

static void discovery_enrich_device_types(const DaliDiscoveryTransport *transport,
                                          uint8_t addr,
                                          DaliDiscoveryDeviceInfo *device)
{
    uint8_t type = 0u;
    if (dali_discovery_query_device_type(transport, addr, &type) != DALI_OK ||
        type == DALI_DISCOVERY_DEVICE_TYPE_NONE_OR_END) {
        return;
    }

    if (type != DALI_DISCOVERY_DEVICE_TYPE_MULTIPLE) {
        discovery_store_device_type(device, type);
        return;
    }

    /* Multiple types: the reply to each QUERY NEXT DEVICE TYPE depends on how
     * many came before it, so the whole enumeration has to run as one block.
     * The plain query above stays outside it — that is the common single-type
     * path, and it keeps its retry budget. */
    DaliSequence seq;
    if (dali_discovery_build_device_types_sequence(addr, &seq) != DALI_OK) {
        return;
    }

    DaliSequenceResult result;
    (void)dali_transport_run_sequence_atomic(transport, &seq, &result);
    /* Types gathered before a failing step are still worth keeping, so the
     * result is read on both the success and the failure path. */
    (void)dali_discovery_device_types_from_sequence(&result, device);
}

static void discovery_enrich_gear_profile(const DaliDiscoveryTransport *transport,
                                          uint8_t addr,
                                          DaliDiscoveryDeviceInfo *device)
{
    bool query_dt6_curve = dali_discovery_has_device_type(device, 6u);
    DaliSequence seq;
    if (dali_discovery_build_profile_sequence(addr, query_dt6_curve, &seq) != DALI_OK) {
        return;
    }

    DaliSequenceResult result;
    (void)dali_transport_run_sequence_atomic(transport, &seq, &result);

    /* The parser commits only a complete profile, so a failed sequence leaves
     * this device's previously known-good metadata untouched. */
    DaliDiscoveryGearProfile profile = {
        .has_level_limits = device->has_level_limits,
        .min_level = device->min_level,
        .max_level = device->max_level,
        .has_dimming_curve = device->has_dimming_curve,
        .dimming_curve = device->dimming_curve,
    };
    if (dali_discovery_profile_from_sequence(&result,
                                             query_dt6_curve,
                                             &profile) != DALI_OK) {
        return;
    }

    device->has_level_limits = profile.has_level_limits;
    device->min_level = profile.min_level;
    device->max_level = profile.max_level;
    device->has_dimming_curve = profile.has_dimming_curve;
    device->dimming_curve = profile.dimming_curve;
}

static void discovery_enrich_device(const DaliDiscoveryTransport *transport,
                                    uint8_t addr,
                                    DaliDiscoveryDeviceInfo *device)
{
    DaliTarget target = { .type = DALI_ADDR_SHORT, .address = addr };

    uint16_t groups = 0u;
    if (dali_discovery_query_groups(transport, addr, &groups) == DALI_OK) {
        /* A valid gear-only group reply is positive evidence even when the
         * initial QUERY STATUS reply was lost. This also merges gear and input
         * roles that legitimately share the same numeric short address. */
        device->has_control_gear = true;
        device->has_groups = true;
        device->groups = groups;
    }

    discovery_enrich_device_types(transport, addr, device);

    uint8_t version = 0u;
    if (dali_discovery_query_version(transport, addr, &version) == DALI_OK) {
        device->has_version = true;
        device->version = version;
    }

    uint8_t level = 0u;
    if (dali_discovery_query_actual_level(transport, addr, &level) == DALI_OK) {
        device->has_actual_level = true;
        device->actual_level = level;
    }

    if (device->has_control_gear) {
        discovery_enrich_gear_profile(transport, addr, device);
    }

    DaliFrame instances_frame;
    if (dali_input_build_query_number_of_instances(addr, &instances_frame) == DALI_OK) {
        uint8_t count = 0u;
        DaliError inst_err =
            dali_discovery_query_u8(transport, &instances_frame, &count);
        if (inst_err == DALI_OK && count > 0u) {
            device->has_input_device = true;
            device->has_instance_count = true;
            device->instance_count = count;
        } else if (inst_err == DALI_ERR_RX_ACTIVITY && !device->has_input_device) {
            /*
             * The same finding the top-level scan records at line 1013, reached
             * by the other route. An address where control gear answered never
             * takes that path — the scan probes the device space only when the
             * gear query said absent — so before this, two control devices
             * sharing a device short address were invisible whenever any gear
             * happened to answer at the same number. That is not a corner case:
             * it is exactly the hybrid units, which answer in both spaces.
             *
             * Deliberately does not set `present` or touch the instance count,
             * for the reason the scan's copy does not: something is there and
             * nothing is known about it.
             *
             * Skipped when this address already has a good device-space
             * reading, which is the case on the input-only entry to this
             * function: the instance count was just read successfully, so a
             * second query that meets activity is a blip on one frame, not a
             * second control device, and treating it as one would throw away a
             * reading in hand.
             */
            device->has_undecodable_device_activity = true;
        }
    }

    if (device->has_control_gear) {
        DaliMemoryBank0Identity identity;
        if (dali_memory_read_bank0_identity(transport, addr, &identity) == DALI_OK) {
            device->has_identity = true;
            device->identity = identity;
        }
    }

    /*
     * The control device's own Bank 0, read from the device address space. Read
     * whenever an input device answered, independently of whether control gear
     * did: the two spaces are independent, and a unit answering both is not
     * thereby one physical device — the identification numbers decide that, and
     * this is what supplies them.
     */
    if (device->has_input_device) {
        DaliMemoryBank0Identity device_identity;
        if (dali_memory_read_device_bank0_identity(transport, addr,
                                                   &device_identity) == DALI_OK) {
            device->has_device_identity = true;
            device->device_identity = device_identity;
        }
    }

    /* Scene levels — 0xFF means scene not configured ("MASK"). */
    for (uint8_t scene = 0u; scene < DALI_SCENE_COUNT; scene++) {
        DaliFrame scene_q;
        if (dali_control_build_query(target, DALI_CMD_QUERY_SCENE_LEVEL,
                                      scene, &scene_q) != DALI_OK) {
            continue;
        }
        uint8_t scene_level = 0u;
        if (dali_discovery_query_u8(transport, &scene_q, &scene_level) == DALI_OK) {
            device->scene_levels[scene] = scene_level;
            device->has_scene_levels = true;
        }
    }

    if (dali_discovery_has_device_type(device, 6u)) {
        uint8_t failure = 0u;
        DaliFrame fail_q = dali_dt6_query_failure_status(addr);
        if (discovery_query_dt6(transport, &fail_q, &failure) == DALI_OK) {
            device->has_dt6 = true;
            device->dt6_failure_status = failure;
        }

        uint8_t features = 0u;
        DaliFrame feat_q = dali_dt6_query_features(addr);
        if (discovery_query_dt6(transport, &feat_q, &features) == DALI_OK) {
            device->dt6_features = features;
        }
    }

    if (dali_discovery_has_device_type(device, 8u)) {
        uint8_t gear_features = 0u;
        DaliFrame gf_q = dali_dt8_query_gear_features_status(addr);
        if (discovery_query_dt8(transport, &gf_q, &gear_features) == DALI_OK) {
            device->has_dt8 = true;
            device->dt8_gear_features = gear_features;
        }

        uint8_t colour_status = 0u;
        DaliFrame cs_q = dali_dt8_query_colour_status(addr);
        if (discovery_query_dt8(transport, &cs_q, &colour_status) == DALI_OK) {
            device->dt8_colour_status = colour_status;
        }

        uint8_t type_features = 0u;
        DaliFrame tf_q = dali_dt8_query_colour_type_features(addr);
        if (discovery_query_dt8(transport, &tf_q, &type_features) == DALI_OK) {
            device->dt8_colour_type_features = type_features;
        }
    }
}

/*
 * The walk itself, split out so that everything wrapped around it runs on every
 * exit path. There are five returns below and three of them are failures; a
 * release that only covered the clean one would leave the bus deaf to events
 * exactly when a scan had gone wrong.
 */
static DaliError discovery_scan_walk(DaliDiscoveryInventory *inventory,
                                     const DaliDiscoveryTransport *transport,
                                     DaliDiscoveryFoundCb found_cb,
                                     void *found_ctx,
                                     uint8_t *found_out)
{
    DaliError reset_err = dali_discovery_inventory_reset(inventory);
    if (reset_err != DALI_OK) {
        return reset_err;
    }

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        uint8_t status = 0u;
        DaliError err = dali_discovery_query_status(transport, addr, &status);
        if (err == DALI_OK) {
            err = dali_discovery_inventory_store_status(inventory, addr, status);
            if (err != DALI_OK) {
                return err;
            }
            discovery_enrich_device(transport, addr, &inventory->devices[addr]);
            /* Enrichment is the only route to a contested device address at a
             * number where gear answered; the inventory-level counter is kept
             * here because enrichment sees one device record, not the tally. */
            if (inventory->devices[addr].has_undecodable_device_activity) {
                inventory->undecodable_device_count++;
            }
            if (found_cb != NULL) {
                found_cb(addr, &inventory->devices[addr], found_ctx);
            }
            continue;
        }
        if (err == DALI_ERR_RX_ACTIVITY) {
            /*
             * Reply-window activity that could not be decoded. This is not
             * silence, so the address must not be reported free, and it is not
             * an answer, so no device may be invented from it. Record the
             * ambiguity, leave the entry absent, and keep scanning: one
             * contested address must not cost the other sixty-three.
             *
             * Skipping the input-device probe below is deliberate. The bus at
             * this address is already known to be undecodable, so a second
             * query buys no information.
             */
            inventory->devices[addr].has_undecodable_activity = true;
            inventory->undecodable_count++;
            continue;
        }
        if (!scan_error_is_absent(err)) {
            return err;
        }
        /* No gear status reply — try QUERY NUMBER OF INSTANCES to detect a
         * pure DALI-2 input device that has no control gear (Part 303). */
        DaliFrame inst_q;
        uint8_t count = 0u;
        if (dali_input_build_query_number_of_instances(addr, &inst_q) != DALI_OK) {
            continue;
        }

        DaliError inst_err = dali_discovery_query_u8(transport, &inst_q, &count);
        if (inst_err == DALI_ERR_RX_ACTIVITY) {
            /*
             * Something answered in the device address space and could not be
             * decoded — two control devices sharing a short address is the
             * expected cause. Recorded rather than dropped: every other
             * outcome here is silently treated as "no device", which made a
             * contested device address invisible instead of merely unreadable.
             *
             * It deliberately does not set `present` and does not touch
             * anything the control-gear free-address mask reads. The two
             * address spaces are independent, and reserving a gear address
             * because a control device collided at the same number would be a
             * different bug from the one this fixes.
             */
            inventory->devices[addr].has_undecodable_device_activity = true;
            inventory->undecodable_device_count++;
            continue;
        }
        if (inst_err == DALI_OK && count > 0u) {
            DaliDiscoveryDeviceInfo *device = &inventory->devices[addr];
            if (!device->present) {
                inventory->found_count++;
            }
            device->present          = true;
            device->has_input_device = true;
            device->has_instance_count = true;
            device->instance_count   = count;
            discovery_enrich_device(transport, addr, device);
            if (found_cb != NULL) {
                found_cb(addr, device, found_ctx);
            }
        }
    }

    inventory->valid = true;
    if (found_out != NULL) {
        *found_out = inventory->found_count;
    }
    return DALI_OK;
}

DaliError dali_discovery_scan_ex(DaliDiscoveryInventory *inventory,
                                 const DaliDiscoveryTransport *transport,
                                 DaliDiscoveryFoundCb found_cb,
                                 void *found_ctx,
                                 uint8_t *found_out,
                                 const DaliDiscoveryScanOptions *options,
                                 DaliDiscoveryScanResult *result_out)
{
    DaliDiscoveryScanResult result;
    memset(&result, 0, sizeof(result));

    if (inventory == NULL ||
        !transport_valid(transport) ||
        !dali_transport_supports_atomic_sequence(transport)) {
        if (result_out != NULL) {
            *result_out = result;
        }
        return DALI_ERR_INVALID;
    }

    /*
     * Validation first, so a rejected call never leaves control devices
     * quiesced by a bracket its walk was never going to run.
     */
    bool start_attempted = false;
    if (options != NULL && options->quiesce_control_devices) {
        result.quiescence_requested = true;
        if (!dali_transport_supports_delay(transport)) {
            /*
             * Without a delay there is no settle, so an event already on the
             * wire could still be mid-frame when the walk starts transmitting.
             * Quiescence whose settle did not run reads as hardening that is
             * not there, so it is refused outright rather than half-applied.
             */
            result.quiescence_error = DALI_ERR_INVALID;
        } else {
            start_attempted = true;
            bool started = false;
            result.quiescence_error =
                dali_discovery_quiescence_start(transport, &started);
            result.quiescence_started = started;
        }
    }

    DaliError err = discovery_scan_walk(inventory, transport, found_cb,
                                        found_ctx, found_out);

    /*
     * Released whenever a START was attempted, including when it reported
     * failure: a frame that transmitted and then failed its settle has still
     * reached the bus, and leaving that unreleased is the one outcome that
     * silences an installation. Only what was never attempted is left alone.
     */
    if (start_attempted) {
        result.quiescence_release_attempted = true;
        DaliError release_err = dali_discovery_quiescence_release(transport);
        result.quiescent_state_unknown =
            result.quiescence_started && release_err != DALI_OK;
        if (result.quiescence_error == DALI_OK) {
            result.quiescence_error = release_err;
        }
    }

    if (result_out != NULL) {
        *result_out = result;
    }
    return err;
}

DaliError dali_discovery_scan(DaliDiscoveryInventory *inventory,
                              const DaliDiscoveryTransport *transport,
                              DaliDiscoveryFoundCb found_cb,
                              void *found_ctx,
                              uint8_t *found_out)
{
    return dali_discovery_scan_ex(inventory, transport, found_cb, found_ctx,
                                  found_out, NULL, NULL);
}

static void discovery_input_reset(DaliDiscoveryInputDevice *out, uint8_t addr)
{
    memset(out, 0, sizeof(*out));
    out->device.address = addr;
    for (uint8_t i = 0u; i < DALI_INPUT_MAX_INSTANCES; i++) {
        out->device.instances[i].instance = i;
        out->instance_type_errors[i] = DALI_ERR_INVALID;
    }
}

static DaliError query_instance_u8(const DaliDiscoveryTransport *transport,
                                   DaliDiscoveryInputQueryBuilder builder,
                                   uint8_t addr,
                                   uint8_t instance,
                                   uint8_t *out)
{
    if (!transport_valid(transport) || builder == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliError err = builder(addr, instance, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_discovery_query_u8(transport, &frame, out);
}

static void query_instance_optional_fields(const DaliDiscoveryTransport *transport,
                                           uint8_t addr,
                                           DaliInputInstanceInfo *info)
{
    if (info == NULL) {
        return;
    }

    uint8_t raw = 0u;
    if (query_instance_u8(transport,
                          dali_input_build_query_instance_enabled,
                          addr,
                          info->instance,
                          &raw) == DALI_OK) {
        info->has_enabled = true;
        info->enabled = dali_is_yes(raw);
    }
    if (query_instance_u8(transport,
                          dali_input_build_query_resolution,
                          addr,
                          info->instance,
                          &raw) == DALI_OK) {
        info->has_resolution = true;
        info->resolution = raw;
    }
    if (query_instance_u8(transport,
                          dali_input_build_query_instance_status,
                          addr,
                          info->instance,
                          &raw) == DALI_OK) {
        info->has_status = true;
        info->status = raw;
    }
    if (query_instance_u8(transport,
                          dali_input_build_query_instance_error,
                          addr,
                          info->instance,
                          &raw) == DALI_OK) {
        info->has_error = true;
        info->error = raw;
    }
}

DaliError dali_discovery_query_input_device(const DaliDiscoveryTransport *transport,
                                            uint8_t addr,
                                            DaliDiscoveryInputDevice *out)
{
    if (!transport_valid(transport) ||
        out == NULL ||
        addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    discovery_input_reset(out, addr);

    DaliFrame count_frame;
    DaliError err = dali_input_build_query_number_of_instances(addr, &count_frame);
    if (err != DALI_OK) {
        return err;
    }

    uint8_t count_raw = 0u;
    err = dali_discovery_query_u8(transport, &count_frame, &count_raw);
    if (err != DALI_OK) {
        return err;
    }

    out->device.has_instance_count = true;
    out->device.instance_count = count_raw;

    uint8_t count = dali_discovery_input_visible_instance_count(out);
    for (uint8_t instance = 0u; instance < count; instance++) {
        uint8_t type = 0u;
        DaliError type_err = query_instance_u8(transport,
                                               dali_input_build_query_instance_type,
                                               addr,
                                               instance,
                                               &type);
        out->instance_type_errors[instance] = type_err;
        if (type_err != DALI_OK) {
            continue;
        }

        DaliInputInstanceInfo *info = &out->device.instances[instance];
        DaliError classify_err = dali_input_classify_instance(instance, type, info);
        if (classify_err != DALI_OK) {
            out->instance_type_errors[instance] = classify_err;
            continue;
        }

        query_instance_optional_fields(transport, addr, info);
    }

    return DALI_OK;
}

uint8_t dali_discovery_input_visible_instance_count(
    const DaliDiscoveryInputDevice *input_device)
{
    if (input_device == NULL || !input_device->device.has_instance_count) {
        return 0u;
    }

    uint8_t count = input_device->device.instance_count;
    return count > DALI_INPUT_MAX_INSTANCES ? DALI_INPUT_MAX_INSTANCES : count;
}
