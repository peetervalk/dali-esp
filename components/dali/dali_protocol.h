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
 * Response parsing
 * --------------------------------------------------------------------------*/
DaliError dali_parse_response(uint8_t raw, uint8_t *value_out);
