#pragma once

/*
 * dali_dim_curve.h - DALI arc-power curves and configured level windows
 *
 * An arc power level is not a linear percentage of light output. The standard
 * defines levels 1..254 as a logarithmic scale spanning three decades:
 *
 *     output(X) = 10 ^ ((X - 1) / (253 / 3) - 3)
 *
 * so level 1 emits 0.1 % of maximum, level 254 emits 100 %, and level 85 emits
 * about 1 %. Reading the level as a linear fraction gets both directions wrong
 * by the same curve: a lamp sitting at 1 % light is reported as 33 % bright,
 * and a requested 1 % becomes level 3, which is 0.1 % — dark enough to look
 * like a failed command.
 *
 * A gear can instead select the linear curve, and can configure MIN LEVEL and
 * MAX LEVEL inside the legal 1..254 arc-power range. The typed helpers below
 * model both curves and normalize such a configured window to 0..1. OFF is
 * deliberately outside that normalized on-range.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dali_frame.h"

/* Light output of the dimmest and brightest levels, as a 0..1 fraction. */
#define DALI_DIM_CURVE_STANDARD_MIN_OUTPUT 0.001f
#define DALI_DIM_CURVE_LINEAR_MIN_OUTPUT   (1.0f / 254.0f)
#define DALI_DIM_CURVE_MIN_OUTPUT DALI_DIM_CURVE_STANDARD_MIN_OUTPUT
#define DALI_DIM_CURVE_MAX_OUTPUT 1.0f

typedef enum {
    DALI_DIM_CURVE_STANDARD = 0,
    DALI_DIM_CURVE_LINEAR = 1,
} DaliDimCurve;

typedef struct {
    DaliDimCurve curve;
    uint8_t      min_level;
    uint8_t      max_level;
} DaliLevelProfile;

bool dali_dim_curve_is_valid(DaliDimCurve curve);

/* Convert a legal on-level through the selected curve. */
DaliError dali_dim_curve_level_to_output_for(DaliDimCurve curve,
                                             uint8_t level,
                                             float *output);

/* Convert finite output to the nearest legal on-level, clamping at the ends. */
DaliError dali_dim_curve_output_to_level_for(DaliDimCurve curve,
                                             float output,
                                             uint8_t *level);

/* Validate 1 <= MIN <= MAX <= 254 and a known curve. */
DaliError dali_level_profile_validate(const DaliLevelProfile *profile);

/*
 * Widen `profile` so it also covers `addition`.
 *
 * One entity addressing several gear — a group or broadcast target — has no
 * single window when the members are commissioned differently. Gear clamps any
 * arc power level into its own MIN/MAX regardless of what the sender believed,
 * so a narrower window cannot make a mixed group uniform; it only makes levels
 * the hardware can reach unreachable. The union keeps every member's range
 * addressable and lets each one bottom out at its own floor, which is what the
 * fixtures physically do anyway.
 *
 * Members on different curves cannot both be driven correctly through one
 * mapping, so a disagreement falls back to the standard curve. Either profile
 * being invalid leaves `profile` unchanged.
 */
DaliError dali_level_profile_widen(DaliLevelProfile *profile,
                                   const DaliLevelProfile *addition);

/* Map normalized on-range 0..1 to the profile in physical-output space. */
DaliError dali_level_profile_relative_to_level(const DaliLevelProfile *profile,
                                               float relative,
                                               uint8_t *level);

/* Map a level inside the configured profile back to normalized on-range.
 * A fixed MIN == MAX profile has no recoverable slider position and reports
 * the canonical on value 1.0. */
DaliError dali_level_profile_level_to_relative(const DaliLevelProfile *profile,
                                               uint8_t level,
                                               float *relative);

/*
 * Relative light output (0.0 .. 1.0) for arc power level `level`.
 *
 * Levels 0 (OFF) and 255 (MASK) are not points on the curve; both return 0.0.
 */
float dali_dim_curve_level_to_output(uint8_t level);

/*
 * Arc power level (1..254) whose light output is nearest `output`.
 *
 * Outputs at or below the dimmest level clamp to 1 and outputs at or above
 * maximum clamp to 254, so the result is always a legal DAPC level: "off" is a
 * separate command, not level 0. NaN clamps to 1 rather than propagating.
 *
 * This is the exact inverse of dali_dim_curve_level_to_output() for every level
 * that function can produce, which is what lets an observed level survive a
 * round trip through a consumer that stores light output instead of levels.
 */
uint8_t dali_dim_curve_output_to_level(float output);
