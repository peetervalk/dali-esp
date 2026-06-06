#pragma once

/*
 * dali_input_device.h - generic DALI-2 input-device discovery helpers
 *
 * This module only handles standard DALI-2 control-device/input-instance
 * structure. Vendor profiles such as Steinel and Lunatone stay in their own
 * modules and can annotate these results later.
 */

#include "dali_protocol.h"

#define DALI_INPUT_MAX_INSTANCES 32u
#define DALI_INPUT_INSTANCE_DEVICE 0xFEu
#define DALI_INPUT_INSTANCE_ALL    0xFFu

typedef enum {
    DALI_INPUT_INSTANCE_TYPE_GENERIC     = 0,
    DALI_INPUT_INSTANCE_TYPE_PUSH_BUTTON = 1,
    DALI_INPUT_INSTANCE_TYPE_ABSOLUTE    = 2,
    DALI_INPUT_INSTANCE_TYPE_OCCUPANCY   = 3,
    DALI_INPUT_INSTANCE_TYPE_LIGHT       = 4,
} DaliInputInstanceType;

typedef enum {
    DALI_INPUT_ROLE_UNKNOWN = 0,
    DALI_INPUT_ROLE_GENERIC,
    DALI_INPUT_ROLE_PUSH_BUTTON,
    DALI_INPUT_ROLE_ABSOLUTE,
    DALI_INPUT_ROLE_OCCUPANCY,
    DALI_INPUT_ROLE_LIGHT,
} DaliInputRole;

typedef enum {
    DALI_INPUT_ROLE_SOURCE_UNKNOWN = 0,
    DALI_INPUT_ROLE_SOURCE_STANDARD_TYPE,
    DALI_INPUT_ROLE_SOURCE_SELF_DESCRIBED,
    DALI_INPUT_ROLE_SOURCE_VENDOR_PROFILE,
    DALI_INPUT_ROLE_SOURCE_USER_CONFIG,
} DaliInputRoleSource;

typedef enum {
    DALI_INPUT_USABLE_DISCOVERED_ONLY = 0,
    DALI_INPUT_USABLE_STANDARD,
    DALI_INPUT_USABLE_SELF_DESCRIBED,
    DALI_INPUT_USABLE_PROFILE_UNVERIFIED,
    DALI_INPUT_USABLE_USER_CONFIRMED,
} DaliInputUsableState;

typedef struct {
    uint8_t              instance;
    bool                 has_type;
    uint8_t              type;
    bool                 has_resolution;
    uint8_t              resolution;
    bool                 has_enabled;
    bool                 enabled;
    bool                 has_status;
    uint8_t              status;
    bool                 has_error;
    uint8_t              error;
    DaliInputRole        role;
    DaliInputRoleSource  role_source;
    DaliInputUsableState usable;
} DaliInputInstanceInfo;

typedef struct {
    uint8_t address;
    bool    has_instance_count;
    uint8_t instance_count;
    DaliInputInstanceInfo instances[DALI_INPUT_MAX_INSTANCES];
} DaliInputDeviceInfo;

DaliError dali_input_build_query_number_of_instances(uint8_t addr, DaliFrame *out);
DaliError dali_input_build_query_instance_type(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_resolution(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_instance_error(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_instance_status(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_instance_enabled(uint8_t addr, uint8_t instance, DaliFrame *out);

DaliError dali_input_classify_instance(uint8_t instance,
                                       uint8_t type,
                                       DaliInputInstanceInfo *out);

const char *dali_input_type_name(uint8_t type);
const char *dali_input_role_name(DaliInputRole role);
const char *dali_input_role_source_name(DaliInputRoleSource source);
const char *dali_input_usable_name(DaliInputUsableState usable);
