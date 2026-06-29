#include "dali_input_device.h"

#include <stddef.h>

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
