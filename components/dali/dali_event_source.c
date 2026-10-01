#include "dali_event_source.h"

#include <string.h>

_Static_assert(DALI_EVENT_SOURCE_PROFILE_STEPS <= DALI_SEQUENCE_MAX_STEPS,
               "the profile read must fit one scheduler sequence");

/* Step order of the profile read; the parser indexes replies by it. */
enum {
    PROFILE_STEP_TYPE = 0,
    PROFILE_STEP_SCHEME,
    PROFILE_STEP_GROUP_PRIMARY,
    PROFILE_STEP_GROUP_1,
    PROFILE_STEP_GROUP_2,
};

static const DaliCommandId k_profile_queries[DALI_EVENT_SOURCE_PROFILE_STEPS] = {
    [PROFILE_STEP_TYPE]          = DALI_CMD_QUERY_INSTANCE_TYPE,
    [PROFILE_STEP_SCHEME]        = DALI_CMD_QUERY_EVENT_SCHEME,
    [PROFILE_STEP_GROUP_PRIMARY] = DALI_CMD_QUERY_PRIMARY_INSTANCE_GROUP,
    [PROFILE_STEP_GROUP_1]       = DALI_CMD_QUERY_INSTANCE_GROUP_1,
    [PROFILE_STEP_GROUP_2]       = DALI_CMD_QUERY_INSTANCE_GROUP_2,
};

void dali_event_source_profile_clear(DaliEventSourceProfile *profile)
{
    if (profile == NULL) {
        return;
    }
    memset(profile, 0, sizeof(*profile));
    for (uint8_t i = 0u; i < DALI_EVENT_SOURCE_INSTANCE_GROUPS; i++) {
        profile->instance_groups[i] = DALI_EVENT_SOURCE_GROUP_NONE;
    }
}

static bool type_fits(const DaliInputEvent *event,
                      const DaliEventSourceProfile *profile)
{
    if (!event->source.has_instance_type ||
        profile == NULL || !profile->has_type) {
        return true;
    }
    return event->source.instance_type == profile->type;
}

static bool device_group_fits(uint8_t group, const DaliEventSourceProfile *profile)
{
    if (profile == NULL || !profile->has_device_groups) {
        return true;
    }
    return group < 32u && (profile->device_groups & ((uint32_t)1u << group)) != 0u;
}

static bool instance_group_fits(uint8_t group, const DaliEventSourceProfile *profile)
{
    if (profile == NULL || !profile->has_instance_groups) {
        return true;
    }
    for (uint8_t i = 0u; i < DALI_EVENT_SOURCE_INSTANCE_GROUPS; i++) {
        if (profile->instance_groups[i] != DALI_EVENT_SOURCE_GROUP_NONE &&
            profile->instance_groups[i] == group) {
            return true;
        }
    }
    return false;
}

static DaliEventSourceMatch possible_if(bool fits)
{
    return fits ? DALI_EVENT_SOURCE_MATCH_POSSIBLE : DALI_EVENT_SOURCE_MATCH_NONE;
}

DaliEventSourceMatch dali_event_source_match(const DaliInputEvent *event,
                                             uint8_t address,
                                             uint8_t instance,
                                             const DaliEventSourceProfile *profile)
{
    if (event == NULL ||
        event->frame_kind != DALI_EVENT_FRAME_INPUT_24BIT ||
        address >= DALI_SHORT_ADDRESS_COUNT ||
        instance >= DALI_INSTANCE_COUNT) {
        return DALI_EVENT_SOURCE_MATCH_NONE;
    }

    const DaliEventSource *src = &event->source;
    switch (src->scheme) {
        case DALI_EVENT_SOURCE_DEVICE_INSTANCE:
            /* The whole identity is on the wire; nothing to infer. */
            if (src->has_device_address && src->has_instance &&
                src->device_address == address && src->instance == instance) {
                return DALI_EVENT_SOURCE_MATCH_EXACT;
            }
            return DALI_EVENT_SOURCE_MATCH_NONE;

        case DALI_EVENT_SOURCE_DEVICE:
            return possible_if(src->has_device_address &&
                               src->device_address == address &&
                               type_fits(event, profile));

        case DALI_EVENT_SOURCE_INSTANCE:
            return possible_if(src->has_instance &&
                               src->instance == instance &&
                               type_fits(event, profile));

        case DALI_EVENT_SOURCE_DEVICE_GROUP:
            return possible_if(src->has_device_group &&
                               device_group_fits(src->device_group, profile) &&
                               type_fits(event, profile));

        case DALI_EVENT_SOURCE_INSTANCE_GROUP:
            return possible_if(src->has_instance_group &&
                               instance_group_fits(src->instance_group, profile) &&
                               type_fits(event, profile));

        case DALI_EVENT_SOURCE_POWER_NOTIFICATION:
        case DALI_EVENT_SOURCE_NONE:
        default:
            return DALI_EVENT_SOURCE_MATCH_NONE;
    }
}

bool dali_event_source_scheme_differs(const DaliInputEvent *event,
                                      const DaliEventSourceProfile *profile)
{
    if (event == NULL || profile == NULL || !profile->has_scheme ||
        event->frame_kind != DALI_EVENT_FRAME_INPUT_24BIT ||
        event->source.scheme > DALI_EVENT_SOURCE_INSTANCE_GROUP) {
        return false;
    }
    return (uint8_t)event->source.scheme != profile->scheme;
}

DaliError dali_event_source_build_profile_sequence(uint8_t address,
                                                   uint8_t instance,
                                                   DaliSequence *out)
{
    if (out == NULL ||
        address >= DALI_SHORT_ADDRESS_COUNT ||
        instance >= DALI_INSTANCE_COUNT) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0u; i < DALI_EVENT_SOURCE_PROFILE_STEPS; i++) {
        DaliError err = dali_build_instance_command(address, instance,
                                                    k_profile_queries[i],
                                                    &out->steps[i].frame);
        if (err != DALI_OK) {
            return err;
        }
        out->steps[i].needs_reply  = true;
        out->steps[i].retries_left = 1u;
    }
    out->step_count = DALI_EVENT_SOURCE_PROFILE_STEPS;
    return DALI_OK;
}

static bool profile_reply(const DaliSequenceResult *result,
                          uint8_t step,
                          uint8_t *value_out)
{
    DaliFrame reply;
    if (!dali_sequence_result_reply(result, step, &reply) ||
        reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return false;
    }
    *value_out = (uint8_t)(reply.data & 0xFFu);
    return true;
}

static bool group_value_valid(uint8_t value)
{
    return value < 32u || value == DALI_EVENT_SOURCE_GROUP_NONE;
}

DaliError dali_event_source_profile_from_sequence(const DaliSequenceResult *result,
                                                  DaliEventSourceProfile *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    dali_event_source_profile_clear(out);
    if (result == NULL) {
        return DALI_ERR_INVALID;
    }

    bool complete = true;
    uint8_t value;

    if (profile_reply(result, PROFILE_STEP_TYPE, &value) && value < 32u) {
        out->has_type = true;
        out->type     = value;
    } else {
        complete = false;
    }

    if (profile_reply(result, PROFILE_STEP_SCHEME, &value) &&
        value <= (uint8_t)DALI_EVENT_SOURCE_INSTANCE_GROUP) {
        out->has_scheme = true;
        out->scheme     = value;
    } else {
        complete = false;
    }

    uint8_t groups[DALI_EVENT_SOURCE_INSTANCE_GROUPS];
    bool groups_ok = true;
    for (uint8_t i = 0u; i < DALI_EVENT_SOURCE_INSTANCE_GROUPS; i++) {
        if (!profile_reply(result, (uint8_t)(PROFILE_STEP_GROUP_PRIMARY + i),
                           &groups[i]) ||
            !group_value_valid(groups[i])) {
            groups_ok = false;
            break;
        }
    }
    if (groups_ok) {
        out->has_instance_groups = true;
        memcpy(out->instance_groups, groups, sizeof(groups));
    } else {
        complete = false;
    }

    if (result->result != DALI_OK) {
        return result->result;
    }
    return complete ? DALI_OK : DALI_ERR_MALFORMED;
}
