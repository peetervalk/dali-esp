#pragma once

/*
 * dali_gear_dt8.h — IEC 62386-209 (colour control gear, device type 8)
 *
 * DT8 supports three colour modes: XY chromaticity, colour temperature (Tc),
 * and RGBWAF channel control. All share the same opcode space (0xE0–0xFF) as
 * DT6 — every DT8 transaction must be preceded by ENABLE DEVICE TYPE 8.
 *
 * Caller responsibilities for multi-step commands:
 *   - XY / Tc set:   load DTR0 (low byte) + DTR1 (high byte) before the command.
 *                    Use dali_dt8_encode_16() to build both DTR frames at once.
 *   - RGB dim:       load DTR0=R, DTR1=G, DTR2=B before the command.
 *   - WAF dim:       load DTR0=W, DTR1=A, DTR2=F before the command.
 *   - QueryColourValue: load DTR0 with a DaliDt8ColourValueSelector before the
 *                    query. The 8-bit reply is the MSB of the 16-bit result;
 *                    query QUERY_CONTENT_DTR0 afterwards for the LSB.
 *   - Store* commands require send_twice = true.
 *   - Temporary Set* commands do not take effect until Activate is sent.
 *
 * No hardware dependencies. No scheduler dependency.
 */

#include "dali_protocol.h"
#include "dali_transport.h"

/* ---------------------------------------------------------------------------
 * Constants and enumerations
 * --------------------------------------------------------------------------*/

/* Bit positions in the QueryGearFeaturesStatus response (IEC 62386-209 §9.4.1.1) */
#define DALI_DT8_FEATURE_XY_CAPABLE        (1u << 0u)
#define DALI_DT8_FEATURE_TC_CAPABLE        (1u << 1u)
#define DALI_DT8_FEATURE_PRIMARY_N_CAPABLE (1u << 2u)
#define DALI_DT8_FEATURE_RGBWAF_CAPABLE    (1u << 3u)
#define DALI_DT8_FEATURE_AUTO_CALIBRATION  (1u << 4u)

/* Bit positions in the QueryColourStatus response (IEC 62386-209 §9.4.1.2) */
#define DALI_DT8_STATUS_XY_ACTIVE          (1u << 0u)
#define DALI_DT8_STATUS_TC_ACTIVE          (1u << 1u)
#define DALI_DT8_STATUS_PRIMARY_N_ACTIVE   (1u << 2u)
#define DALI_DT8_STATUS_RGBWAF_ACTIVE      (1u << 3u)
#define DALI_DT8_STATUS_TC_OUT_OF_RANGE    (1u << 4u)

/* DTR0 selector values for QueryColourValue (IEC 62386-209 §9.4.2.4) */
typedef enum {
    DALI_DT8_VALUE_X_COORDINATE   = 0u,
    DALI_DT8_VALUE_Y_COORDINATE   = 1u,
    DALI_DT8_VALUE_COLOUR_TEMP_TC = 2u,
    DALI_DT8_VALUE_PRIMARY_0      = 3u,
    DALI_DT8_VALUE_PRIMARY_1      = 4u,
    DALI_DT8_VALUE_PRIMARY_2      = 5u,
    DALI_DT8_VALUE_PRIMARY_3      = 6u,
    DALI_DT8_VALUE_PRIMARY_4      = 7u,
    DALI_DT8_VALUE_PRIMARY_5      = 8u,
    DALI_DT8_VALUE_RED            = 9u,
    DALI_DT8_VALUE_GREEN          = 10u,
    DALI_DT8_VALUE_BLUE           = 11u,
    DALI_DT8_VALUE_WHITE          = 12u,
    DALI_DT8_VALUE_AMBER          = 13u,
    DALI_DT8_VALUE_FREE_COLOUR    = 14u,
} DaliDt8ColourValueSelector;

/* ---------------------------------------------------------------------------
 * Response parsers
 * --------------------------------------------------------------------------*/

typedef struct {
    bool xy_capable;
    bool tc_capable;
    bool primary_n_capable;
    bool rgbwaf_capable;
    bool auto_calibration;
} DaliDt8GearFeatures;

typedef struct {
    bool xy_active;
    bool tc_active;
    bool primary_n_active;
    bool rgbwaf_active;
    bool tc_out_of_range;
} DaliDt8ColourStatus;

/* Parse QueryGearFeaturesStatus (0xF7) response byte. */
DaliError dali_dt8_parse_gear_features(uint8_t raw, DaliDt8GearFeatures *out);

/* Parse QueryColourStatus (0xF8) response byte. */
DaliError dali_dt8_parse_colour_status(uint8_t raw, DaliDt8ColourStatus *out);

/* ---------------------------------------------------------------------------
 * 16-bit DTR encoding helpers
 * --------------------------------------------------------------------------*/

/*
 * Encode a 16-bit colour value (XY coordinate or Tc) into a DTR0 + DTR1 frame
 * pair.  DTR0 carries the low byte; DTR1 carries the high byte.
 * Both output pointers must be non-NULL.
 */
DaliError dali_dt8_encode_16(uint16_t value,
                              DaliFrame *dtr0_out,
                              DaliFrame *dtr1_out);

/*
 * Build a DTR0 frame pre-loaded with a QueryColourValue selector.
 * Send this before ENABLE + QueryColourValue.
 */
DaliFrame dali_dt8_build_dtr0_selector(DaliDt8ColourValueSelector selector);

/* ---------------------------------------------------------------------------
 * Colour temperature conversion helpers
 * --------------------------------------------------------------------------*/

/* Convert Kelvin to Mirek (reciprocal megakelvin). Clamps to avoid div-by-zero. */
uint16_t dali_dt8_kelvin_to_mirek(uint16_t kelvin);

/* Convert Mirek to Kelvin. Clamps to avoid div-by-zero. */
uint16_t dali_dt8_mirek_to_kelvin(uint16_t mirek);

/* ---------------------------------------------------------------------------
 * Transport-dependent 16-bit colour value read
 *
 * QueryColourValue returns only the MSB of a 16-bit result. Recovering the
 * full value requires a 4-step sequence:
 *   1. DTR0 = selector           (no reply, broadcast special)
 *   2. ENABLE DEVICE TYPE 8      (no reply, broadcast special)
 *   3. QueryColourValue at addr  (reply = MSB, and the gear puts the LSB in DTR0)
 *   4. QUERY CONTENT DTR0        (reply = LSB of combined result)
 *
 * Every step depends on the one before it, and step 3 overwrites the very
 * register step 1 set up, so the group is built as a DaliSequence and run
 * through dali_transport_run_sequence_atomic(). A frame-only transport is
 * rejected before any frame is sent.
 *
 * The transport is the shared DaliTransport under its DT8-era names, so one
 * transport value serves this module and every other.
 * --------------------------------------------------------------------------*/

typedef DaliTransactionFn DaliDt8TransactionFn;
typedef DaliTransport     DaliDt8Transport;

#define DALI_DT8_COLOUR_VALUE_SEQUENCE_STEPS 4u
#define DALI_DT8_COLOUR_VALUE_STEP_DTR0      0u
#define DALI_DT8_COLOUR_VALUE_STEP_ENABLE    1u
#define DALI_DT8_COLOUR_VALUE_STEP_QUERY     2u
#define DALI_DT8_COLOUR_VALUE_STEP_DTR0_READ 3u

/* Retry budget for the DT8 reads that are safe to repeat on their own. */
#define DALI_DT8_QUERY_RETRIES_LEFT 1u

/*
 * Build the four-step colour value read for one address and selector.
 *
 * The QueryColourValue step carries no retry budget: it is answered under the
 * preceding ENABLE DEVICE TYPE 8, and it replaces the selector in DTR0 with the
 * result's low byte. A lone retransmission would therefore be read as a request
 * for whatever selector that low byte happens to name. Retry the sequence.
 */
DaliError dali_dt8_build_colour_value_sequence(uint8_t                    addr,
                                               DaliDt8ColourValueSelector selector,
                                               DaliSequence              *out);

/* Assemble the 16-bit value from a completed colour value sequence. */
DaliError dali_dt8_colour_value_from_sequence(const DaliSequenceResult *result,
                                              uint16_t                 *out);

/*
 * Read a 16-bit colour value (XY coordinate, Tc, or RGBWAF dim level) using
 * the 4-step QueryColourValue + QueryContentDTR0 sequence.
 *
 * selector — which value to read (DaliDt8ColourValueSelector)
 * out      — receives the 16-bit result (MSB from query reply, LSB from DTR0)
 */
DaliError dali_dt8_read_colour_value_16(const DaliDt8Transport    *transport,
                                        uint8_t                    addr,
                                        DaliDt8ColourValueSelector selector,
                                        uint16_t                  *out);

/* ---------------------------------------------------------------------------
 * Enable frame — must be sent before every DT8 command on the bus
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt8_enable(void);

/* ---------------------------------------------------------------------------
 * Temporary register command frames
 * These do NOT take effect until Activate is sent.
 * DTR loading is the caller's responsibility (see file header).
 * --------------------------------------------------------------------------*/

/* XY chromaticity mode */
DaliFrame dali_dt8_set_temporary_x_coordinate(uint8_t addr);   /* DTR0=low, DTR1=high */
DaliFrame dali_dt8_set_temporary_y_coordinate(uint8_t addr);   /* DTR0=low, DTR1=high */
DaliFrame dali_dt8_x_coordinate_step_up(uint8_t addr);
DaliFrame dali_dt8_x_coordinate_step_down(uint8_t addr);
DaliFrame dali_dt8_y_coordinate_step_up(uint8_t addr);
DaliFrame dali_dt8_y_coordinate_step_down(uint8_t addr);

/* Colour temperature mode */
DaliFrame dali_dt8_set_temporary_colour_temperature(uint8_t addr); /* DTR0=low, DTR1=high (Mirek) */
DaliFrame dali_dt8_colour_temperature_step_cooler(uint8_t addr);
DaliFrame dali_dt8_colour_temperature_step_warmer(uint8_t addr);

/* RGBWAF channel mode */
DaliFrame dali_dt8_set_temporary_primary_n_dim_level(uint8_t addr); /* DTR0=level, DTR1=primary, DTR2=reserved */
DaliFrame dali_dt8_set_temporary_rgb_dim_level(uint8_t addr);   /* DTR0=R, DTR1=G, DTR2=B */
DaliFrame dali_dt8_set_temporary_waf_dim_level(uint8_t addr);   /* DTR0=W, DTR1=A, DTR2=F */
DaliFrame dali_dt8_set_temporary_rgbwaf_control(uint8_t addr);  /* DTR0 = channel enable bitmap */

/* Activate — applies all pending temporary register values */
DaliFrame dali_dt8_activate(uint8_t addr);

/* Copy current colour report into the temporary registers */
DaliFrame dali_dt8_copy_report_to_temporary(uint8_t addr);

/* ---------------------------------------------------------------------------
 * Configuration command frames (send_twice = true)
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt8_store_ty_primary_n(uint8_t addr);          /* DTR0=Ty, DTR1=primary, DTR2=reserved */
DaliFrame dali_dt8_store_xy_coordinate_primary_n(uint8_t addr); /* DTR0=x_low, DTR1=x_high, DTR2=primary */
DaliFrame dali_dt8_store_colour_temperature_tc_limit(uint8_t addr); /* DTR0=low, DTR1=high, DTR2=selector */
DaliFrame dali_dt8_store_gear_features_status(uint8_t addr);  /* DTR0 = features bitmap */
DaliFrame dali_dt8_assign_colour_to_linked_channel(uint8_t addr); /* DTR0 = channel */
DaliFrame dali_dt8_start_auto_calibration(uint8_t addr);

/* ---------------------------------------------------------------------------
 * Query command frames (expect 8-bit backward frame)
 * --------------------------------------------------------------------------*/

DaliFrame dali_dt8_query_gear_features_status(uint8_t addr);
DaliFrame dali_dt8_query_colour_status(uint8_t addr);
DaliFrame dali_dt8_query_colour_type_features(uint8_t addr);
DaliFrame dali_dt8_query_colour_value(uint8_t addr);  /* pre-load DTR0 with selector */
DaliFrame dali_dt8_query_rgbwaf_control(uint8_t addr);
DaliFrame dali_dt8_query_assigned_colour(uint8_t addr); /* pre-load DTR0 with primary index */
DaliFrame dali_dt8_query_extended_version_number(uint8_t addr);
