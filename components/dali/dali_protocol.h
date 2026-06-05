#pragma once

/*
 * dali_protocol.h — DALI frame construction and response parsing
 *
 * Responsibilities:
 *   - Stateless frame builders for standard 16-bit DALI commands
 *   - Stateless frame builders for DALI-2 24-bit extended commands
 *   - Response parsing (8-bit backward frames)
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
    DALI_CMD_RANDOMIZE,
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

    DALI_CMD_QUERY_VALUE_MULTIPLICATOR,
    DALI_CMD_QUERY_VALUE_DIVISOR,
    DALI_CMD_QUERY_OFFSET_MSB,
    DALI_CMD_QUERY_OFFSET_LSB,
    DALI_CMD_QUERY_OFFSET_MULTIPLICATOR,
    DALI_CMD_QUERY_OFFSET_DIVISOR,
    DALI_CMD_QUERY_UNIT,
    DALI_CMD_QUERY_RESOLUTION,
    DALI_CMD_QUERY_INPUT_VALUE,
    DALI_CMD_QUERY_INPUT_VALUE_LATCH,
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

/* ---------------------------------------------------------------------------
 * DALI-2 24-bit instance command builders  (IEC 62386-103 §9.1.3)
 *
 * Frame layout (MSB first, 24 bits):
 *   Byte 2 (first tx): address byte — same encoding as 16-bit DALI
 *   Byte 1 (middle):   instance byte — 0x00..0x1F specific; 0xFF = all
 *   Byte 0 (last tx):  command byte
 *
 * Command codes should be verified against IEC 62386-103/3xx before use.
 * --------------------------------------------------------------------------*/

/* Instance command to a single device (short address 0–63).
 * instance: 0–31 for a specific instance; 0xFF for all instances on device. */
DaliFrame dali_cmd_instance(uint8_t addr, uint8_t instance, uint8_t cmd);

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
    DaliResponseKind kind;
    uint8_t          raw;
    uint8_t          value;
    uint8_t          bitset;
    bool             yes;
    DaliStatus       status;
} DaliParsedResponse;

/* Parse a backward frame according to an explicit response kind. */
DaliError dali_parse_by_kind(DaliResponseKind kind,
                             const DaliFrame *frame,
                             DaliParsedResponse *out);

/* Parse a backward frame according to command metadata. */
DaliError dali_parse_command_response(DaliCommandId id,
                                      const DaliFrame *frame,
                                      DaliParsedResponse *out);
