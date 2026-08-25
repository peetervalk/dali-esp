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
 *
 * ── What this is not ────────────────────────────────────────────────────────
 *
 * It is a `dali:` block, not a device backup, and the emitted header says so.
 * The registries below hold the values this component was handed and nothing
 * else: not the ESPHome-owned half of an entity (unit, device class, state
 * class, accuracy, id, internal, filters, automations), not the platforms with
 * no registry here (button, number, text), and not the node's own blocks
 * (esphome, wifi, api, ota). An operator who reads this as a file to flash
 * loses all of it silently, which is worth one paragraph up front.
 */

#include "dali_config_export.h"

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/application.h"

extern "C" {
#include "../../../components/dali/dali_dim_curve.h"
#include "../../../components/dali/dali_discovery.h"
#include "../../../components/dali/dali_frame.h"
#include "../../../components/dali/dali_input_device.h"
#include "../../../components/dali/dali_input_poll.h"
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

/*
 * Whether any sensor entity reads this address at all. Only meaningful as a
 * fallback: with no per-instance detail available there is nothing finer to
 * compare against, and reporting a partly-covered address would be worse than
 * staying quiet, since the operator cannot act on it either.
 */
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

/*
 * Whether a sensor entity reads this exact instance.
 *
 * Matched per instance rather than per address because a device mixes instance
 * kinds: a sensor head whose lux and occupancy are wired up but whose
 * temperature is not should have the temperature reported as missing, not go
 * quiet because something else on the same address is covered.
 */
bool input_instance_has_entity(uint8_t addr, uint8_t instance)
{
    for (uint8_t i = 0u; i < dali_registry_sensor_count(); i++) {
        const DaliBusSensor *sensor = dali_registry_sensor_at(i);
        if (sensor == nullptr) continue;

        DaliSensorConfig cfg{};
        sensor->describe_config(&cfg);
        if (cfg.address == addr && cfg.instance == instance) return true;
    }
    return false;
}

/*
 * Whether a dispatch rule names this instance.
 *
 * Only a 24-bit input frame carries the address of the device that sent it, so
 * an `input_24bit` rule with `address_kind: short` is the only kind that can
 * be traced back to one. Everything else the dispatch table holds is real
 * coverage that simply cannot be attributed — see legacy_dispatch_present().
 */
bool input_instance_has_dispatch(uint8_t addr, uint8_t instance)
{
    for (uint8_t i = 0u; i < dali_registry_dispatch_count(); i++) {
        const DaliDispatchEntry *entry = dali_registry_dispatch_at(i);
        if (entry == nullptr) continue;

        if (entry->key.frame_kind != DALI_EVENT_FRAME_INPUT_24BIT) continue;
        if (entry->key.address_kind != DALI_EVENT_ADDRESS_SHORT) continue;
        if (entry->key.address != addr) continue;

        if (entry->key.instance == DALI_DISPATCH_INSTANCE_ANY ||
            entry->key.instance == instance) {
            return true;
        }
    }
    return false;
}

/*
 * Whether any rule matches legacy 16-bit frames.
 *
 * A legacy frame is a forward frame: the coupler sending it is addressing
 * gear, not reporting itself, so there is no source address anywhere in it. A
 * rule matching one may be the entire reason an input device listed below
 * needs nothing further, and no observation of the bus can connect the two.
 * Saying that outright is the honest answer; omitting it lets the export imply
 * a device is unhandled when it is handled perfectly well.
 */
bool legacy_dispatch_present()
{
    for (uint8_t i = 0u; i < dali_registry_dispatch_count(); i++) {
        const DaliDispatchEntry *entry = dali_registry_dispatch_at(i);
        if (entry != nullptr &&
            entry->key.frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT) {
            return true;
        }
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
                       "  # entity omitted: unrecognized target type\r\n");
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
        /*
         * Deliberately no `query_address`. The scan that produced this draft
         * has already told the component which addresses are in the group, and
         * a cold node asks the bus for itself, so writing one here would bake a
         * fact about today's bus into a file that outlives it. `a%u` is named
         * only so the reader knows the group is not empty.
         */
        if (group_query[g] != 0xFFu) {
            dali_cli_printf(out,
                            "%s  # membership is discovered; a%u is one member\r\n",
                            lead, (unsigned) group_query[g]);
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
 * Input instances the bus reported that nothing reads, as commented-out
 * sensor entities.
 *
 * These were once listed as a bare instance count under an instruction to go
 * and run `instances <addr>`. Drafting was avoided on the grounds that a
 * sensor entity needs an instance number and a value width, and inventing
 * those from a count would read as a recommendation while being a guess.
 *
 * It is not a guess any more. A scan reads each instance's type and reported
 * resolution and caches both, so the width comes off the device — through
 * dali_input_poll_bytes_for_resolution(), the conversion the poller itself
 * uses — and the type settles whether a sensor entity is the right answer at
 * all. That second part is what the count could never carry: a push button
 * holds no value to poll, and drafting it a `sensor:` entry yields an entity
 * that polls forever and publishes nothing.
 *
 * What genuinely cannot be read off a device stays out of the draft. Scale,
 * offset, and unit are decisions about what the number should mean, and a
 * plausible-looking guess at those is the failure this function is avoiding,
 * not a smaller version of it.
 */
void emit_discovered_inputs(const DaliCliOut *out,
                            const DaliDiscoveryInventory *inventory,
                            DaliShellInputLookupFn input_lookup,
                            bool under_live_block)
{
    bool header_done = false;
    bool button_seen = false;

    /* As in emit_discovered_lights: with no configured sensor entity there is
     * no `sensor:` key to hang these off, so the block carries its own. */
    const char *lead  = under_live_block ? "  # " : "#   ";
    const char *prose = under_live_block ? "  # " : "# ";

    auto emit_header = [&]() {
        if (header_done) return;
        header_done = true;
        if (under_live_block) {
            dali_cli_write(out,
                           "\r\n  # ── Found on the bus, not read "
                           "───────────────────────────────\r\n");
        } else {
            dali_cli_write(out,
                           "\r\n# ── Found on the bus, not read "
                           "─────────────────────────────────\r\n");
        }
        dali_cli_printf(out,
                        "%sNothing reads these input instances. Where a scan "
                        "has read an instance's\r\n", prose);
        dali_cli_printf(out,
                        "%stype and width, the draft below carries them; "
                        "scale, offset, and unit\r\n", prose);
        dali_cli_printf(out, "%sare yours to add.\r\n", prose);
        if (!under_live_block) dali_cli_write(out, "# sensor:\r\n");
    };

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device =
            dali_discovery_inventory_get(inventory, addr);
        if (device == nullptr || !device->present || !device->has_input_device) {
            continue;
        }

        /*
         * No cached detail means nothing has read this address's instances
         * this session, which is a different answer from "it has none" and
         * gets the older, coarser report.
         */
        DaliDiscoveryInputDevice input{};
        if (input_lookup == nullptr || !input_lookup(addr, &input)) {
            if (input_has_entity(addr)) continue;
            emit_header();
            dali_cli_printf(out,
                            "%saddress %u: %u instance(s), types unread — run "
                            "`instances %u`, then export again\r\n",
                            prose, (unsigned) addr,
                            (unsigned) (device->has_instance_count
                                            ? device->instance_count
                                            : 0u),
                            (unsigned) addr);
            continue;
        }

        uint8_t count = dali_discovery_input_visible_instance_count(&input);
        for (uint8_t inst = 0u; inst < count; inst++) {
            const DaliInputInstanceInfo *info = &input.device.instances[inst];

            if (input_instance_has_entity(addr, inst)) continue;
            if (input_instance_has_dispatch(addr, inst)) continue;

            emit_header();

            if (!info->has_type) {
                dali_cli_printf(out,
                                "%saddress %u instance %u: type unread, so "
                                "nothing can be drafted for it\r\n",
                                prose, (unsigned) addr, (unsigned) inst);
                continue;
            }

            if (info->role == DALI_INPUT_ROLE_PUSH_BUTTON) {
                button_seen = true;
                dali_cli_printf(out,
                                "%saddress %u instance %u: push button — no "
                                "value to poll, so no sensor\r\n",
                                prose, (unsigned) addr, (unsigned) inst);
                dali_cli_printf(out,
                                "%sentity. Route it with a headless_dispatch "
                                "rule on its event.\r\n", prose);
                continue;
            }

            if (info->role != DALI_INPUT_ROLE_OCCUPANCY &&
                info->role != DALI_INPUT_ROLE_LIGHT &&
                info->role != DALI_INPUT_ROLE_ABSOLUTE) {
                dali_cli_printf(out,
                                "%saddress %u instance %u: %s instance, with "
                                "no standard value to\r\n",
                                prose, (unsigned) addr, (unsigned) inst,
                                dali_input_role_name(info->role));
                dali_cli_printf(out,
                                "%spoll — check what the device documents for "
                                "it.\r\n", prose);
                continue;
            }

            dali_cli_printf(out, "%s- platform: dali\r\n", lead);
            dali_cli_printf(out, "%s  name: \"DALI %u %s %u\"\r\n", lead,
                            (unsigned) addr,
                            dali_input_role_name(info->role),
                            (unsigned) inst);
            dali_cli_printf(out, "%s  address: %u\r\n", lead, (unsigned) addr);
            dali_cli_printf(out, "%s  instance: %u\r\n", lead, (unsigned) inst);
            if (info->has_resolution) {
                dali_cli_printf(out, "%s  value_bytes: %u\r\n", lead,
                                (unsigned) dali_input_poll_bytes_for_resolution(
                                    info->resolution));
            } else {
                dali_cli_printf(out,
                                "%s  # resolution unread; value_bytes would "
                                "default to 1\r\n", lead);
            }
        }
    }

    /*
     * Only worth saying once a button has actually been listed: that is the
     * case where the operator is looking at a device the export calls
     * uncovered and may well have wired up already.
     */
    if (button_seen && legacy_dispatch_present()) {
        dali_cli_printf(out,
                        "%sThis config has legacy_16bit dispatch rules. "
                        "Those frames carry no\r\n", prose);
        dali_cli_printf(out,
                        "%ssource address, so one of them may already handle a "
                        "button listed\r\n", prose);
        dali_cli_printf(out,
                        "%sabove — nothing on the bus can say which device "
                        "sent it.\r\n", prose);
    }
}

}  // namespace

/* ── Entry point ─────────────────────────────────────────────────────────── */

void DaliComponent::export_config_yaml(const DaliCliOut *out,
                                       const DaliShellConfigInfo *shell,
                                       const DaliDiscoveryInventory *inventory,
                                       DaliShellInputLookupFn input_lookup)
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
                   "# This is the `dali:` block and the entities naming it — "
                   "a diff against your\r\n"
                   "# source, not a file to flash. Nothing ESPHome owns is "
                   "visible from here:\r\n"
                   "# no esphome:/wifi:/api:/ota: blocks, no "
                   "unit_of_measurement, device_class,\r\n"
                   "# state_class, accuracy_decimals, id, internal, filters, "
                   "or automations on\r\n"
                   "# an entity, and no button:, number:, or text: entries. "
                   "Restoring from this\r\n"
                   "# alone would drop all of it.\r\n"
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
    /*
     * 0 is the schema default and means no periodic gear polling at all, which
     * `poll_interval: 0` states in a way that reads like a broken setting. The
     * schema skips the setter for it too, so omitting the key round-trips to
     * exactly this firmware.
     */
    if (poll_interval_s_ > 0u) {
        dali_cli_printf(out, "  poll_interval: %u\r\n",
                        (unsigned) poll_interval_s_);
    } else {
        dali_cli_write(out,
                       "  # poll_interval unset: no periodic gear polling\r\n");
    }

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
        emit_discovered_inputs(out, inventory, input_lookup, sensor_count > 0u);
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
