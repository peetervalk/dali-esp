#include "dali_input_device.h"

#include <stddef.h>
#include <string.h>

static DaliInputRole role_for_type(uint8_t type)
{
    switch (type) {
        case DALI_INPUT_INSTANCE_TYPE_GENERIC:
            return DALI_INPUT_ROLE_GENERIC;
        case DALI_INPUT_INSTANCE_TYPE_PUSH_BUTTON:
            return DALI_INPUT_ROLE_PUSH_BUTTON;
        case DALI_INPUT_INSTANCE_TYPE_ABSOLUTE:
            return DALI_INPUT_ROLE_ABSOLUTE;
        case DALI_INPUT_INSTANCE_TYPE_OCCUPANCY:
            return DALI_INPUT_ROLE_OCCUPANCY;
        case DALI_INPUT_INSTANCE_TYPE_LIGHT:
            return DALI_INPUT_ROLE_LIGHT;
        default:
            return DALI_INPUT_ROLE_UNKNOWN;
    }
}

static DaliInputUsableState usable_for_role(DaliInputRole role)
{
    switch (role) {
        case DALI_INPUT_ROLE_PUSH_BUTTON:
        case DALI_INPUT_ROLE_ABSOLUTE:
        case DALI_INPUT_ROLE_OCCUPANCY:
        case DALI_INPUT_ROLE_LIGHT:
            return DALI_INPUT_USABLE_STANDARD;

        case DALI_INPUT_ROLE_GENERIC:
        case DALI_INPUT_ROLE_UNKNOWN:
        default:
            return DALI_INPUT_USABLE_DISCOVERED_ONLY;
    }
}

DaliError dali_input_build_query_number_of_instances(uint8_t addr, DaliFrame *out)
{
    return dali_build_device_command(addr, DALI_CMD_QUERY_NUMBER_OF_INSTANCES, out);
}

DaliError dali_input_build_query_content_dtr0(uint8_t addr, DaliFrame *out)
{
    return dali_build_device_command(addr, DALI_CMD_QUERY_DEVICE_CONTENT_DTR0, out);
}

DaliError dali_input_build_query_content_dtr1(uint8_t addr, DaliFrame *out)
{
    return dali_build_device_command(addr, DALI_CMD_QUERY_DEVICE_CONTENT_DTR1, out);
}

DaliError dali_input_build_query_content_dtr2(uint8_t addr, DaliFrame *out)
{
    return dali_build_device_command(addr, DALI_CMD_QUERY_DEVICE_CONTENT_DTR2, out);
}

static DaliError dtr_readback_command_id(DaliDtrRegister reg, DaliCommandId *out)
{
    switch (reg) {
        case DALI_DTR0: *out = DALI_CMD_QUERY_DEVICE_CONTENT_DTR0; return DALI_OK;
        case DALI_DTR1: *out = DALI_CMD_QUERY_DEVICE_CONTENT_DTR1; return DALI_OK;
        case DALI_DTR2: *out = DALI_CMD_QUERY_DEVICE_CONTENT_DTR2; return DALI_OK;
        default:        return DALI_ERR_INVALID;
    }
}

DaliError dali_input_build_dtr_check_sequence(uint8_t addr,
                                              DaliDtrRegister reg,
                                              uint8_t value,
                                              DaliSequence *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliCommandId query_id;
    DaliError err = dtr_readback_command_id(reg, &query_id);
    if (err != DALI_OK) {
        return err;
    }

    DaliFrame load;
    err = dali_build_control_device_dtr_data(reg, value, &load);
    if (err != DALI_OK) {
        return err;
    }

    DaliFrame readback;
    err = dali_build_device_command(addr, query_id, &readback);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequence seq;
    memset(&seq, 0, sizeof(seq));
    /* No retry budget on either step: a repeated DTR load is indistinguishable
     * from the caller's next value, and a repeated read would report on it. */
    seq.steps[0].frame       = load;
    seq.steps[1].frame       = readback;
    seq.steps[1].needs_reply = true;
    seq.step_count           = 2u;

    *out = seq;
    return DALI_OK;
}

DaliError dali_input_build_query_instance_type(uint8_t addr, uint8_t instance, DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_INSTANCE_TYPE, out);
}

DaliError dali_input_build_query_resolution(uint8_t addr, uint8_t instance, DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_RESOLUTION, out);
}

DaliError dali_input_build_query_instance_error(uint8_t addr, uint8_t instance, DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_INSTANCE_ERROR, out);
}

DaliError dali_input_build_query_instance_status(uint8_t addr, uint8_t instance, DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_INSTANCE_STATUS, out);
}

DaliError dali_input_build_query_instance_enabled(uint8_t addr, uint8_t instance, DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_INSTANCE_ENABLED, out);
}

DaliError dali_input_build_query_event_priority(uint8_t addr, uint8_t instance, DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_EVENT_PRIORITY, out);
}

DaliError dali_input_build_query_primary_instance_group(uint8_t addr,
                                                        uint8_t instance,
                                                        DaliFrame *out)
{
    return dali_build_instance_command(addr, instance,
                                       DALI_CMD_QUERY_PRIMARY_INSTANCE_GROUP,
                                       out);
}

DaliError dali_input_build_query_instance_group1(uint8_t addr, uint8_t instance, DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_INSTANCE_GROUP_1, out);
}

DaliError dali_input_build_query_instance_group2(uint8_t addr, uint8_t instance, DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_INSTANCE_GROUP_2, out);
}

DaliError dali_input_build_query_event_scheme(uint8_t addr, uint8_t instance, DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_EVENT_SCHEME, out);
}

DaliError dali_input_build_query_event_filter_zero(uint8_t addr,
                                                   uint8_t instance,
                                                   DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_EVENT_FILTER_0_7, out);
}

DaliError dali_input_build_query_event_filter_one(uint8_t addr,
                                                  uint8_t instance,
                                                  DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_EVENT_FILTER_8_15, out);
}

DaliError dali_input_build_query_event_filter_two(uint8_t addr,
                                                  uint8_t instance,
                                                  DaliFrame *out)
{
    return dali_build_instance_command(addr, instance, DALI_CMD_QUERY_EVENT_FILTER_16_23, out);
}

DaliError dali_input_build_query_instance_configuration(uint8_t addr,
                                                        uint8_t instance,
                                                        DaliFrame *out)
{
    return dali_build_instance_command(addr, instance,
                                       DALI_CMD_QUERY_INSTANCE_CONFIGURATION,
                                       out);
}

DaliError dali_input_build_query_available_instance_types(uint8_t addr,
                                                          uint8_t instance,
                                                          DaliFrame *out)
{
    return dali_build_instance_command(addr, instance,
                                       DALI_CMD_QUERY_AVAILABLE_INSTANCE_TYPES,
                                       out);
}

DaliError dali_input_classify_instance(uint8_t instance,
                                       uint8_t type,
                                       DaliInputInstanceInfo *out)
{
    if (out == NULL || instance >= DALI_INPUT_MAX_INSTANCES) {
        return DALI_ERR_INVALID;
    }

    DaliInputRole role = role_for_type(type);
    *out = (DaliInputInstanceInfo){
        .instance       = instance,
        .has_type       = true,
        .type           = type,
        .has_resolution = false,
        .resolution     = 0u,
        .has_enabled    = false,
        .enabled        = false,
        .has_status     = false,
        .status         = 0u,
        .has_error      = false,
        .error          = 0u,
        .role           = role,
        .role_source    = role == DALI_INPUT_ROLE_UNKNOWN
                        ? DALI_INPUT_ROLE_SOURCE_UNKNOWN
                        : DALI_INPUT_ROLE_SOURCE_STANDARD_TYPE,
        .usable         = usable_for_role(role),
    };
    return DALI_OK;
}

const char *dali_input_type_name(uint8_t type)
{
    switch (type) {
        case DALI_INPUT_INSTANCE_TYPE_GENERIC:
            return "generic";
        case DALI_INPUT_INSTANCE_TYPE_PUSH_BUTTON:
            return "push-button";
        case DALI_INPUT_INSTANCE_TYPE_ABSOLUTE:
            return "absolute";
        case DALI_INPUT_INSTANCE_TYPE_OCCUPANCY:
            return "occupancy";
        case DALI_INPUT_INSTANCE_TYPE_LIGHT:
            return "light";
        default:
            return "unknown";
    }
}

const char *dali_input_role_name(DaliInputRole role)
{
    switch (role) {
        case DALI_INPUT_ROLE_GENERIC:
            return "generic";
        case DALI_INPUT_ROLE_PUSH_BUTTON:
            return "push-button";
        case DALI_INPUT_ROLE_ABSOLUTE:
            return "absolute";
        case DALI_INPUT_ROLE_OCCUPANCY:
            return "occupancy";
        case DALI_INPUT_ROLE_LIGHT:
            return "light";
        case DALI_INPUT_ROLE_UNKNOWN:
        default:
            return "unknown";
    }
}

const char *dali_input_role_source_name(DaliInputRoleSource source)
{
    switch (source) {
        case DALI_INPUT_ROLE_SOURCE_STANDARD_TYPE:
            return "standard";
        case DALI_INPUT_ROLE_SOURCE_SELF_DESCRIBED:
            return "self-described";
        case DALI_INPUT_ROLE_SOURCE_VENDOR_PROFILE:
            return "vendor-profile";
        case DALI_INPUT_ROLE_SOURCE_USER_CONFIG:
            return "user-config";
        case DALI_INPUT_ROLE_SOURCE_UNKNOWN:
        default:
            return "unknown";
    }
}

const char *dali_input_usable_name(DaliInputUsableState usable)
{
    switch (usable) {
        case DALI_INPUT_USABLE_STANDARD:
            return "standard";
        case DALI_INPUT_USABLE_SELF_DESCRIBED:
            return "self-described";
        case DALI_INPUT_USABLE_PROFILE_UNVERIFIED:
            return "profile-unverified";
        case DALI_INPUT_USABLE_USER_CONFIRMED:
            return "user-confirmed";
        case DALI_INPUT_USABLE_DISCOVERED_ONLY:
        default:
            return "unverified";
    }
}
