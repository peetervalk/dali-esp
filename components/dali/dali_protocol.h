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
 * 16-bit frame builders
 * --------------------------------------------------------------------------*/

/* DAPC — Direct Arc Power Control.  addr: 0–63 short address. level: 0–254 */
DaliFrame dali_cmd_dapc(uint8_t addr, uint8_t level);

/* Recall max level */
DaliFrame dali_cmd_recall_max(uint8_t addr);

/* Recall min level */
DaliFrame dali_cmd_recall_min(uint8_t addr);

/* Turn off */
DaliFrame dali_cmd_off(uint8_t addr);

/* Query status — expects 8-bit backward frame */
DaliFrame dali_cmd_query_status(uint8_t addr);

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
