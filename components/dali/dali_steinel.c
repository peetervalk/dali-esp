#include "dali_steinel.h"

#include <stddef.h>

static const DaliSteinelInstanceInfo s_hf360_instances[] = {
    { DALI_STEINEL_HF360_INSTANCE_BRIGHTNESS,  4u, "brightness" },
    { DALI_STEINEL_HF360_INSTANCE_MOTION,      3u, "motion" },
    { DALI_STEINEL_HF360_INSTANCE_TEMPERATURE, 0u, "temperature" },
    { DALI_STEINEL_HF360_INSTANCE_HUMIDITY,    0u, "humidity" },
};

const DaliSteinelInstanceInfo *dali_steinel_hf360_instance_lookup(uint8_t instance)
{
    for (uint8_t i = 0u; i < DALI_STEINEL_HF360_II_INSTANCE_COUNT; i++) {
        if (s_hf360_instances[i].instance == instance) {
            return &s_hf360_instances[i];
        }
    }
    return NULL;
}

bool dali_steinel_hf360_expected_instance(uint8_t instance, uint8_t type)
{
    const DaliSteinelInstanceInfo *info = dali_steinel_hf360_instance_lookup(instance);
    return info != NULL && info->type == type;
}

int32_t dali_steinel_temperature_deci_c(uint16_t bin_value)
{
    return (int32_t)bin_value - 50;
}

uint32_t dali_steinel_humidity_deci_percent(uint16_t bin_value)
{
    return (uint32_t)bin_value * 5u;
}

float dali_steinel_temperature_c(uint16_t bin_value)
{
    return (float)dali_steinel_temperature_deci_c(bin_value) / 10.0f;
}

float dali_steinel_humidity_percent(uint16_t bin_value)
{
    return (float)dali_steinel_humidity_deci_percent(bin_value) / 10.0f;
}
