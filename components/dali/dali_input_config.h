#pragma once

/*
 * dali_input_config.h — IEC 62386-103 instance configuration frame builders
 *                        and type-specific configuration for DT301/303/304.
 *
 * All configuration commands require send_twice = true in the scheduler.
 * DTR loading (before sending the command) is the caller's responsibility.
 *
 * Opcodes are in the 24-bit instance command byte:
 *   Generic IEC 62386-103 config: 0x60–0x7F
 *   Type-specific (DT301/303/304): 0xE0–0xFF
 *
 * Opcode source: IEC 62386-103:2014 Table 15, cross-referenced with
 * python-dali (https://github.com/sde1000/python-dali).
 * Verify against hardware before shipping.
 */

#include "dali_protocol.h"

/* ---------------------------------------------------------------------------
 * Generic IEC 62386-103 instance configuration
 * --------------------------------------------------------------------------*/

/* Set Event Priority (DTR0 = priority value 1–5).  send_twice. */
DaliFrame dali_input_build_set_event_priority(uint8_t addr, uint8_t instance);

/* Enable Instance. send_twice. */
DaliFrame dali_input_build_enable_instance(uint8_t addr, uint8_t instance);

/* Disable Instance. send_twice. */
DaliFrame dali_input_build_disable_instance(uint8_t addr, uint8_t instance);

/* Set Primary Instance Group (DTR0 = group 0–15 or 0xFF to clear). send_twice. */
DaliFrame dali_input_build_set_primary_group(uint8_t addr, uint8_t instance);

/* Set Instance Group 1 (DTR0 = group or 0xFF). send_twice. */
DaliFrame dali_input_build_set_instance_group1(uint8_t addr, uint8_t instance);

/* Set Instance Group 2 (DTR0 = group or 0xFF). send_twice. */
DaliFrame dali_input_build_set_instance_group2(uint8_t addr, uint8_t instance);

/* Set Event Scheme (DTR0 = scheme, e.g. 0=instance, 1=device, 2=device+instance). send_twice. */
DaliFrame dali_input_build_set_event_scheme(uint8_t addr, uint8_t instance);

/* Set Event Filter (DTR0/1/2 = 3 filter bytes). send_twice. */
DaliFrame dali_input_build_set_event_filter(uint8_t addr, uint8_t instance);

/* Set Report Timer (DTR0 = timer value). send_twice. */
DaliFrame dali_input_build_set_report_timer(uint8_t addr, uint8_t instance);

/* Set Input Hysteresis (DTR0 = hysteresis value). send_twice. */
DaliFrame dali_input_build_set_hysteresis(uint8_t addr, uint8_t instance);

/* Set Deadtime Timer (DTR0 = deadtime value). send_twice. */
DaliFrame dali_input_build_set_deadtime_timer(uint8_t addr, uint8_t instance);

/* ---------------------------------------------------------------------------
 * DT301 push-button type-specific configuration (IEC 62386-301)
 * --------------------------------------------------------------------------*/

/* Set push-button hysteresis / bounce filter (DTR0 = value). send_twice. */
DaliFrame dali_input_pb_build_set_hysteresis(uint8_t addr, uint8_t instance);

/* Set push-button dead-time between events (DTR0 = value). send_twice. */
DaliFrame dali_input_pb_build_set_deadtime(uint8_t addr, uint8_t instance);

/* Set double-click detection priority (DTR0 = priority). send_twice. */
DaliFrame dali_input_pb_build_set_double_click_priority(uint8_t addr, uint8_t instance);

/* Set repeat period for held-button auto-repeat (DTR0 = value). send_twice. */
DaliFrame dali_input_pb_build_set_repeat_period(uint8_t addr, uint8_t instance);

/* Set hold timer (DTR0 = value). send_twice. */
DaliFrame dali_input_pb_build_set_hold_timer(uint8_t addr, uint8_t instance);

/* ---------------------------------------------------------------------------
 * DT303 occupancy sensor type-specific configuration (IEC 62386-303)
 * --------------------------------------------------------------------------*/

/* Set occupancy dead-time before re-trigger (DTR0 = value). send_twice. */
DaliFrame dali_input_occ_build_set_deadtime(uint8_t addr, uint8_t instance);

/* Set hold timer for "occupied" state (DTR0 = value). send_twice. */
DaliFrame dali_input_occ_build_set_hold_timer(uint8_t addr, uint8_t instance);

/* ---------------------------------------------------------------------------
 * DT304 light sensor type-specific configuration (IEC 62386-304)
 * --------------------------------------------------------------------------*/

/* Set light sensor hysteresis (DTR0 = value). send_twice. */
DaliFrame dali_input_light_build_set_hysteresis(uint8_t addr, uint8_t instance);

/* Set light sensor deadband (DTR0 = value). send_twice. */
DaliFrame dali_input_light_build_set_deadband(uint8_t addr, uint8_t instance);

/* ---------------------------------------------------------------------------
 * Generic IEC 62386-103 instance query frame builders
 * Single transmission, expects 8-bit reply. No DTR0, no send_twice.
 * --------------------------------------------------------------------------*/

DaliFrame dali_input_build_query_instance_type(uint8_t addr, uint8_t instance);
DaliFrame dali_input_build_query_resolution(uint8_t addr, uint8_t instance);
DaliFrame dali_input_build_query_hysteresis(uint8_t addr, uint8_t instance);
DaliFrame dali_input_build_query_deadtime_timer(uint8_t addr, uint8_t instance);
DaliFrame dali_input_build_query_report_timer(uint8_t addr, uint8_t instance);

/* DT303 occupancy sensor type-specific queries (IEC 62386-303) */
DaliFrame dali_input_occ_build_query_deadtime(uint8_t addr, uint8_t instance);
DaliFrame dali_input_occ_build_query_hold_timer(uint8_t addr, uint8_t instance);

/* DT304 light sensor type-specific queries (IEC 62386-304) */
DaliFrame dali_input_light_build_query_hysteresis(uint8_t addr, uint8_t instance);
DaliFrame dali_input_light_build_query_deadband(uint8_t addr, uint8_t instance);

/* Generic IEC 62386-103 instance state queries (protocol-table backed) */
DaliFrame dali_input_build_query_instance_enabled(uint8_t addr, uint8_t instance);
DaliFrame dali_input_build_query_instance_status(uint8_t addr, uint8_t instance);
