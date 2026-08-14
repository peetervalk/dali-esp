#pragma once

/*
 * dali_config_export.h — reading back the configuration the firmware is running
 *
 * `export config` in the diagnostic shell prints the `dali:` block, and the
 * light/sensor platform entries that name it, as YAML that would rebuild this
 * device. The generator lives in dali_config_export.cpp; this header is the
 * narrow window it needs onto state dali_component.cpp owns.
 *
 * ── Why this is not read from the YAML ──────────────────────────────────────
 *
 * There is no YAML on the device. ESPHome compiles it to C++ on the host, and
 * only the result is flashed — nothing in the image is a copy of the source.
 * What the device does have is every value that source set, sitting in the
 * registries below and on the entities themselves. Reconstructing from those
 * has a property that reading back an embedded copy would not: it describes
 * the firmware that is running, which is the answer worth having on the
 * occasions when it differs from the file on the host.
 *
 * ── Why accessors instead of exposing the registries ────────────────────────
 *
 * s_light_registry, s_sensor_registry, and s_dispatch_table are file-static in
 * dali_component.cpp because their threading rules are file-local: entries are
 * appended during setup() on Core 0 and never afterwards, and several of their
 * fields are touched by the DALI task. Handing out const element pointers
 * keeps that ownership intact and gives the exporter exactly what it needs —
 * the configuration each entry was built from, through describe_config().
 *
 * All three are safe to walk from any task only because registration is over
 * by the time a shell session can exist. Nothing here may be called before
 * setup() completes.
 */

#include "dali_component.h"

extern "C" {
#include "../../../components/dali/dali_dispatch.h"
}

namespace esphome {
namespace dali {

/* Registered light entities, in registration order. Null for a bad index. */
uint8_t             dali_registry_light_count();
const DaliBusLight *dali_registry_light_at(uint8_t index);

/* Registered input-sensor entities, in registration order. */
uint8_t              dali_registry_sensor_count();
const DaliBusSensor *dali_registry_sensor_at(uint8_t index);

/* Loaded headless_dispatch entries, in YAML order. */
uint8_t                  dali_registry_dispatch_count();
const DaliDispatchEntry *dali_registry_dispatch_at(uint8_t index);

}  // namespace dali
}  // namespace esphome
