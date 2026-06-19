#pragma once

/*
 * dali_gear_dt6.h — IEC 62386-207 (LED control gear, device type 6)
 *
 * DT6 commands use opcodes 0xE0–0xFF on addressed 16-bit frames, but every
 * DT6 transaction on the bus must be preceded by ENABLE DEVICE TYPE 6.
 * This module provides pure frame builders; callers sequence the pair
 * [dali_dt6_enable(), <command frame>] as a 2-step DaliSequence or via
 * two consecutive transport calls (enable no-reply, then command with reply).
 *
 * Configuration commands must also be sent twice (send_twice = true).
 * DTR0 must be preloaded by the caller before SelectDimmingCurve and
 * StoreDTRAsFastFadeTime.
 *
 * No hardware dependencies. No scheduler dependency.
 */

#include "dali_protocol.h"

/* ---------------------------------------------------------------------------
 * Response types
 * --------------------------------------------------------------------------*/

/* QUERY FAILURE STATUS response bitmap (IEC 62386-207 §9.2.4.14) */
typedef struct {
    bool led_short_circuit;             /* bit 0 */
    bool led_open_circuit;              /* bit 1 */
    bool load_decrease;                 /* bit 2 */
    bool load_increase;                 /* bit 3 */
    bool current_protector_active;      /* bit 4 */
    bool thermal_shutdown;              /* bit 5 */
    bool thermal_overload;              /* bit 6 */
    bool reference_measurement_failed;  /* bit 7 */
} DaliDt6FailureStatus;

/* DTR0 values for SELECT DIMMING CURVE */
#define DALI_DT6_CURVE_STANDARD 0u   /* logarithmic, IEC 62386-207 default */
#define DALI_DT6_CURVE_LINEAR   1u

/* ---------------------------------------------------------------------------
 * Enable frame — must be sent before every DT6 command on the bus
 * --------------------------------------------------------------------------*/

/* Build ENABLE DEVICE TYPE 6 (special broadcast frame, no reply, no send-twice). */
DaliFrame dali_dt6_enable(void);

/* ---------------------------------------------------------------------------
 * Configuration command frames
 * All must be sent twice (send_twice = true in DaliSequenceStep/DaliTransaction).
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt6_reference_system_power(uint8_t addr);
DaliFrame dali_dt6_enable_current_protector(uint8_t addr);
DaliFrame dali_dt6_disable_current_protector(uint8_t addr);
DaliFrame dali_dt6_select_dimming_curve(uint8_t addr);       /* DTR0 = DALI_DT6_CURVE_* */
DaliFrame dali_dt6_store_dtr_as_fast_fade_time(uint8_t addr); /* DTR0 = time value      */

/* ---------------------------------------------------------------------------
 * Query command frames — expect 8-bit backward frame
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt6_query_gear_type(uint8_t addr);
DaliFrame dali_dt6_query_dimming_curve(uint8_t addr);
DaliFrame dali_dt6_query_possible_operating_modes(uint8_t addr);
DaliFrame dali_dt6_query_features(uint8_t addr);
DaliFrame dali_dt6_query_failure_status(uint8_t addr);
DaliFrame dali_dt6_query_short_circuit(uint8_t addr);
DaliFrame dali_dt6_query_open_circuit(uint8_t addr);
DaliFrame dali_dt6_query_load_decrease(uint8_t addr);
DaliFrame dali_dt6_query_load_increase(uint8_t addr);
DaliFrame dali_dt6_query_current_protector_active(uint8_t addr);
DaliFrame dali_dt6_query_thermal_shutdown(uint8_t addr);
DaliFrame dali_dt6_query_thermal_overload(uint8_t addr);
DaliFrame dali_dt6_query_reference_running(uint8_t addr);
DaliFrame dali_dt6_query_reference_measurement_failed(uint8_t addr);
DaliFrame dali_dt6_query_current_protector_enabled(uint8_t addr);
DaliFrame dali_dt6_query_operating_mode(uint8_t addr);
DaliFrame dali_dt6_query_fast_fade_time(uint8_t addr);
DaliFrame dali_dt6_query_min_fast_fade_time(uint8_t addr);
DaliFrame dali_dt6_query_extended_version_number(uint8_t addr);

/* ---------------------------------------------------------------------------
 * Response parsers
 * --------------------------------------------------------------------------*/

/* Parse a QUERY FAILURE STATUS response byte into individual fault flags. */
DaliError dali_dt6_parse_failure_status(uint8_t raw, DaliDt6FailureStatus *out);
