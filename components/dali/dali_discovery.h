#pragma once

/*
 * dali_discovery.h - reusable DALI bus discovery helpers
 *
 * This module coordinates read-only discovery queries over an abstract
 * transaction function. It has no ESP-IDF, ESPHome, UART, or task dependency.
 */

#include "dali_control.h"
#include "dali_input_device.h"
#include "dali_memory.h"

#define DALI_DISCOVERY_MAX_DEVICE_TYPES  4u

typedef DaliError (*DaliDiscoveryTransactionFn)(const DaliFrame *frame,
                                                bool needs_reply,
                                                uint8_t retries_left,
                                                bool send_twice,
                                                DaliFrame *reply_out,
                                                void *ctx);

typedef struct {
    DaliDiscoveryTransactionFn transact;
    void                      *ctx;
} DaliDiscoveryTransport;

typedef struct {
    DaliInputDeviceInfo device;
    DaliError           instance_type_errors[DALI_INPUT_MAX_INSTANCES];
} DaliDiscoveryInputDevice;

typedef struct {
    bool                     present;
    bool                     has_status;
    uint8_t                  status;
    bool                     has_groups;
    uint16_t                 groups;        /* bitmask: bit N set means member of group N */
    bool                     has_device_type;
    uint8_t                  device_type;           /* primary — same as device_types[0] */
    uint8_t                  device_type_count;
    uint8_t                  device_types[DALI_DISCOVERY_MAX_DEVICE_TYPES];
    bool                     has_version;
    uint8_t                  version;
    bool                     has_actual_level;
    uint8_t                  actual_level;
    bool                     has_input_device;
    bool                     has_instance_count;
    uint8_t                  instance_count;
    bool                     has_identity;
    DaliMemoryBank0Identity  identity;
    bool                     has_bank1;
    DaliMemoryBank1Identity  bank1;
    bool                     has_scene_levels;
    uint8_t                  scene_levels[DALI_SCENE_COUNT];
    bool                     has_dt6;
    uint8_t                  dt6_failure_status;
    uint8_t                  dt6_features;
    bool                     has_dt8;
    uint8_t                  dt8_gear_features;
    uint8_t                  dt8_colour_status;
    uint8_t                  dt8_colour_type_features;
} DaliDiscoveryDeviceInfo;

typedef struct {
    bool                    valid;
    uint8_t                 found_count;
    DaliDiscoveryDeviceInfo devices[DALI_SHORT_ADDRESS_COUNT];
} DaliDiscoveryInventory;

typedef void (*DaliDiscoveryFoundCb)(uint8_t addr,
                                     const DaliDiscoveryDeviceInfo *device,
                                     void *ctx);

/* Return true if device has the given device type in its type list. */
bool dali_discovery_has_device_type(const DaliDiscoveryDeviceInfo *device, uint8_t type);

DaliError dali_discovery_inventory_reset(DaliDiscoveryInventory *inventory);
const DaliDiscoveryDeviceInfo *dali_discovery_inventory_get(
    const DaliDiscoveryInventory *inventory,
    uint8_t addr);

DaliError dali_discovery_inventory_store_status(DaliDiscoveryInventory *inventory,
                                                uint8_t addr,
                                                uint8_t status);
DaliError dali_discovery_inventory_store_groups(DaliDiscoveryInventory *inventory,
                                                uint8_t addr,
                                                uint16_t groups);
DaliError dali_discovery_inventory_update_input_device(
    DaliDiscoveryInventory *inventory,
    const DaliDiscoveryInputDevice *input_device);

DaliError dali_discovery_query_u8(const DaliDiscoveryTransport *transport,
                                  const DaliFrame *frame,
                                  uint8_t *out);
DaliError dali_discovery_query_status(const DaliDiscoveryTransport *transport,
                                      uint8_t addr,
                                      uint8_t *status_out);
DaliError dali_discovery_query_groups(const DaliDiscoveryTransport *transport,
                                      uint8_t addr,
                                      uint16_t *groups_out);
DaliError dali_discovery_query_device_type(const DaliDiscoveryTransport *transport,
                                           uint8_t addr,
                                           uint8_t *type_out);
DaliError dali_discovery_query_version(const DaliDiscoveryTransport *transport,
                                       uint8_t addr,
                                       uint8_t *version_out);
DaliError dali_discovery_query_actual_level(const DaliDiscoveryTransport *transport,
                                            uint8_t addr,
                                            uint8_t *level_out);
const char *dali_discovery_device_type_name(uint8_t type);
DaliError dali_discovery_scan(DaliDiscoveryInventory *inventory,
                              const DaliDiscoveryTransport *transport,
                              DaliDiscoveryFoundCb found_cb,
                              void *found_ctx,
                              uint8_t *found_out);

DaliError dali_discovery_query_input_device(const DaliDiscoveryTransport *transport,
                                            uint8_t addr,
                                            DaliDiscoveryInputDevice *out);
uint8_t dali_discovery_input_visible_instance_count(
    const DaliDiscoveryInputDevice *input_device);
