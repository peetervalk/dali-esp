#include "dali_discovery.h"
#include "dali_memory.h"
#include "dali_gear_dt6.h"
#include "dali_gear_dt8.h"

#include <string.h>

/* DALI-2 QUERY DEVICE TYPE / QUERY NEXT DEVICE TYPE sentinel replies. */
#define DALI_DISCOVERY_QUERY_RETRIES_LEFT      1u
#define DALI_DISCOVERY_DEVICE_TYPE_NONE_OR_END 0xFEu
#define DALI_DISCOVERY_DEVICE_TYPE_MULTIPLE    0xFFu

typedef DaliError (*DaliDiscoveryInputQueryBuilder)(uint8_t addr,
                                                    uint8_t instance,
                                                    DaliFrame *out);

/* Send ENABLE DEVICE TYPE N (no reply) then a query, return the byte. */
static DaliError discovery_query_dt_typed(const DaliDiscoveryTransport *transport,
                                          DaliFrame enable,
                                          const DaliFrame *query,
                                          uint8_t *out)
{
    DaliError err = transport->transact(&enable, false, 0u, false, NULL, transport->ctx);
    if (err != DALI_OK) {
        return err;
    }
    return dali_discovery_query_u8(transport, query, out);
}

static DaliError discovery_query_dt6(const DaliDiscoveryTransport *transport,
                                     const DaliFrame *query,
                                     uint8_t *out)
{
    return discovery_query_dt_typed(transport, dali_dt6_enable(), query, out);
}

static DaliError discovery_query_dt8(const DaliDiscoveryTransport *transport,
                                     const DaliFrame *query,
                                     uint8_t *out)
{
    return discovery_query_dt_typed(transport, dali_dt8_enable(), query, out);
}

static bool transport_valid(const DaliDiscoveryTransport *transport)
{
    return transport != NULL && transport->transact != NULL;
}

bool dali_discovery_has_device_type(const DaliDiscoveryDeviceInfo *device, uint8_t type)
{
    if (device == NULL || !device->has_device_type) {
        return false;
    }
    for (uint8_t i = 0u; i < device->device_type_count; i++) {
        if (device->device_types[i] == type) {
            return true;
        }
    }
    return false;
}

static bool scan_error_is_absent(DaliError err)
{
    return err == DALI_ERR_TIMEOUT || err == DALI_ERR_MALFORMED;
}

DaliError dali_discovery_inventory_reset(DaliDiscoveryInventory *inventory)
{
    if (inventory == NULL) {
        return DALI_ERR_INVALID;
    }

    memset(inventory, 0, sizeof(*inventory));
    return DALI_OK;
}

const DaliDiscoveryDeviceInfo *dali_discovery_inventory_get(
    const DaliDiscoveryInventory *inventory,
    uint8_t addr)
{
    if (inventory == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return NULL;
    }
    return &inventory->devices[addr];
}

DaliError dali_discovery_inventory_store_status(DaliDiscoveryInventory *inventory,
                                                uint8_t addr,
                                                uint8_t status)
{
    if (inventory == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliDiscoveryDeviceInfo *device = &inventory->devices[addr];
    if (!device->present) {
        inventory->found_count++;
    }
    device->present = true;
    device->has_status = true;
    device->has_control_gear = true;
    device->status = status;
    return DALI_OK;
}

DaliError dali_discovery_inventory_store_groups(DaliDiscoveryInventory *inventory,
                                                uint8_t addr,
                                                uint16_t groups)
{
    if (inventory == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliDiscoveryDeviceInfo *device = &inventory->devices[addr];
    device->has_groups = true;
    device->groups = groups;
    return DALI_OK;
}

DaliError dali_discovery_inventory_update_input_device(
    DaliDiscoveryInventory *inventory,
    const DaliDiscoveryInputDevice *input_device)
{
    if (inventory == NULL ||
        input_device == NULL ||
        input_device->device.address >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    uint8_t addr = input_device->device.address;
    DaliDiscoveryDeviceInfo *device = &inventory->devices[addr];
    if (!device->present) {
        inventory->found_count++;
    }
    device->present = true;
    device->has_input_device = true;
    device->has_instance_count = input_device->device.has_instance_count;
    device->instance_count = input_device->device.instance_count;
    return DALI_OK;
}

DaliError dali_discovery_query_u8(const DaliDiscoveryTransport *transport,
                                  const DaliFrame *frame,
                                  uint8_t *out)
{
    if (!transport_valid(transport) || frame == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame reply = {0u, 0u};
    DaliError err = transport->transact(frame,
                                        true,
                                        DALI_DISCOVERY_QUERY_RETRIES_LEFT,
                                        false,
                                        &reply,
                                        transport->ctx);
    if (err != DALI_OK) {
        return err;
    }
    if (reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_MALFORMED;
    }

    *out = (uint8_t)(reply.data & 0xFFu);
    return DALI_OK;
}

DaliError dali_discovery_query_status(const DaliDiscoveryTransport *transport,
                                      uint8_t addr,
                                      uint8_t *status_out)
{
    if (!transport_valid(transport) || status_out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliTarget target = {
        .type = DALI_ADDR_SHORT,
        .address = addr,
    };
    DaliError err = dali_control_build_query_status(target, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_discovery_query_u8(transport, &frame, status_out);
}

static DaliError discovery_query_simple(const DaliDiscoveryTransport *transport,
                                        uint8_t addr,
                                        DaliCommandId id,
                                        uint8_t *out)
{
    DaliFrame frame;
    DaliTarget target = { .type = DALI_ADDR_SHORT, .address = addr };
    DaliError err = dali_control_build_query(target, id, 0u, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_discovery_query_u8(transport, &frame, out);
}

DaliError dali_discovery_query_groups(const DaliDiscoveryTransport *transport,
                                      uint8_t addr,
                                      uint16_t *groups_out)
{
    if (!transport_valid(transport) || groups_out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliTarget target = { .type = DALI_ADDR_SHORT, .address = addr };

    DaliError err = dali_control_build_query(target, DALI_CMD_QUERY_GROUPS_0_7, 0u, &frame);
    if (err != DALI_OK) {
        return err;
    }
    uint8_t low = 0u;
    err = dali_discovery_query_u8(transport, &frame, &low);
    if (err != DALI_OK) {
        return err;
    }

    err = dali_control_build_query(target, DALI_CMD_QUERY_GROUPS_8_15, 0u, &frame);
    if (err != DALI_OK) {
        return err;
    }
    uint8_t high = 0u;
    err = dali_discovery_query_u8(transport, &frame, &high);
    if (err != DALI_OK) {
        return err;
    }

    *groups_out = (uint16_t)low | ((uint16_t)high << 8u);
    return DALI_OK;
}

DaliError dali_discovery_query_device_type(const DaliDiscoveryTransport *transport,
                                           uint8_t addr,
                                           uint8_t *type_out)
{
    if (!transport_valid(transport) || type_out == NULL) {
        return DALI_ERR_INVALID;
    }
    return discovery_query_simple(transport, addr, DALI_CMD_QUERY_DEVICE_TYPE, type_out);
}

DaliError dali_discovery_query_version(const DaliDiscoveryTransport *transport,
                                       uint8_t addr,
                                       uint8_t *version_out)
{
    if (!transport_valid(transport) || version_out == NULL) {
        return DALI_ERR_INVALID;
    }
    return discovery_query_simple(transport, addr, DALI_CMD_QUERY_VERSION_NUMBER, version_out);
}

DaliError dali_discovery_query_actual_level(const DaliDiscoveryTransport *transport,
                                            uint8_t addr,
                                            uint8_t *level_out)
{
    if (!transport_valid(transport) || level_out == NULL) {
        return DALI_ERR_INVALID;
    }
    return discovery_query_simple(transport, addr, DALI_CMD_QUERY_ACTUAL_LEVEL, level_out);
}

const char *dali_discovery_device_type_name(uint8_t type)
{
    switch (type) {
    case 0:   return "fluorescent";
    case 1:   return "emergency";
    case 2:   return "HID";
    case 3:   return "halogen-LV";
    case 4:   return "incandescent";
    case 5:   return "DC-controlled";
    case 6:   return "LED";
    case 7:   return "switching";
    case 8:   return "colour";
    default:  return "unknown";
    }
}

static void discovery_store_device_type(DaliDiscoveryDeviceInfo *device,
                                        uint8_t type)
{
    if (device->device_type_count >= DALI_DISCOVERY_MAX_DEVICE_TYPES) {
        device->device_types_truncated = true;
        return;
    }

    device->device_types[device->device_type_count++] = type;
    if (!device->has_device_type) {
        device->has_device_type = true;
        device->device_type = type;
    }
}

static void discovery_enrich_device_types(const DaliDiscoveryTransport *transport,
                                          uint8_t addr,
                                          DaliDiscoveryDeviceInfo *device)
{
    uint8_t type = 0u;
    if (dali_discovery_query_device_type(transport, addr, &type) != DALI_OK ||
        type == DALI_DISCOVERY_DEVICE_TYPE_NONE_OR_END) {
        return;
    }

    if (type != DALI_DISCOVERY_DEVICE_TYPE_MULTIPLE) {
        discovery_store_device_type(device, type);
        return;
    }

    DaliTarget target = { .type = DALI_ADDR_SHORT, .address = addr };
    DaliFrame next_query;
    if (dali_control_build_query(target, DALI_CMD_QUERY_NEXT_DEVICE_TYPE,
                                 0u, &next_query) != DALI_OK) {
        return;
    }

    for (;;) {
        uint8_t next_type = 0u;
        if (dali_discovery_query_u8(transport, &next_query, &next_type) != DALI_OK ||
            next_type == DALI_DISCOVERY_DEVICE_TYPE_NONE_OR_END ||
            next_type == DALI_DISCOVERY_DEVICE_TYPE_MULTIPLE) {
            return;
        }
        if (device->device_type_count > 0u &&
            next_type <= device->device_types[device->device_type_count - 1u]) {
            return;
        }

        discovery_store_device_type(device, next_type);
        if (device->device_types_truncated) {
            return;
        }
    }
}

static void discovery_enrich_device(const DaliDiscoveryTransport *transport,
                                    uint8_t addr,
                                    DaliDiscoveryDeviceInfo *device)
{
    DaliTarget target = { .type = DALI_ADDR_SHORT, .address = addr };

    uint16_t groups = 0u;
    if (dali_discovery_query_groups(transport, addr, &groups) == DALI_OK) {
        device->has_groups = true;
        device->groups = groups;
    }

    discovery_enrich_device_types(transport, addr, device);

    uint8_t version = 0u;
    if (dali_discovery_query_version(transport, addr, &version) == DALI_OK) {
        device->has_version = true;
        device->version = version;
    }

    uint8_t level = 0u;
    if (dali_discovery_query_actual_level(transport, addr, &level) == DALI_OK) {
        device->has_actual_level = true;
        device->actual_level = level;
    }

    DaliFrame instances_frame;
    if (dali_input_build_query_number_of_instances(addr, &instances_frame) == DALI_OK) {
        uint8_t count = 0u;
        if (dali_discovery_query_u8(transport, &instances_frame, &count) == DALI_OK &&
            count > 0u) {
            device->has_input_device = true;
            device->has_instance_count = true;
            device->instance_count = count;
        }
    }

    if (device->has_control_gear) {
        DaliMemoryBank0Identity identity;
        if (dali_memory_read_bank0_identity(transport, addr, &identity) == DALI_OK) {
            device->has_identity = true;
            device->identity = identity;
        }
    }

    /* Scene levels — 0xFF means scene not configured ("MASK"). */
    for (uint8_t scene = 0u; scene < DALI_SCENE_COUNT; scene++) {
        DaliFrame scene_q;
        if (dali_control_build_query(target, DALI_CMD_QUERY_SCENE_LEVEL,
                                      scene, &scene_q) != DALI_OK) {
            continue;
        }
        uint8_t scene_level = 0u;
        if (dali_discovery_query_u8(transport, &scene_q, &scene_level) == DALI_OK) {
            device->scene_levels[scene] = scene_level;
            device->has_scene_levels = true;
        }
    }

    if (dali_discovery_has_device_type(device, 6u)) {
        uint8_t failure = 0u;
        DaliFrame fail_q = dali_dt6_query_failure_status(addr);
        if (discovery_query_dt6(transport, &fail_q, &failure) == DALI_OK) {
            device->has_dt6 = true;
            device->dt6_failure_status = failure;
        }

        uint8_t features = 0u;
        DaliFrame feat_q = dali_dt6_query_features(addr);
        if (discovery_query_dt6(transport, &feat_q, &features) == DALI_OK) {
            device->dt6_features = features;
        }
    }

    if (dali_discovery_has_device_type(device, 8u)) {
        uint8_t gear_features = 0u;
        DaliFrame gf_q = dali_dt8_query_gear_features_status(addr);
        if (discovery_query_dt8(transport, &gf_q, &gear_features) == DALI_OK) {
            device->has_dt8 = true;
            device->dt8_gear_features = gear_features;
        }

        uint8_t colour_status = 0u;
        DaliFrame cs_q = dali_dt8_query_colour_status(addr);
        if (discovery_query_dt8(transport, &cs_q, &colour_status) == DALI_OK) {
            device->dt8_colour_status = colour_status;
        }

        uint8_t type_features = 0u;
        DaliFrame tf_q = dali_dt8_query_colour_type_features(addr);
        if (discovery_query_dt8(transport, &tf_q, &type_features) == DALI_OK) {
            device->dt8_colour_type_features = type_features;
        }
    }
}

DaliError dali_discovery_scan(DaliDiscoveryInventory *inventory,
                              const DaliDiscoveryTransport *transport,
                              DaliDiscoveryFoundCb found_cb,
                              void *found_ctx,
                              uint8_t *found_out)
{
    if (inventory == NULL || !transport_valid(transport)) {
        return DALI_ERR_INVALID;
    }

    DaliError reset_err = dali_discovery_inventory_reset(inventory);
    if (reset_err != DALI_OK) {
        return reset_err;
    }

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        uint8_t status = 0u;
        DaliError err = dali_discovery_query_status(transport, addr, &status);
        if (err == DALI_OK) {
            err = dali_discovery_inventory_store_status(inventory, addr, status);
            if (err != DALI_OK) {
                return err;
            }
            discovery_enrich_device(transport, addr, &inventory->devices[addr]);
            if (found_cb != NULL) {
                found_cb(addr, &inventory->devices[addr], found_ctx);
            }
            continue;
        }
        if (!scan_error_is_absent(err)) {
            return err;
        }
        /* No gear status reply — try QUERY NUMBER OF INSTANCES to detect a
         * pure DALI-2 input device that has no control gear (Part 303). */
        DaliFrame inst_q;
        uint8_t count = 0u;
        if (dali_input_build_query_number_of_instances(addr, &inst_q) == DALI_OK &&
            dali_discovery_query_u8(transport, &inst_q, &count) == DALI_OK &&
            count > 0u) {
            DaliDiscoveryDeviceInfo *device = &inventory->devices[addr];
            if (!device->present) {
                inventory->found_count++;
            }
            device->present          = true;
            device->has_input_device = true;
            device->has_instance_count = true;
            device->instance_count   = count;
            discovery_enrich_device(transport, addr, device);
            if (found_cb != NULL) {
                found_cb(addr, device, found_ctx);
            }
        }
    }

    inventory->valid = true;
    if (found_out != NULL) {
        *found_out = inventory->found_count;
    }
    return DALI_OK;
}

static void discovery_input_reset(DaliDiscoveryInputDevice *out, uint8_t addr)
{
    memset(out, 0, sizeof(*out));
    out->device.address = addr;
    for (uint8_t i = 0u; i < DALI_INPUT_MAX_INSTANCES; i++) {
        out->device.instances[i].instance = i;
        out->instance_type_errors[i] = DALI_ERR_INVALID;
    }
}

static DaliError query_instance_u8(const DaliDiscoveryTransport *transport,
                                   DaliDiscoveryInputQueryBuilder builder,
                                   uint8_t addr,
                                   uint8_t instance,
                                   uint8_t *out)
{
    if (!transport_valid(transport) || builder == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliError err = builder(addr, instance, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_discovery_query_u8(transport, &frame, out);
}

static void query_instance_optional_fields(const DaliDiscoveryTransport *transport,
                                           uint8_t addr,
                                           DaliInputInstanceInfo *info)
{
    if (info == NULL) {
        return;
    }

    uint8_t raw = 0u;
    if (query_instance_u8(transport,
                          dali_input_build_query_instance_enabled,
                          addr,
                          info->instance,
                          &raw) == DALI_OK) {
        info->has_enabled = true;
        info->enabled = dali_is_yes(raw);
    }
    if (query_instance_u8(transport,
                          dali_input_build_query_resolution,
                          addr,
                          info->instance,
                          &raw) == DALI_OK) {
        info->has_resolution = true;
        info->resolution = raw;
    }
    if (query_instance_u8(transport,
                          dali_input_build_query_instance_status,
                          addr,
                          info->instance,
                          &raw) == DALI_OK) {
        info->has_status = true;
        info->status = raw;
    }
    if (query_instance_u8(transport,
                          dali_input_build_query_instance_error,
                          addr,
                          info->instance,
                          &raw) == DALI_OK) {
        info->has_error = true;
        info->error = raw;
    }
}

DaliError dali_discovery_query_input_device(const DaliDiscoveryTransport *transport,
                                            uint8_t addr,
                                            DaliDiscoveryInputDevice *out)
{
    if (!transport_valid(transport) ||
        out == NULL ||
        addr >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    discovery_input_reset(out, addr);

    DaliFrame count_frame;
    DaliError err = dali_input_build_query_number_of_instances(addr, &count_frame);
    if (err != DALI_OK) {
        return err;
    }

    uint8_t count_raw = 0u;
    err = dali_discovery_query_u8(transport, &count_frame, &count_raw);
    if (err != DALI_OK) {
        return err;
    }

    out->device.has_instance_count = true;
    out->device.instance_count = count_raw;

    uint8_t count = dali_discovery_input_visible_instance_count(out);
    for (uint8_t instance = 0u; instance < count; instance++) {
        uint8_t type = 0u;
        DaliError type_err = query_instance_u8(transport,
                                               dali_input_build_query_instance_type,
                                               addr,
                                               instance,
                                               &type);
        out->instance_type_errors[instance] = type_err;
        if (type_err != DALI_OK) {
            continue;
        }

        DaliInputInstanceInfo *info = &out->device.instances[instance];
        DaliError classify_err = dali_input_classify_instance(instance, type, info);
        if (classify_err != DALI_OK) {
            out->instance_type_errors[instance] = classify_err;
            continue;
        }

        query_instance_optional_fields(transport, addr, info);
    }

    return DALI_OK;
}

uint8_t dali_discovery_input_visible_instance_count(
    const DaliDiscoveryInputDevice *input_device)
{
    if (input_device == NULL || !input_device->device.has_instance_count) {
        return 0u;
    }

    uint8_t count = input_device->device.instance_count;
    return count > DALI_INPUT_MAX_INSTANCES ? DALI_INPUT_MAX_INSTANCES : count;
}
