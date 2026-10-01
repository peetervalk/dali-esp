#pragma once

/*
 * dali_input_device.h - generic DALI-2 input-device discovery helpers
 *
 * This module only handles standard DALI-2 control-device/input-instance
 * structure. Vendor profiles such as Steinel and Lunatone stay in their own
 * modules and can annotate these results later.
 */

#include "dali_protocol.h"
#include "dali_scheduler.h"   /* DaliSequence, for the DTR check sequence */

#define DALI_INPUT_MAX_INSTANCES    DALI_INSTANCE_COUNT
#define DALI_INPUT_INSTANCE_DEVICE  DALI_DEVICE_INSTANCE
#define DALI_INPUT_INSTANCE_ALL     DALI_ALL_INSTANCES

/* An instance belongs to up to three instance groups: primary, 1 and 2. A slot
 * holding MASK is unused. */
#define DALI_INPUT_INSTANCE_GROUP_SLOTS 3u
#define DALI_INPUT_INSTANCE_GROUP_NONE  0xFFu

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
    /* How the instance addresses its events, and to whom. The scheme decides
     * whether an integration can tell which instance an event came from
     * without asking: only Device/Instance (2) carries the pair it polls by.
     * Raw values, as read. */
    bool                 has_event_scheme;
    uint8_t              event_scheme;
    bool                 has_event_priority;
    uint8_t              event_priority;
    /* All three slots, or none: a partial set would misreport membership. */
    bool                 has_instance_groups;
    uint8_t              instance_groups[DALI_INPUT_INSTANCE_GROUP_SLOTS];
    /* Stored as bytes, not as their enum types. An enum is int-sized, and the
     * shell caches SHELL_INPUT_CACHE_MAX devices of 32 of these each, so three
     * int fields cost 4.5 kB of static RAM where three bytes cost 1.5 kB. */
    uint8_t              role;          /* DaliInputRole */
    uint8_t              role_source;   /* DaliInputRoleSource */
    uint8_t              usable;        /* DaliInputUsableState */
} DaliInputInstanceInfo;

typedef struct {
    uint8_t address;
    bool    has_instance_count;
    uint8_t instance_count;
    DaliInputInstanceInfo instances[DALI_INPUT_MAX_INSTANCES];
} DaliInputDeviceInfo;

/*
 * The generic IEC 62386-103 configuration of one instance: what a device RESET
 * returns to its defaults, and so what a backup records and a restore puts
 * back. Type-specific settings -- Part 301 timers, Part 303 hold and report
 * timers, Part 304 hysteresis -- are not part of it. Each field comes from its
 * own query and is optional for that reason; the type is recorded so a restore
 * can refuse to configure an instance that is no longer the same kind of thing.
 */
typedef struct {
    uint32_t event_filter;      /* eventFilter bits 0-23 */
    bool     has_type;
    uint8_t  type;
    bool     has_enabled;
    bool     enabled;
    bool     has_event_scheme;
    uint8_t  event_scheme;      /* 0-4 */
    bool     has_event_priority;
    uint8_t  event_priority;    /* 2-5 when valid */
    bool     has_event_filter;
    bool     has_instance_groups;
    uint8_t  instance_groups[DALI_INPUT_INSTANCE_GROUP_SLOTS]; /* 0-31 or NONE */
} DaliInstanceSettings;

DaliError dali_input_build_query_number_of_instances(uint8_t addr, DaliFrame *out);

/* Read back a control device's DTR registers (Part 103 device frames 0x36-0x38).
 * Pair one of these with a control-device DTR load in a single sequence to
 * verify that the device accepted the value: nothing else confirms a DTR write,
 * because the load itself produces no reply. */
DaliError dali_input_build_query_content_dtr0(uint8_t addr, DaliFrame *out);
DaliError dali_input_build_query_content_dtr1(uint8_t addr, DaliFrame *out);
DaliError dali_input_build_query_content_dtr2(uint8_t addr, DaliFrame *out);

/*
 * Build "load DTR<reg> = value, then read DTR<reg> back" as one sequence.
 *
 * The two frames must not be separately scheduled: any other locally scheduled
 * DTR write landing between them would be what the read returns, so the check
 * would report on the wrong value. reg selects DTR0, DTR1, or DTR2.
 */
DaliError dali_input_build_dtr_check_sequence(uint8_t addr,
                                              DaliDtrRegister reg,
                                              uint8_t value,
                                              DaliSequence *out);
/*
 * Quiescent mode (Part 103 device level, opcodes 0x1D/0x1E, instance byte
 * 0xFE). enable selects START, otherwise STOP.
 *
 * Both are send-twice: the caller must transmit the returned frame twice within
 * the 100 ms window, which for this project means handing it to the scheduler
 * with send_twice set rather than enqueueing it twice.
 *
 * A quiesced device stops producing event frames, so anything driven by one —
 * an occupancy light, a wall switch — goes silent until STOP is sent. Whether
 * the standard also ends the state on its own timer is not established here;
 * treat STOP as the only thing that reliably releases it.
 */
DaliError dali_input_build_quiescent_mode(uint8_t addr, bool enable, DaliFrame *out);

/* The same, addressed to every control device at once (address byte 0xFF). */
DaliError dali_input_build_quiescent_mode_broadcast(bool enable, DaliFrame *out);

DaliError dali_input_build_query_instance_type(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_resolution(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_instance_error(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_instance_status(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_instance_enabled(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_event_priority(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_primary_instance_group(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_instance_group1(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_instance_group2(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_event_scheme(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_event_filter_zero(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_event_filter_one(uint8_t addr, uint8_t instance, DaliFrame *out);
DaliError dali_input_build_query_event_filter_two(uint8_t addr, uint8_t instance, DaliFrame *out);
/* DTR0 selects the configuration index. DTR2:DTR1 holds the selected 16-bit
 * value; the backward byte repeats its least-significant byte. */
DaliError dali_input_build_query_instance_configuration(uint8_t addr, uint8_t instance, DaliFrame *out);

/* The backward byte and DTR2:DTR1:DTR0 together form the 32-bit available-type
 * bitmap. Reading the complete value requires an atomic follow-up DTR read. */
DaliError dali_input_build_query_available_instance_types(uint8_t addr, uint8_t instance, DaliFrame *out);

DaliError dali_input_classify_instance(uint8_t instance,
                                       uint8_t type,
                                       DaliInputInstanceInfo *out);

const char *dali_input_type_name(uint8_t type);
const char *dali_input_role_name(DaliInputRole role);
const char *dali_input_role_source_name(DaliInputRoleSource source);
const char *dali_input_usable_name(DaliInputUsableState usable);
