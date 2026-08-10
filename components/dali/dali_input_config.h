#pragma once

/*
 * IEC 62386 input-device instance command frame builders.
 *
 * Common commands are from IEC 62386-103:2022. Type-specific commands are
 * from Parts 301, 303, and 304.
 *
 * For each configuration command, load every named control-device DTR once,
 * then send the returned frame twice within 100 ms with no intervening
 * command. ENABLE INSTANCE and DISABLE INSTANCE are also sent twice but use no
 * DTR. Instance control commands marked "send once" use one transmission.
 * Queries use one transmission and expect an 8-bit backward frame.
 * These Part 103/3xx commands do not use ENABLE DEVICE TYPE.
 */

#include "dali_protocol.h"

/* Common instance configuration (IEC 62386-103:2022). */

/* SET EVENT PRIORITY: DTR0 = priority [2, 5]. Send twice. */
DaliFrame dali_input_build_set_event_priority(uint8_t addr, uint8_t instance);

/* ENABLE INSTANCE: no DTR. Send twice. */
DaliFrame dali_input_build_enable_instance(uint8_t addr, uint8_t instance);

/* DISABLE INSTANCE: no DTR. Send twice. */
DaliFrame dali_input_build_disable_instance(uint8_t addr, uint8_t instance);

/* SET PRIMARY INSTANCE GROUP: DTR0 = group [0, 31] or MASK (0xFF). Send twice. */
DaliFrame dali_input_build_set_primary_group(uint8_t addr, uint8_t instance);

/* SET INSTANCE GROUP 1: DTR0 = group [0, 31] or MASK (0xFF). Send twice. */
DaliFrame dali_input_build_set_instance_group1(uint8_t addr, uint8_t instance);

/* SET INSTANCE GROUP 2: DTR0 = group [0, 31] or MASK (0xFF). Send twice. */
DaliFrame dali_input_build_set_instance_group2(uint8_t addr, uint8_t instance);

/* SET EVENT SCHEME: DTR0 = event scheme [0, 4]. Send twice. */
DaliFrame dali_input_build_set_event_scheme(uint8_t addr, uint8_t instance);

/* SET EVENT FILTER: DTR2:DTR1:DTR0 = eventFilter[23:0]. Send twice. */
DaliFrame dali_input_build_set_event_filter(uint8_t addr, uint8_t instance);

/*
 * SET INSTANCE TYPE: DTR0[7:5] = 0 and DTR0[4:0] = a supported instance type.
 * Send twice. Changing the type can reset instance variables and restart the
 * input device.
 */
DaliFrame dali_input_build_set_instance_type(uint8_t addr, uint8_t instance);

/*
 * SET INSTANCE CONFIGURATION: DTR0 selects instanceConfiguration[], and
 * DTR2:DTR1 supplies its 16-bit value. DTR0 = 191 with DTR2:DTR1 = 0x55CC
 * restores all instance configurations to factory defaults. Send twice.
 */
DaliFrame dali_input_build_set_instance_configuration(uint8_t addr, uint8_t instance);

/* Part 301 push button / binary input (instance type 1). */

/* SET SHORT TIMER: DTR0 = tShort [tShortMin, 255], in 20 ms units. Send twice. */
DaliFrame dali_input_pb_build_set_short_timer(uint8_t addr, uint8_t instance);

/* SET DOUBLE TIMER: DTR0 = 0 or [tDoubleMin, 100], in 20 ms units. Send twice. */
DaliFrame dali_input_pb_build_set_double_timer(uint8_t addr, uint8_t instance);

/* SET REPEAT TIMER: DTR0 = tRepeat [5, 100], in 20 ms units. Send twice. */
DaliFrame dali_input_pb_build_set_repeat_timer(uint8_t addr, uint8_t instance);

/* SET STUCK TIMER: DTR0 = tStuck [5, 255], in 1 s units. Send twice. */
DaliFrame dali_input_pb_build_set_stuck_timer(uint8_t addr, uint8_t instance);

/* Part 301 timer queries: send once; expect an 8-bit reply. */
DaliFrame dali_input_pb_build_query_short_timer(uint8_t addr, uint8_t instance);
DaliFrame dali_input_pb_build_query_short_timer_min(uint8_t addr, uint8_t instance);
DaliFrame dali_input_pb_build_query_double_timer(uint8_t addr, uint8_t instance);
DaliFrame dali_input_pb_build_query_double_timer_min(uint8_t addr, uint8_t instance);
DaliFrame dali_input_pb_build_query_repeat_timer(uint8_t addr, uint8_t instance);
DaliFrame dali_input_pb_build_query_stuck_timer(uint8_t addr, uint8_t instance);

/* Part 303 occupancy sensor (instance type 3), including Amendment 1:2024. */

/* CATCH MOVEMENT: no DTR; send once. */
DaliFrame dali_input_occ_build_catch_movement(uint8_t addr, uint8_t instance);

/* SET HOLD TIMER: DTR0 [0, 254], in 10 s units; 0 selects 1 s. Send twice. */
DaliFrame dali_input_occ_build_set_hold_timer(uint8_t addr, uint8_t instance);

/* SET REPORT TIMER: DTR0 [0, 255], in 1 s units; 0 disables it. Send twice. */
DaliFrame dali_input_occ_build_set_report_timer(uint8_t addr, uint8_t instance);

/* SET DEADTIME TIMER: DTR0 [0, 255], 50 ms units; 0 disables it. Send twice. */
DaliFrame dali_input_occ_build_set_deadtime(uint8_t addr, uint8_t instance);

/* CANCEL HOLD TIMER: no DTR; send once. */
DaliFrame dali_input_occ_build_cancel_hold_timer(uint8_t addr, uint8_t instance);

/* SET DETECTION RANGE: DTR0 [0, 100], if supported. Send twice. */
DaliFrame dali_input_occ_build_set_detection_range(uint8_t addr, uint8_t instance);

/* SET SENSITIVITY: DTR0 [0, 100], if supported. Send twice. */
DaliFrame dali_input_occ_build_set_sensitivity(uint8_t addr, uint8_t instance);

/* Part 303 queries: send once; expect an 8-bit reply. */
DaliFrame dali_input_occ_build_query_capabilities(uint8_t addr, uint8_t instance);
DaliFrame dali_input_occ_build_query_detection_range(uint8_t addr, uint8_t instance);
DaliFrame dali_input_occ_build_query_sensitivity(uint8_t addr, uint8_t instance);
DaliFrame dali_input_occ_build_query_deadtime(uint8_t addr, uint8_t instance);
DaliFrame dali_input_occ_build_query_hold_timer(uint8_t addr, uint8_t instance);
DaliFrame dali_input_occ_build_query_report_timer(uint8_t addr, uint8_t instance);
DaliFrame dali_input_occ_build_query_catching(uint8_t addr, uint8_t instance);

/* Part 304 light sensor (instance type 4). */

/* SET REPORT TIMER: DTR0 [0, 255], in 1 s units; 0 disables it. Send twice. */
DaliFrame dali_input_light_build_set_report_timer(uint8_t addr, uint8_t instance);

/* SET HYSTERESIS: DTR0 = percentage [0, 25]. Send twice. */
DaliFrame dali_input_light_build_set_hysteresis(uint8_t addr, uint8_t instance);

/* SET DEADTIME TIMER: DTR0 [0, 255], 50 ms units; 0 disables it. Send twice. */
DaliFrame dali_input_light_build_set_deadtime(uint8_t addr, uint8_t instance);

/* SET HYSTERESIS MIN: DTR0 = absolute minimum [0, 255]. Send twice. */
DaliFrame dali_input_light_build_set_hysteresis_min(uint8_t addr, uint8_t instance);

/* Part 304 queries: send once; expect an 8-bit reply. */
DaliFrame dali_input_light_build_query_hysteresis_min(uint8_t addr, uint8_t instance);
DaliFrame dali_input_light_build_query_deadtime(uint8_t addr, uint8_t instance);
DaliFrame dali_input_light_build_query_report_timer(uint8_t addr, uint8_t instance);
DaliFrame dali_input_light_build_query_hysteresis(uint8_t addr, uint8_t instance);
