#pragma once

#include <stdint.h>

// Forward declaration — avoids pulling DaliComponent into the scan task headers
namespace esphome { namespace dali { class DaliComponent; } }

#ifdef __cplusplus
extern "C" {
#endif

void dali_scan_start(esphome::dali::DaliComponent *component);

#ifdef __cplusplus
}
#endif
