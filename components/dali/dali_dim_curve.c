#include "dali_dim_curve.h"

#include <math.h>
#include <stddef.h>

/* Levels 1..254 are 253 steps spread evenly over three decades of output. */
#define DALI_DIM_CURVE_DECADES          3.0f
#define DALI_DIM_CURVE_STEPS_PER_DECADE (253.0f / DALI_DIM_CURVE_DECADES)

bool dali_dim_curve_is_valid(DaliDimCurve curve)
{
    return curve == DALI_DIM_CURVE_STANDARD || curve == DALI_DIM_CURVE_LINEAR;
}

DaliError dali_dim_curve_level_to_output_for(DaliDimCurve curve,
                                             uint8_t level,
                                             float *output)
{
    if (output == NULL || !dali_dim_curve_is_valid(curve) ||
        level == 0u || level == DALI_DAPC_MASK_LEVEL) {
        return DALI_ERR_INVALID;
    }

    if (curve == DALI_DIM_CURVE_LINEAR) {
        *output = (float)level / (float)DALI_DAPC_MAX_LEVEL;
    } else {
        *output = powf(10.0f,
                       ((float)(level - 1u) / DALI_DIM_CURVE_STEPS_PER_DECADE) -
                           DALI_DIM_CURVE_DECADES);
    }
    return DALI_OK;
}

DaliError dali_dim_curve_output_to_level_for(DaliDimCurve curve,
                                             float output,
                                             uint8_t *level)
{
    if (level == NULL || !dali_dim_curve_is_valid(curve) || !isfinite(output)) {
        return DALI_ERR_INVALID;
    }

    float min_output = curve == DALI_DIM_CURVE_LINEAR
                           ? DALI_DIM_CURVE_LINEAR_MIN_OUTPUT
                           : DALI_DIM_CURVE_STANDARD_MIN_OUTPUT;
    if (output <= min_output) {
        *level = 1u;
        return DALI_OK;
    }
    if (output >= DALI_DIM_CURVE_MAX_OUTPUT) {
        *level = DALI_DAPC_MAX_LEVEL;
        return DALI_OK;
    }

    float unrounded = curve == DALI_DIM_CURVE_LINEAR
                          ? output * (float)DALI_DAPC_MAX_LEVEL
                          : 1.0f + (log10f(output) + DALI_DIM_CURVE_DECADES) *
                                       DALI_DIM_CURVE_STEPS_PER_DECADE;
    long rounded = lroundf(unrounded);
    if (rounded < 1L) rounded = 1L;
    if (rounded > (long)DALI_DAPC_MAX_LEVEL) {
        rounded = (long)DALI_DAPC_MAX_LEVEL;
    }
    *level = (uint8_t)rounded;
    return DALI_OK;
}

DaliError dali_level_profile_validate(const DaliLevelProfile *profile)
{
    if (profile == NULL || !dali_dim_curve_is_valid(profile->curve) ||
        profile->min_level == 0u ||
        profile->max_level == DALI_DAPC_MASK_LEVEL ||
        profile->min_level > profile->max_level) {
        return DALI_ERR_INVALID;
    }
    return DALI_OK;
}

DaliError dali_level_profile_widen(DaliLevelProfile *profile,
                                   const DaliLevelProfile *addition)
{
    if (dali_level_profile_validate(profile) != DALI_OK ||
        dali_level_profile_validate(addition) != DALI_OK) {
        return DALI_ERR_INVALID;
    }

    if (addition->min_level < profile->min_level) {
        profile->min_level = addition->min_level;
    }
    if (addition->max_level > profile->max_level) {
        profile->max_level = addition->max_level;
    }
    if (addition->curve != profile->curve) {
        profile->curve = DALI_DIM_CURVE_STANDARD;
    }
    return DALI_OK;
}

DaliError dali_level_profile_relative_to_level(const DaliLevelProfile *profile,
                                               float relative,
                                               uint8_t *level)
{
    if (level == NULL || dali_level_profile_validate(profile) != DALI_OK ||
        !isfinite(relative) || relative < 0.0f || relative > 1.0f) {
        return DALI_ERR_INVALID;
    }
    if (profile->min_level == profile->max_level) {
        *level = profile->min_level;
        return DALI_OK;
    }

    float min_output;
    float max_output;
    (void)dali_dim_curve_level_to_output_for(profile->curve,
                                             profile->min_level,
                                             &min_output);
    (void)dali_dim_curve_level_to_output_for(profile->curve,
                                             profile->max_level,
                                             &max_output);
    float output = min_output + relative * (max_output - min_output);

    uint8_t mapped_level;
    DaliError err = dali_dim_curve_output_to_level_for(profile->curve,
                                                       output,
                                                       &mapped_level);
    if (err != DALI_OK) return err;
    if (mapped_level < profile->min_level) {
        mapped_level = profile->min_level;
    } else if (mapped_level > profile->max_level) {
        mapped_level = profile->max_level;
    }
    *level = mapped_level;
    return DALI_OK;
}

DaliError dali_level_profile_level_to_relative(const DaliLevelProfile *profile,
                                               uint8_t level,
                                               float *relative)
{
    if (relative == NULL || dali_level_profile_validate(profile) != DALI_OK ||
        level < profile->min_level || level > profile->max_level) {
        return DALI_ERR_INVALID;
    }
    if (profile->min_level == profile->max_level) {
        *relative = 1.0f;
        return DALI_OK;
    }

    float min_output;
    float max_output;
    float level_output;
    (void)dali_dim_curve_level_to_output_for(profile->curve,
                                             profile->min_level,
                                             &min_output);
    (void)dali_dim_curve_level_to_output_for(profile->curve,
                                             profile->max_level,
                                             &max_output);
    (void)dali_dim_curve_level_to_output_for(profile->curve,
                                             level,
                                             &level_output);
    float mapped = (level_output - min_output) / (max_output - min_output);
    if (mapped < 0.0f) {
        mapped = 0.0f;
    } else if (mapped > 1.0f) {
        mapped = 1.0f;
    }
    *relative = mapped;
    return DALI_OK;
}

float dali_dim_curve_level_to_output(uint8_t level)
{
    float output;
    if (dali_dim_curve_level_to_output_for(DALI_DIM_CURVE_STANDARD,
                                           level,
                                           &output) != DALI_OK) {
        return 0.0f;
    }
    return output;
}

uint8_t dali_dim_curve_output_to_level(float output)
{
    /* Preserve the legacy wrapper's clamping, including NaN/-Inf to level 1
     * and +Inf to maximum. Typed callers receive DALI_ERR_INVALID instead. */
    if (!(output > DALI_DIM_CURVE_MIN_OUTPUT)) return 1u;
    if (output >= DALI_DIM_CURVE_MAX_OUTPUT) return DALI_DAPC_MAX_LEVEL;

    uint8_t level;
    if (dali_dim_curve_output_to_level_for(DALI_DIM_CURVE_STANDARD,
                                           output,
                                           &level) != DALI_OK) {
        return 1u;
    }
    return level;
}
