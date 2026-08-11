#pragma once

/*
 * dali_dim_curve.h - IEC 62386-102 logarithmic dimming curve
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
 * These helpers convert between the two, so an integration can speak in light
 * output while the bus carries arc power levels.
 */

#include <stdint.h>

/* Light output of the dimmest and brightest levels, as a 0..1 fraction. */
#define DALI_DIM_CURVE_MIN_OUTPUT 0.001f
#define DALI_DIM_CURVE_MAX_OUTPUT 1.0f

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
