/*
 * dali_config_export.cpp — `export config`
 *
 * Prints the `dali:` block, and the light and sensor entries that name it, as
 * YAML that would rebuild this device. See dali_config_export.h for why this
 * is reconstructed from live state rather than read from a file.
 *
 * Two kinds of line come out of here, and the difference is the point:
 *
 *   configuration  what the YAML set, read back from the entities themselves.
 *                  Printed as live YAML, because it already is.
 *   discovery      what the last scan found on the bus and no entity covers.
 *                  Printed commented out, because turning a bus observation
 *                  into an entity is the operator's decision, and a scan that
 *                  ran while a luminaire was unpowered would otherwise delete
 *                  it from the config on the next export.
 *
 * Every value a light entity reports is the configured one, never the one in
 * force — see DaliLightConfig. A group entity that a scan handed a query
 * address prints without one, because that is what the YAML said, and the next
 * scan will supply it again.
 */

#include "dali_config_export.h"

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/application.h"

extern "C" {
#include "../../../components/dali/dali_dim_curve.h"
#include "../../../components/dali/dali_discovery.h"
#include "../../../components/dali/dali_frame.h"
}

namespace esphome {
namespace dali {

namespace {

/* ── Value spellings ─────────────────────────────────────────────────────────
 *
 * Each of these maps a C enum back to the word the YAML schema accepts. They
 * are the inverse of the tables in __init__.py; a value with no spelling means
 * the two have drifted, so it prints as a number and a comment rather than as
 * a plausible word that would not validate.
 * ---------------------------------------------------------------------------*/

const char *target_type_name(uint8_t type)
{
    switch (type) {
        case DALI_ADDR_SHORT:     return "short";
        case DALI_ADDR_GROUP:     return "group";
        case DALI_ADDR_BROADCAST: return "broadcast";
        default:                  return nullptr;
    }
}

const char *dim_curve_name(uint8_t curve)
{
    switch (curve) {
        case DALI_DIM_CURVE_STANDARD: return "standard";
        case DALI_DIM_CURVE_LINEAR:   return "linear";
        default:                      return nullptr;
    }
}

const char *frame_kind_name(uint8_t kind)
{
    switch (kind) {
        case DALI_EVENT_FRAME_LEGACY_16BIT: return "legacy_16bit";
        case DALI_EVENT_FRAME_INPUT_24BIT:  return "input_24bit";
        default:                            return nullptr;
    }
}

const char *address_kind_name(uint8_t kind)
{
    switch (kind) {
        case DALI_EVENT_ADDRESS_INVALID:   return "none";
        case DALI_EVENT_ADDRESS_SHORT:     return "short";
        case DALI_EVENT_ADDRESS_GROUP:     return "group";
        case DALI_EVENT_ADDRESS_BROADCAST: return "broadcast";
        default:                           return nullptr;
    }
}

const char *dispatch_action_name(uint8_t action)
{
    switch (action) {
        case DALI_DISPATCH_ACTION_OBSERVE:    return "observe";
        case DALI_DISPATCH_ACTION_MIRROR:     return "mirror";
        case DALI_DISPATCH_ACTION_RECALL_MAX: return "recall_max";
        case DALI_DISPATCH_ACTION_RECALL_MIN: return "recall_min";
        case DALI_DISPATCH_ACTION_OFF:        return "off";
        case DALI_DISPATCH_ACTION_GO_TO_LAST: return "go_to_last";
        case DALI_DISPATCH_ACTION_DIM_UP:     return "dim_up";
        case DALI_DISPATCH_ACTION_DIM_DOWN:   return "dim_down";
        case DALI_DISPATCH_ACTION_SCENE:      return "scene";
        case DALI_DISPATCH_ACTION_TOGGLE:     return "toggle";
        default:                              return nullptr;
    }
}

/* ── Emitters ─────────────────────────────────────────────────────────────── */

/*
 * A name as a YAML double-quoted scalar. Quoting unconditionally rather than
 * only when it looks necessary: an entity called "on", "12:00", or "Yes" is a
 * bool, a sexagesimal, and a bool respectively to a YAML parser, and each
 * would load as something that is not the name the operator chose.
 *
 * Written a chunk at a time through dali_cli_write() because a name has no
 * length bound worth trusting against DALI_CLI_FORMAT_MAX.
 */
void emit_quoted(const DaliCliOut *out, const char *text)
{
    dali_cli_write(out, "\"");
    if (text != nullptr) {
        char   chunk[64];
        size_t len = 0u;
        for (const char *p = text; *p != '\0'; p++) {
            if (len + 2u >= sizeof(chunk)) {
                chunk[len] = '\0';
                dali_cli_write(out, chunk);
                len = 0u;
            }
            if (*p == '"' || *p == '\\') {
                chunk[len++] = '\\';
            }
            chunk[len++] = *p;
        }
        chunk[len] = '\0';
        dali_cli_write(out, chunk);
    }
    dali_cli_write(out, "\"");
}

/* `key: "name"` at a given indent — the shape every entity line starts with. */
void emit_named_key(const DaliCliOut *out, const char *indent, const char *key,
                    const char *name)
{
    dali_cli_printf(out, "%s%s: ", indent, key);
    emit_quoted(out, name);
    dali_cli_write(out, "\r\n");
}

/* An optional text sensor as its two-line block; nothing at all when unset. */
void emit_text_sensor(const DaliCliOut *out, const char *key,
                      text_sensor::TextSensor *sensor)
{
    if (sensor == nullptr) return;
    dali_cli_printf(out, "  %s:\r\n", key);
    emit_named_key(out, "    ", "name", sensor->get_name().c_str());
}

/* A group bitmask as a YAML flow sequence: `[1, 4, 7]`. */
void emit_group_list(const DaliCliOut *out, uint16_t groups)
{
    dali_cli_write(out, "[");
    bool first = true;
    for (uint8_t g = 0u; g < 16u; g++) {
        if ((groups & (1u << g)) == 0u) continue;
        dali_cli_printf(out, first ? "%u" : ", %u", (unsigned) g);
        first = false;
    }
    dali_cli_write(out, "]");
}

/* ── Coverage ────────────────────────────────────────────────────────────────
 *
 * Whether a discovered address already reaches an entity, so the export can
 * suggest only what is genuinely missing. A group entity covers its members,
 * which is why this needs the inventory: group membership is a fact about the
 * gear, and the entity records only which group it drives.
 * ---------------------------------------------------------------------------*/

bool gear_has_entity(uint8_t addr, const DaliDiscoveryDeviceInfo *device)
{
    for (uint8_t i = 0u; i < dali_registry_light_count(); i++) {
        const DaliBusLight *light = dali_registry_light_at(i);
        if (light == nullptr) continue;

        DaliLightConfig cfg{};
        light->describe_config(&cfg);

        if (cfg.target_type == DALI_ADDR_BROADCAST) return true;
        if (cfg.target_type == DALI_ADDR_SHORT && cfg.target_address == addr) {
            return true;
        }
        if (cfg.target_type == DALI_ADDR_GROUP && device != nullptr &&
            device->has_groups && cfg.target_address < 16u &&
            (device->groups & (1u << cfg.target_address)) != 0u) {
            return true;
        }
    }
    return false;
}

bool input_has_entity(uint8_t addr)
{
    for (uint8_t i = 0u; i < dali_registry_sensor_count(); i++) {
        const DaliBusSensor *sensor = dali_registry_sensor_at(i);
        if (sensor == nullptr) continue;

        DaliSensorConfig cfg{};
        sensor->describe_config(&cfg);
        if (cfg.address == addr) return true;
    }
    return false;
}

/* ── Sections ─────────────────────────────────────────────────────────────── */

void emit_dispatch_entry(const DaliCliOut *out, const DaliDispatchEntry *entry)
{
    const char *frame  = frame_kind_name((uint8_t) entry->key.frame_kind);
    const char *source = address_kind_name((uint8_t) entry->key.address_kind);
    const char *output = target_type_name((uint8_t) entry->output.type);
    const char *action = dispatch_action_name((uint8_t) entry->action);

    if (frame == nullptr || source == nullptr || output == nullptr ||
        action == nullptr) {
        dali_cli_write(out,
                       "    # entry omitted: firmware holds a value this "
                       "export has no spelling for\r\n");
        return;
    }

    dali_cli_printf(out, "    - frame_kind: %s\r\n", frame);
    dali_cli_printf(out, "      address_kind: %s\r\n", source);
    dali_cli_printf(out, "      address: %u\r\n", (unsigned) entry->key.address);

    if (entry->key.event_information == DALI_DISPATCH_EVENT_ANY) {
        dali_cli_write(out, "      event_information: any\r\n");
    } else {
        dali_cli_printf(out, "      event_information: %u\r\n",
                        (unsigned) entry->key.event_information);
    }

    if (entry->key.instance == DALI_DISPATCH_INSTANCE_ANY) {
        dali_cli_write(out, "      instance: any\r\n");
    } else {
        dali_cli_printf(out, "      instance: %u\r\n",
                        (unsigned) entry->key.instance);
    }

    dali_cli_printf(out, "      output_type: %s\r\n", output);
    dali_cli_printf(out, "      output_address: %u\r\n",
                    (unsigned) entry->output.address);
    dali_cli_printf(out, "      action: %s\r\n", action);

    if (entry->action == DALI_DISPATCH_ACTION_SCENE) {
        dali_cli_printf(out, "      scene: %u\r\n", (unsigned) entry->scene);
    }
}

void emit_light_entity(const DaliCliOut *out, const DaliLightConfig *cfg)
{
    const char *type = target_type_name(cfg->target_type);
    if (type == nullptr) {
        dali_cli_write(out,
                       "  # entity omitted: unrecognised target type\r\n");
        return;
    }

    dali_cli_write(out, "  - platform: dali\r\n");
    emit_named_key(out, "    ", "name", cfg->name);
    dali_cli_printf(out, "    target_type: %s\r\n", type);
    dali_cli_printf(out, "    target_address: %u\r\n",
                    (unsigned) cfg->target_address);

    if (cfg->query_address != 0xFFu) {
        dali_cli_printf(out, "    query_address: %u\r\n",
                        (unsigned) cfg->query_address);
    }
    if (cfg->member_groups != 0u) {
        dali_cli_write(out, "    member_groups: ");
        emit_group_list(out, cfg->member_groups);
        dali_cli_write(out, "\r\n");
    }
    if (cfg->has_min_level) {
        dali_cli_printf(out, "    min_level: %u\r\n", (unsigned) cfg->min_level);
    }
    if (cfg->has_max_level) {
        dali_cli_printf(out, "    max_level: %u\r\n", (unsigned) cfg->max_level);
    }
    if (cfg->has_dimming_curve) {
        const char *curve = dim_curve_name(cfg->dimming_curve);
        if (curve != nullptr) {
            dali_cli_printf(out, "    dimming_curve: %s\r\n", curve);
        }
    }
}

void emit_sensor_entity(const DaliCliOut *out, const DaliSensorConfig *cfg)
{
    dali_cli_write(out, "  - platform: dali\r\n");
    emit_named_key(out, "    ", "name", cfg->name);
    dali_cli_printf(out, "    address: %u\r\n", (unsigned) cfg->address);
    dali_cli_printf(out, "    instance: %u\r\n", (unsigned) cfg->instance);
    dali_cli_printf(out, "    poll_interval: %u\r\n",
                    (unsigned) cfg->poll_interval_s);
    dali_cli_printf(out, "    poll_on_event: %s\r\n",
                    cfg->poll_on_event ? "true" : "false");
    dali_cli_printf(out, "    value_bytes: %u\r\n", (unsigned) cfg->value_bytes);
    dali_cli_printf(out, "    scale: %g\r\n", (double) cfg->scale);
    dali_cli_printf(out, "    offset: %g\r\n", (double) cfg->offset);
}

/*
 * Gear the bus reported that nothing drives, as commented-out entities.
 *
 * Groups come first and individually-addressed gear second, because that is
 * the order the choice is usually made in: a group entity covers its members
 * in one entry, and the per-address suggestions below it exist for gear that
 * belongs to no group at all — commissioned but never assigned, which is
 * exactly the state a fresh bus is in.
 */
void emit_discovered_lights(const DaliCliOut *out,
                            const DaliDiscoveryInventory *inventory,
                            bool under_live_block)
{
    uint16_t groups_with_gear = 0u;
    uint8_t  group_query[16];
    bool     header_done = false;

    /*
     * With no configured light entity there is no `light:` key to hang these
     * off, and adding one would break the file: a key whose only content is
     * comments loads as null, and the light domain wants a list. So the whole
     * block gets commented, header included, and uncommenting it is one
     * operation rather than one plus remembering to add the key.
     */
    const char *lead = under_live_block ? "  # " : "#   ";

    auto emit_header = [&]() {
        if (header_done) return;
        header_done = true;
        if (under_live_block) {
            dali_cli_write(out,
                           "\r\n  # ── Found on the bus, not configured "
                           "─────────────────────────\r\n");
        } else {
            dali_cli_write(out,
                           "\r\n# ── Found on the bus, not configured "
                           "───────────────────────────\r\n"
                           "# Nothing is configured to drive this gear. "
                           "Uncomment what you want.\r\n"
                           "# light:\r\n");
        }
    };

    for (uint8_t g = 0u; g < 16u; g++) group_query[g] = 0xFFu;

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device =
            dali_discovery_inventory_get(inventory, addr);
        if (device == nullptr || !device->present || !device->has_groups) continue;
        if (device->has_input_device && !device->has_control_gear) continue;

        for (uint8_t g = 0u; g < 16u; g++) {
            if ((device->groups & (1u << g)) == 0u) continue;
            groups_with_gear |= (uint16_t) (1u << g);
            if (group_query[g] == 0xFFu) group_query[g] = addr;
        }
    }

    for (uint8_t g = 0u; g < 16u; g++) {
        if ((groups_with_gear & (1u << g)) == 0u) continue;

        bool covered = false;
        for (uint8_t i = 0u; i < dali_registry_light_count() && !covered; i++) {
            const DaliBusLight *light = dali_registry_light_at(i);
            if (light == nullptr) continue;
            DaliLightConfig cfg{};
            light->describe_config(&cfg);
            covered = (cfg.target_type == DALI_ADDR_GROUP &&
                       cfg.target_address == g) ||
                      cfg.target_type == DALI_ADDR_BROADCAST;
        }
        if (covered) continue;

        emit_header();

        dali_cli_printf(out, "%s- platform: dali\r\n", lead);
        dali_cli_printf(out, "%s  name: \"Group %u\"\r\n", lead, (unsigned) g);
        dali_cli_printf(out, "%s  target_type: group\r\n", lead);
        dali_cli_printf(out, "%s  target_address: %u\r\n", lead, (unsigned) g);
        if (group_query[g] != 0xFFu) {
            dali_cli_printf(out, "%s  query_address: %u\r\n", lead,
                            (unsigned) group_query[g]);
        }
    }

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device =
            dali_discovery_inventory_get(inventory, addr);
        if (device == nullptr || !device->present) continue;
        if (device->has_input_device && !device->has_control_gear) continue;
        if (gear_has_entity(addr, device)) continue;

        emit_header();

        dali_cli_printf(out, "%s- platform: dali\r\n", lead);
        dali_cli_printf(out, "%s  name: \"DALI %u\"", lead, (unsigned) addr);
        if (device->has_device_type) {
            dali_cli_printf(out, "  # %s",
                            dali_discovery_device_type_name(device->device_type));
        }
        dali_cli_write(out, "\r\n");
        dali_cli_printf(out, "%s  target_type: short\r\n", lead);
        dali_cli_printf(out, "%s  target_address: %u\r\n", lead, (unsigned) addr);
    }
}

/*
 * Input devices the bus reported that no sensor entity reads.
 *
 * Listed rather than drafted: a sensor entity needs an instance number and a
 * value width, and picking those from an instance count would be a guess that
 * reads as a recommendation. `instances <addr>` answers it properly.
 */
void emit_discovered_inputs(const DaliCliOut *out,
                            const DaliDiscoveryInventory *inventory)
{
    bool first = true;

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device =
            dali_discovery_inventory_get(inventory, addr);
        if (device == nullptr || !device->present || !device->has_input_device) {
            continue;
        }
        if (input_has_entity(addr)) continue;

        if (first) {
            dali_cli_write(out,
                           "\r\n# Input devices found with no sensor entity. "
                           "Run `instances <addr>` for\r\n"
                           "# the instance numbers and value widths a "
                           "`sensor:` entry needs.\r\n");
            first = false;
        }
        dali_cli_printf(out, "#   address %u: %u instance(s)\r\n",
                        (unsigned) addr,
                        (unsigned) (device->has_instance_count
                                        ? device->instance_count
                                        : 0u));
    }
}

}  // namespace

/* ── Entry point ─────────────────────────────────────────────────────────── */

void DaliComponent::export_config_yaml(const DaliCliOut *out,
                                       const DaliShellConfigInfo *shell,
                                       const DaliDiscoveryInventory *inventory)
{
    if (out == nullptr) return;

    dali_cli_write(out,
                   "# ── dali: configuration ─────────────────────────────"
                   "──────────────────────\r\n"
                   "#\r\n"
                   "# Reconstructed from the running firmware. ESPHome "
                   "compiles the YAML on the\r\n"
                   "# host and flashes only the result, so this describes "
                   "what the device is\r\n"
                   "# configured as rather than what any file on disk "
                   "says.\r\n"
                   "#\r\n"
                   "# `id:` is omitted: ESPHome generates one, and the "
                   "entries below resolve to\r\n"
                   "# this block without it. Re-add yours if something "
                   "outside this export\r\n"
                   "# refers to it by name.\r\n");
    dali_cli_write(out, "# Node: ");
    dali_cli_write(out, App.get_name().c_str());
    dali_cli_write(out, "\r\n\r\n");

    /* ── dali: ───────────────────────────────────────────────────────────── */

    dali_cli_write(out, "dali:\r\n");
    dali_cli_printf(out, "  tx_pin: GPIO%u\r\n", (unsigned) tx_pin_);
    dali_cli_printf(out, "  rx_pin: GPIO%u\r\n", (unsigned) rx_pin_);
    dali_cli_printf(out, "  poll_interval: %u\r\n", (unsigned) poll_interval_s_);

    emit_text_sensor(out, "scan_status",     scan_status_);
    emit_text_sensor(out, "scan_result",     scan_result_);
    emit_text_sensor(out, "yaml_result",     yaml_result_);
    emit_text_sensor(out, "couplers_result", couplers_result_);
    emit_text_sensor(out, "bus_monitor",     bus_monitor_);
    emit_text_sensor(out, "command_result",  command_result_);
    emit_text_sensor(out, "bus_fault",       bus_fault_);

    if (shell != nullptr) {
        dali_cli_write(out, "  shell:\r\n");
        dali_cli_printf(out, "    port: %u\r\n", (unsigned) shell->port);
        dali_cli_printf(out, "    idle_timeout: %us\r\n",
                        (unsigned) shell->idle_timeout_s);
        dali_cli_printf(out, "    allow_commissioning: %s\r\n",
                        shell->allow_commissioning ? "true" : "false");
    }

    uint8_t dispatch_count = dali_registry_dispatch_count();
    if (dispatch_count > 0u) {
        dali_cli_write(out, "  headless_dispatch:\r\n");
        for (uint8_t i = 0u; i < dispatch_count; i++) {
            const DaliDispatchEntry *entry = dali_registry_dispatch_at(i);
            if (entry != nullptr) emit_dispatch_entry(out, entry);
        }
    }

    /* ── light: ──────────────────────────────────────────────────────────── */

    uint8_t light_count = dali_registry_light_count();
    if (light_count > 0u) {
        dali_cli_write(out, "\r\nlight:\r\n");
        for (uint8_t i = 0u; i < light_count; i++) {
            const DaliBusLight *light = dali_registry_light_at(i);
            if (light == nullptr) continue;
            DaliLightConfig cfg{};
            light->describe_config(&cfg);
            emit_light_entity(out, &cfg);
        }
    }
    if (inventory != nullptr) {
        emit_discovered_lights(out, inventory, light_count > 0u);
    }

    /* ── sensor: ─────────────────────────────────────────────────────────── */

    uint8_t sensor_count = dali_registry_sensor_count();
    if (sensor_count > 0u) {
        dali_cli_write(out, "\r\nsensor:\r\n");
        for (uint8_t i = 0u; i < sensor_count; i++) {
            const DaliBusSensor *sensor = dali_registry_sensor_at(i);
            if (sensor == nullptr) continue;
            DaliSensorConfig cfg{};
            sensor->describe_config(&cfg);
            emit_sensor_entity(out, &cfg);
        }
    }

    if (inventory != nullptr) {
        emit_discovered_inputs(out, inventory);
    } else {
        dali_cli_write(out,
                       "\r\n# No scan in this session, so nothing is "
                       "reported about what is on the\r\n"
                       "# bus. Run `discover`, then export again to have "
                       "uncovered gear listed.\r\n");
    }
}

}  // namespace dali
}  // namespace esphome
