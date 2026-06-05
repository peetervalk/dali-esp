#pragma once

/*
 * dali_steinel.h - Steinel device profile helpers
 *
 * Keep Steinel-specific instance expectations and value conversions out of the
 * generic protocol/control layers. These helpers do not transmit anything.
 */

#include "dali_protocol.h"

#define DALI_STEINEL_HF360_II_PRODUCT_ID 3742u
#define DALI_STEINEL_HF360_II_INSTANCE_COUNT 4u

typedef enum {
    DALI_STEINEL_HF360_INSTANCE_BRIGHTNESS  = 0,
    DALI_STEINEL_HF360_INSTANCE_MOTION      = 1,
    DALI_STEINEL_HF360_INSTANCE_TEMPERATURE = 2,
    DALI_STEINEL_HF360_INSTANCE_HUMIDITY    = 3,
} DaliSteinelHf360Instance;

typedef struct {
    uint8_t     instance;
    uint8_t     type;
    const char *name;
} DaliSteinelInstanceInfo;

const DaliSteinelInstanceInfo *dali_steinel_hf360_instance_lookup(uint8_t instance);
bool dali_steinel_hf360_expected_instance(uint8_t instance, uint8_t type);

int32_t  dali_steinel_temperature_deci_c(uint16_t bin_value);
uint32_t dali_steinel_humidity_deci_percent(uint16_t bin_value);
float    dali_steinel_temperature_c(uint16_t bin_value);
float    dali_steinel_humidity_percent(uint16_t bin_value);
