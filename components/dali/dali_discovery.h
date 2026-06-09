#pragma once

/*
 * dali_discovery.h - reusable DALI bus discovery helpers
 *
 * This module coordinates read-only discovery queries over an abstract
 * transaction function. It has no ESP-IDF, ESPHome, UART, or task dependency.
 */

#include "dali_control.h"
#include "dali_input_device.h"

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
    bool                     has_input_device;
    bool                     has_instance_count;
    uint8_t                  instance_count;
} DaliDiscoveryDeviceInfo;

typedef struct {
    bool                    valid;
    uint8_t                 found_count;
    DaliDiscoveryDeviceInfo devices[DALI_SHORT_ADDRESS_COUNT];
} DaliDiscoveryInventory;

typedef void (*DaliDiscoveryFoundCb)(uint8_t addr,
                                     const DaliDiscoveryDeviceInfo *device,
                                     void *ctx);

DaliError dali_discovery_inventory_reset(DaliDiscoveryInventory *inventory);
const DaliDiscoveryDeviceInfo *dali_discovery_inventory_get(
    const DaliDiscoveryInventory *inventory,
    uint8_t addr);

DaliError dali_discovery_inventory_store_status(DaliDiscoveryInventory *inventory,
                                                uint8_t addr,
                                                uint8_t status);
DaliError dali_discovery_inventory_update_input_device(
    DaliDiscoveryInventory *inventory,
    const DaliDiscoveryInputDevice *input_device);

DaliError dali_discovery_query_u8(const DaliDiscoveryTransport *transport,
                                  const DaliFrame *frame,
                                  uint8_t *out);
DaliError dali_discovery_query_status(const DaliDiscoveryTransport *transport,
                                      uint8_t addr,
                                      uint8_t *status_out);
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
