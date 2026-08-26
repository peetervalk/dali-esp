#pragma once

/*
 * dali_protocol.h — DALI frame construction and response parsing
 *
 * Responsibilities:
 *   - Stateless frame builders for standard 16-bit DALI commands
 *   - Stateless frame builders for standard DALI-2 24-bit instance commands
 *   - Response parsing (8-bit backward frames)
 *
 * Vendor-specific commands and device profiles live in dedicated helper
 * modules so IEC/common behavior stays separate from custom behavior.
 *
 * No hardware dependencies.  No ESPHome dependencies.
 * All functions are pure (no global state modified).
 */

#include "dali_frame.h"

/* ---------------------------------------------------------------------------
 * Command metadata
 * --------------------------------------------------------------------------*/

typedef enum {
    DALI_CMD_FRAME_DAPC       = 0,
    DALI_CMD_FRAME_16BIT      = 1,
    DALI_CMD_FRAME_SPECIAL    = 2,
    DALI_CMD_FRAME_24BIT_INST = 3,
    DALI_CMD_FRAME_24BIT_DEV  = 4,
    /*
     * IEC 62386-103 special commands: a 24-bit frame whose first byte is the
     * fixed 0xC1 special-command address rather than a device address.
     * Distinct from DALI_CMD_FRAME_24BIT_DEV, which addresses devices.
     */
    DALI_CMD_FRAME_24BIT_SPECIAL = 5,
} DaliCommandFrameKind;

typedef enum {
    DALI_RESP_NONE              = 0,
    DALI_RESP_YES_NO            = 1,
    DALI_RESP_UINT8             = 2,
    DALI_RESP_STATUS            = 3,
    DALI_RESP_BITSET8           = 4,
    DALI_RESP_MEMORY_BYTE       = 5,
    DALI_RESP_INPUT_VALUE_MSB   = 6,
    DALI_RESP_INPUT_VALUE_LATCH = 7,
    DALI_RESP_FADE_TIME_RATE    = 8,
} DaliResponseKind;

typedef enum {
    DALI_CMD_DAPC = 0,

    DALI_CMD_OFF,
    DALI_CMD_UP,
    DALI_CMD_DOWN,
    DALI_CMD_STEP_UP,
    DALI_CMD_STEP_DOWN,
    DALI_CMD_RECALL_MAX_LEVEL,
    DALI_CMD_RECALL_MIN_LEVEL,
    DALI_CMD_STEP_DOWN_AND_OFF,
    DALI_CMD_ON_AND_STEP_UP,
    DALI_CMD_ENABLE_DAPC_SEQUENCE,
    DALI_CMD_GO_TO_LAST_ACTIVE_LEVEL,
    DALI_CMD_GO_TO_SCENE,

    DALI_CMD_RESET,
    DALI_CMD_STORE_ACTUAL_LEVEL_DTR0,
    DALI_CMD_SAVE_PERSISTENT_VARIABLES,
    DALI_CMD_SET_OPERATING_MODE_DTR0,
    DALI_CMD_RESET_MEMORY_BANK_DTR0,
    DALI_CMD_IDENTIFY_DEVICE,
    DALI_CMD_SET_MAX_LEVEL_DTR0,
    DALI_CMD_SET_MIN_LEVEL_DTR0,
    DALI_CMD_SET_SYSTEM_FAILURE_LEVEL_DTR0,
    DALI_CMD_SET_POWER_ON_LEVEL_DTR0,
    DALI_CMD_SET_FADE_TIME_DTR0,
    DALI_CMD_SET_FADE_RATE_DTR0,
    DALI_CMD_SET_EXTENDED_FADE_TIME_DTR0,
    DALI_CMD_SET_SCENE,
    DALI_CMD_REMOVE_FROM_SCENE,
    DALI_CMD_ADD_TO_GROUP,
    DALI_CMD_REMOVE_FROM_GROUP,
    DALI_CMD_SET_SHORT_ADDRESS_DTR0,
    DALI_CMD_ENABLE_WRITE_MEMORY,

    DALI_CMD_QUERY_STATUS,
    DALI_CMD_QUERY_CONTROL_GEAR_PRESENT,
    DALI_CMD_QUERY_LAMP_FAILURE,
    DALI_CMD_QUERY_LAMP_POWER_ON,
    DALI_CMD_QUERY_LIMIT_ERROR,
    DALI_CMD_QUERY_RESET_STATE,
    DALI_CMD_QUERY_MISSING_SHORT_ADDRESS,
    DALI_CMD_QUERY_VERSION_NUMBER,
    DALI_CMD_QUERY_CONTENT_DTR0,
    DALI_CMD_QUERY_DEVICE_TYPE,
    DALI_CMD_QUERY_PHYSICAL_MINIMUM,
    DALI_CMD_QUERY_POWER_FAILURE,
    DALI_CMD_QUERY_CONTENT_DTR1,
    DALI_CMD_QUERY_CONTENT_DTR2,
    DALI_CMD_QUERY_OPERATING_MODE,
    DALI_CMD_QUERY_LIGHT_SOURCE_TYPE,
    DALI_CMD_QUERY_ACTUAL_LEVEL,
    DALI_CMD_QUERY_MAX_LEVEL,
    DALI_CMD_QUERY_MIN_LEVEL,
    DALI_CMD_QUERY_POWER_ON_LEVEL,
    DALI_CMD_QUERY_SYSTEM_FAILURE_LEVEL,
    DALI_CMD_QUERY_FADE_TIME_FADE_RATE,
    DALI_CMD_QUERY_MANUFACTURER_SPECIFIC_MODE,
    DALI_CMD_QUERY_NEXT_DEVICE_TYPE,
    DALI_CMD_QUERY_EXTENDED_FADE_TIME,
    DALI_CMD_QUERY_CONTROL_GEAR_FAILURE,
    DALI_CMD_QUERY_SCENE_LEVEL,
    DALI_CMD_QUERY_GROUPS_0_7,
    DALI_CMD_QUERY_GROUPS_8_15,
    DALI_CMD_QUERY_RANDOM_ADDRESS_H,
    DALI_CMD_QUERY_RANDOM_ADDRESS_M,
    DALI_CMD_QUERY_RANDOM_ADDRESS_L,
    DALI_CMD_READ_MEMORY_LOCATION,
    DALI_CMD_QUERY_EXTENDED_VERSION_NUMBER,

    DALI_CMD_TERMINATE,
    DALI_CMD_DTR0_DATA,
    DALI_CMD_INITIALISE,
    DALI_CMD_RANDOMISE,
    DALI_CMD_COMPARE,
    DALI_CMD_WITHDRAW,
    DALI_CMD_PING,
    DALI_CMD_SEARCH_ADDRH,
    DALI_CMD_SEARCH_ADDRM,
    DALI_CMD_SEARCH_ADDRL,
    DALI_CMD_PROGRAM_SHORT_ADDRESS,
    DALI_CMD_VERIFY_SHORT_ADDRESS,
    DALI_CMD_QUERY_SHORT_ADDRESS,
    DALI_CMD_ENABLE_DEVICE_TYPE,
    DALI_CMD_DTR1_DATA,
    DALI_CMD_DTR2_DATA,
    DALI_CMD_WRITE_MEMORY_LOCATION,
    DALI_CMD_WRITE_MEMORY_LOCATION_NO_REPLY,

    DALI_CMD_QUERY_NUMBER_OF_INSTANCES,
    DALI_CMD_QUERY_INSTANCE_TYPE,
    DALI_CMD_QUERY_RESOLUTION,
    DALI_CMD_QUERY_INSTANCE_ERROR,
    DALI_CMD_QUERY_INSTANCE_STATUS,
    DALI_CMD_QUERY_INSTANCE_ENABLED,
    DALI_CMD_QUERY_INPUT_VALUE,
    DALI_CMD_QUERY_INPUT_VALUE_LATCH,

    /* Additional IEC 62386-103 generic instance queries. Appended to keep
     * the numeric values of the existing public command IDs stable. */
    DALI_CMD_QUERY_EVENT_PRIORITY,
    DALI_CMD_QUERY_PRIMARY_INSTANCE_GROUP,
    DALI_CMD_QUERY_INSTANCE_GROUP_1,
    DALI_CMD_QUERY_INSTANCE_GROUP_2,
    DALI_CMD_QUERY_EVENT_SCHEME,
    DALI_CMD_QUERY_EVENT_FILTER_0_7,
    DALI_CMD_QUERY_EVENT_FILTER_8_15,
    DALI_CMD_QUERY_EVENT_FILTER_16_23,
    DALI_CMD_QUERY_INSTANCE_CONFIGURATION,
    DALI_CMD_QUERY_AVAILABLE_INSTANCE_TYPES,

    /* Part 102 level commands added after the original table. Appended for the
     * same reason as the queries above: existing public IDs keep their values. */
    DALI_CMD_CONTINUOUS_UP,
    DALI_CMD_CONTINUOUS_DOWN,

    /* IEC 62386-103 control-device DTR readback. Distinct from the Part 102
     * gear commands DALI_CMD_QUERY_CONTENT_DTR0/1/2: these are 24-bit device
     * frames (address, 0xFE, opcode), so a control device and a control gear at
     * the same short address answer different frames. */
    DALI_CMD_QUERY_DEVICE_CONTENT_DTR0,
    DALI_CMD_QUERY_DEVICE_CONTENT_DTR1,
    DALI_CMD_QUERY_DEVICE_CONTENT_DTR2,

    /* IEC 62386-103 device-level quiescent mode. Both are send-twice and take
     * no DTR. Quiescent mode suppresses a control device's own bus activity —
     * event frames above all — so it is what makes a bus quiet enough for a
     * COMPARE reply window, a memory block read, or a scan. */
    DALI_CMD_START_QUIESCENT_MODE,
    DALI_CMD_STOP_QUIESCENT_MODE,

    /*
     * IEC 62386-103 TERMINATE, the Part 103 special-command counterpart of
     * DALI_CMD_TERMINATE. Closes a control device's own addressing state.
     *
     * It exists so that control-gear commissioning can close a window it opened
     * by accident: a control device that observes the Part 102 INITIALISE can
     * enter its own addressing state, generate a random address, and answer
     * Part 102 COMPARE as gear that is not there. Quiescent mode does not cover
     * that -- it stops a device transmitting on its own initiative, not
     * responding to a query it was addressed with.
     */
    DALI_CMD_DEVICE_TERMINATE,

    DALI_CMD_COUNT,
} DaliCommandId;

typedef struct {
    DaliCommandId        id;
    const char          *name;
    uint8_t              opcode_first;
    uint8_t              opcode_last;
    DaliCommandFrameKind frame_kind;
    DaliResponseKind     response_kind;
    bool                 send_twice;
    bool                 implemented;
} DaliCommandInfo;

const DaliCommandInfo *dali_command_lookup(DaliCommandId id);
const DaliCommandInfo *dali_command_lookup_opcode(DaliCommandFrameKind frame_kind,
                                                  uint8_t opcode);
uint8_t dali_command_count(void);

/*
 * Return true when retrying this command after a missing reply cannot advance
 * device-side read state. Stateful reads (for example memory and latch cursors)
 * must be retried as a complete higher-level sequence, never as one frame.
 */
bool dali_command_response_retry_safe(DaliCommandId id);

typedef enum {
    DALI_DTR0 = 0,
    DALI_DTR1 = 1,
    DALI_DTR2 = 2,
} DaliDtrRegister;

/*
 * Build a normal addressed 16-bit command from metadata.
 *
 * param is used for opcode ranges:
 *   DAPC level: 0-254
 *   scene/group range commands: 0-15
 *   fixed-opcode commands: must be 0
 */
DaliError dali_build_command(DaliAddressType type,
                             uint8_t address,
                             DaliCommandId id,
                             uint8_t param,
                             DaliFrame *out);

/* Build a short-addressed DALI-2 24-bit instance command from metadata. */
DaliError dali_build_instance_command(uint8_t addr,
                                      uint8_t instance,
                                      DaliCommandId id,
                                      DaliFrame *out);

/* Build a short-addressed DALI-2 24-bit device command from metadata.
 * Device-level commands use instance byte 0xFE. */
/*
 * Address every control device at once: address byte 0xFF, instance byte 0xFE.
 *
 * Separate from dali_build_device_command() rather than a sentinel address,
 * because 0xFF is an address *byte* and that function takes a short address.
 * Note what this reaches: all control devices, not control gear, and the two
 * address spaces are independent — a control device and a control gear may
 * share short address 5 and answer different frames.
 *
 * Every 24-bit device command is accepted, queries included, matching what
 * dali_build_command() allows for gear. A broadcast query collides by
 * construction, so the operator surfaces warn before sending one.
 */
DaliError dali_build_device_broadcast_command(DaliCommandId id, DaliFrame *out);

DaliError dali_build_device_command(uint8_t addr,
                                    DaliCommandId id,
                                    DaliFrame *out);

/*
 * Build an arc power frame carrying MASK (255).
 *
 * MASK is not a level: the gear leaves its arc power unchanged, which is how a
 * DAPC sequence is terminated without disturbing the current output. It is a
 * separate entry point precisely so no level arithmetic can reach 255 and
 * silently stop meaning "set this level".
 */
DaliError dali_build_dapc_mask(DaliAddressType type,
                               uint8_t address,
                               DaliFrame *out);

/* Build a special command frame. */
/*
 * Build an IEC 62386-103 special command: (0xC1 << 16) | (opcode << 8) | param.
 *
 * Separate from dali_build_special(), which builds the 16-bit Part 102 form.
 * The two spaces share names and even opcode numbers while meaning different
 * things, so they deliberately do not share a builder.
 */
DaliError dali_build_device_special(DaliCommandId id,
                                    uint8_t param,
                                    DaliFrame *out);

DaliError dali_build_special(DaliCommandId id,
                             uint8_t param,
                             DaliFrame *out);

/* Build a DTRx DATA special frame. */
DaliError dali_build_dtr_data(DaliDtrRegister reg,
                              uint8_t value,
                              DaliFrame *out);

/* ---------------------------------------------------------------------------
 * 16-bit frame builders
 * --------------------------------------------------------------------------*/

/* DAPC — Direct Arc Power Control.  addr: 0-63 short address. level: 0-254 */
DaliFrame dali_cmd_dapc(uint8_t addr, uint8_t level);

/* Group DAPC.  group: 0-15. level: 0-254 */
DaliFrame dali_cmd_group_dapc(uint8_t group, uint8_t level);

/* Broadcast DAPC to all devices. level: 0-254 */
DaliFrame dali_cmd_broadcast_dapc(uint8_t level);

/* Recall max level */
DaliFrame dali_cmd_recall_max(uint8_t addr);

/* Group recall max level */
DaliFrame dali_cmd_group_recall_max(uint8_t group);

/* Recall min level */
DaliFrame dali_cmd_recall_min(uint8_t addr);

/* Group recall min level */
DaliFrame dali_cmd_group_recall_min(uint8_t group);

/* Broadcast: recall min all devices */
DaliFrame dali_cmd_broadcast_recall_min(void);

/* Turn off */
DaliFrame dali_cmd_off(uint8_t addr);

/* Group turn off */
DaliFrame dali_cmd_group_off(uint8_t group);

/* Query status — expects 8-bit backward frame */
DaliFrame dali_cmd_query_status(uint8_t addr);

/* Group query status — may cause multiple backward frames if several devices reply. */
DaliFrame dali_cmd_group_query_status(uint8_t group);

/* Query actual level — expects 8-bit backward frame */
DaliFrame dali_cmd_query_actual_level(uint8_t addr);

/* Broadcast: turn off all devices */
DaliFrame dali_cmd_broadcast_off(void);

/* Broadcast: recall max all devices */
DaliFrame dali_cmd_broadcast_recall_max(void);

/* Special command frame convenience builders. */
DaliFrame dali_cmd_terminate(void);
DaliFrame dali_cmd_initialise(uint8_t param);
DaliFrame dali_cmd_randomise(void);
DaliFrame dali_cmd_compare(void);
DaliFrame dali_cmd_withdraw(void);
DaliFrame dali_cmd_ping(void);
DaliFrame dali_cmd_search_addr_h(uint8_t value);
DaliFrame dali_cmd_search_addr_m(uint8_t value);
DaliFrame dali_cmd_search_addr_l(uint8_t value);
DaliFrame dali_cmd_program_short_address(uint8_t value);
DaliFrame dali_cmd_verify_short_address(uint8_t value);
DaliFrame dali_cmd_query_short_address(void);
DaliFrame dali_cmd_enable_device_type(uint8_t device_type);
DaliFrame dali_cmd_dtr0_data(uint8_t value);
DaliFrame dali_cmd_dtr1_data(uint8_t value);
DaliFrame dali_cmd_dtr2_data(uint8_t value);
DaliFrame dali_cmd_write_memory_location(uint8_t value);
DaliFrame dali_cmd_write_memory_location_no_reply(uint8_t value);

DaliError dali_build_control_device_dtr_data(DaliDtrRegister reg,
                                             uint8_t value,
                                             DaliFrame *out);
DaliFrame dali_cmd_control_device_dtr0_data(uint8_t value);
DaliFrame dali_cmd_control_device_dtr1_data(uint8_t value);
DaliFrame dali_cmd_control_device_dtr2_data(uint8_t value);
DaliFrame dali_cmd_control_device_write_memory_location(uint8_t value);
DaliFrame dali_cmd_control_device_write_memory_location_no_reply(uint8_t value);

/* ---------------------------------------------------------------------------
 * DALI-2 24-bit instance command builders  (IEC 62386-103 §9.1.3)
 *
 * Frame layout (MSB first, 24 bits):
 *   Byte 2 (first tx): address byte — same encoding as 16-bit DALI
 *   Byte 1 (middle):   instance byte — 0x00..0x1F specific; 0xFE = device;
 *                      0xFF = all
 *   Byte 0 (last tx):  command byte
 *
 * Custom/vendor instance commands should be built by dedicated helper modules.
 * --------------------------------------------------------------------------*/

/* Instance command to a single device (short address 0–63).
 * instance: 0–31 for a specific instance; 0xFF for all instances on device. */
DaliFrame dali_cmd_instance(uint8_t addr, uint8_t instance, uint8_t cmd);

/* Device-level command to a single device (short address 0-63). */
DaliFrame dali_cmd_device(uint8_t addr, uint8_t cmd);
DaliFrame dali_cmd_device_broadcast(uint8_t cmd);

/* Instance command to a device group (0–15).
 * instance: 0–31 for a specific instance; 0xFF for all instances. */
DaliFrame dali_cmd_instance_group(uint8_t group, uint8_t instance, uint8_t cmd);

/* Instance command broadcast to all devices. */
DaliFrame dali_cmd_instance_broadcast(uint8_t instance, uint8_t cmd);

/* ---------------------------------------------------------------------------
 * Response parsing
 * --------------------------------------------------------------------------*/

/* Raw backward frame — returns value in value_out unchanged. */
DaliError dali_parse_response(uint8_t raw, uint8_t *value_out);

/* YES/NO response: 0xFF = YES, any other value = NO (IEC 62386-102 §8.3.3). */
bool dali_is_yes(uint8_t raw);

/* Status byte fields returned by QUERY STATUS (IEC 62386-202 Table 8).
 * Each field: true = the condition is present / flag is set. */
typedef struct {
    bool ballast_failure;       /* bit 0: ballast status not OK             */
    bool lamp_failure;          /* bit 1: lamp failure present              */
    bool lamp_arc_power_on;     /* bit 2: lamp arc power is on              */
    bool limit_error;           /* bit 3: output at limit                   */
    bool fade_running;          /* bit 4: fade in progress                  */
    bool reset_state;           /* bit 5: device is in reset state          */
    bool missing_short_address; /* bit 6: no short address programmed       */
    bool power_failure;         /* bit 7: power failure since last reset    */
} DaliStatus;

/* Parse a QUERY STATUS response byte into individual fields.
 * Returns DALI_ERR_INVALID if out is NULL. */
DaliError dali_parse_status(uint8_t raw, DaliStatus *out);

typedef struct {
    uint8_t fade_time;
    uint8_t fade_rate;
} DaliFadeTimeRate;

/* Split a packed QUERY FADE TIME / FADE RATE byte into high/low nibbles. */
DaliError dali_parse_fade_time_rate(uint8_t raw, DaliFadeTimeRate *out);

typedef struct {
    uint32_t value;          /* MSB-first combined value, up to 4 bytes */
    uint8_t  byte_count;
    uint8_t  expected_bytes;
    bool     complete;
} DaliInputValue;

/* Initialize an MSB-first DALI-2 input-value accumulator. expected: 1..4. */
DaliError dali_input_value_start(DaliInputValue *out, uint8_t expected_bytes);

/* Append one raw byte to an input-value accumulator. */
DaliError dali_input_value_push(DaliInputValue *value, uint8_t byte);

/* Append one 8-bit backward frame to an input-value accumulator. */
DaliError dali_input_value_push_frame(DaliInputValue *value, const DaliFrame *frame);

/* Convenience helper for the common 16-bit two-byte input-value case. */
DaliError dali_input_value_parse_16(uint8_t msb, uint8_t lsb, uint16_t *out);

typedef struct {
    DaliResponseKind kind;
    uint8_t          raw;
    uint8_t          value;
    uint8_t          bitset;
    bool             yes;
    DaliStatus       status;
    DaliFadeTimeRate fade;
} DaliParsedResponse;

/* Parse a backward frame according to an explicit response kind. */
DaliError dali_parse_by_kind(DaliResponseKind kind,
                             const DaliFrame *frame,
                             DaliParsedResponse *out);

/* Parse a backward frame according to command metadata. */
DaliError dali_parse_command_response(DaliCommandId id,
                                      const DaliFrame *frame,
                                      DaliParsedResponse *out);
