#include "dali_dim_curve.h"

#include <math.h>

/* Levels 1..254 are 253 steps spread evenly over three decades of output. */
#define DALI_DIM_CURVE_DECADES          3.0f
#define DALI_DIM_CURVE_STEPS_PER_DECADE (253.0f / DALI_DIM_CURVE_DECADES)

float dali_dim_curve_level_to_output(uint8_t level)
{
    /* 0 is OFF and 255 is MASK; neither names a point on the curve. */
    if (level == 0u || level == 255u) {
        return 0.0f;
    }

    return powf(10.0f,
                ((float)(level - 1) / DALI_DIM_CURVE_STEPS_PER_DECADE)
                    - DALI_DIM_CURVE_DECADES);
}

uint8_t dali_dim_curve_output_to_level(float output)
{
    /* Negated so that NaN, which compares false against everything, lands here
     * on the dimmest level rather than falling through to logf(). */
    if (!(output > DALI_DIM_CURVE_MIN_OUTPUT)) {
        return 1u;
    }
    if (output >= DALI_DIM_CURVE_MAX_OUTPUT) {
        return 254u;
    }

    float level = 1.0f + (log10f(output) + DALI_DIM_CURVE_DECADES)
                             * DALI_DIM_CURVE_STEPS_PER_DECADE;

    long rounded = lroundf(level);
    if (rounded < 1) {
        return 1u;
    }
    if (rounded > 254) {
        return 254u;
    }
    return (uint8_t)rounded;
}
