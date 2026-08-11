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

/* The shared DaliTransport under its discovery-era names. Discovery, memory,
 * commissioning, and input polling all take the same struct, so one transport
 * value serves every module without conversion. */
typedef DaliTransactionFn DaliDiscoveryTransactionFn;
typedef DaliTransport     DaliDiscoveryTransport;

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
    uint8_t                  device_type;   /* legacy first/lowest type; same as device_types[0] */
    uint8_t                  device_type_count;
    uint8_t                  device_types[DALI_DISCOVERY_MAX_DEVICE_TYPES];
    bool                     has_version;
    uint8_t                  version;
    bool                     has_actual_level;
    uint8_t                  actual_level;
    bool                     has_control_gear;  /* true if QUERY STATUS responded (Part 209) */
    bool                     has_input_device;
    bool                     has_instance_count;
    uint8_t                  instance_count;
    bool                     has_identity;  /* Part 102 control-gear Bank 0 */
    DaliMemoryBank0Identity  identity;
    bool                     has_scene_levels;
    uint8_t                  scene_levels[DALI_SCENE_COUNT];
    bool                     has_dt6;
    uint8_t                  dt6_failure_status;
    uint8_t                  dt6_features;
    bool                     has_dt8;
    uint8_t                  dt8_gear_features;
    uint8_t                  dt8_colour_status;
    uint8_t                  dt8_colour_type_features;
    /* True only when a valid fifth type was observed; false can still mean a
     * partial list if enumeration ended on timeout or a malformed reply. */
    bool                     device_types_truncated;
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

/*
 * True when at least one control gear was positively discovered and every
 * present control-gear record has a complete 16-group observation. Pure input
 * devices do not implement control-gear group queries and are deliberately
 * ignored. An empty/all-timeout scan is intentionally not authoritative,
 * because it cannot be distinguished from a disconnected bus.
 */
bool dali_discovery_inventory_has_complete_group_data(
    const DaliDiscoveryInventory *inventory);

/* ---------------------------------------------------------------------------
 * Sequenced queries
 *
 * Some discovery answers depend on the frame that precedes them, so the frames
 * must stay together relative to locally scheduled traffic. Those are built as
 * a DaliSequence and run through dali_transport_run_sequence_atomic(): no other
 * local transaction can be interleaved, and a frame-only transport is rejected
 * before a dependent group is issued. A separate physical bus master can still
 * interpose; that requires bus-level arbitration beyond this transport API.
 *
 * A step that advances device-side state carries no retry budget, because a
 * lone retransmission would be answered out of step with the rest of the
 * sequence. Retry the whole sequence instead.
 * --------------------------------------------------------------------------*/

/* ENABLE DEVICE TYPE n, then one query answered under that type. */
#define DALI_DISCOVERY_DT_SEQUENCE_STEPS 2u
#define DALI_DISCOVERY_DT_STEP_ENABLE    0u
#define DALI_DISCOVERY_DT_STEP_QUERY     1u

/* QUERY GROUPS 0-7, then QUERY GROUPS 8-15. */
#define DALI_DISCOVERY_GROUPS_SEQUENCE_STEPS 2u
#define DALI_DISCOVERY_GROUPS_STEP_0_7       0u
#define DALI_DISCOVERY_GROUPS_STEP_8_15      1u

/*
 * QUERY DEVICE TYPE, then one QUERY NEXT DEVICE TYPE per type the device may
 * report. One extra NEXT step beyond DALI_DISCOVERY_MAX_DEVICE_TYPES exists so
 * a device reporting more types than fit can be reported as truncated rather
 * than silently clipped.
 */
#define DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS (DALI_DISCOVERY_MAX_DEVICE_TYPES + 1u)
#define DALI_DISCOVERY_DEVICE_TYPES_SEQUENCE_STEPS \
    (1u + DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS)
#define DALI_DISCOVERY_DEVICE_TYPES_STEP_FIRST 0u

/*
 * Build ENABLE DEVICE TYPE `device_type` followed by `query`. `query` must be a
 * 16-bit forward frame. The query step takes no retries: ENABLE DEVICE TYPE
 * applies only to the command immediately following it.
 */
DaliError dali_discovery_build_device_type_query_sequence(uint8_t device_type,
                                                          const DaliFrame *query,
                                                          DaliSequence *out);

/* Build the two group queries. Both steps are idempotent, so both may retry. */
DaliError dali_discovery_build_groups_sequence(uint8_t addr, DaliSequence *out);

/*
 * Build the multi-type enumeration. Only worth running once a plain
 * QUERY DEVICE TYPE has answered "multiple"; the sequence re-issues that query
 * as its first step so the answer sequence restarts inside the atomic block.
 * No step retries: QUERY NEXT DEVICE TYPE advances the device's enumeration.
 */
DaliError dali_discovery_build_device_types_sequence(uint8_t addr,
                                                     DaliSequence *out);

/* Copy the 8-bit answer captured for `step`, or report why there is none. */
DaliError dali_discovery_u8_from_sequence(const DaliSequenceResult *result,
                                          uint8_t step,
                                          uint8_t *out);

/* Assemble the 16-bit group mask from a completed group sequence. */
DaliError dali_discovery_groups_from_sequence(const DaliSequenceResult *result,
                                              uint16_t *groups_out);

/*
 * Store every type the enumeration reported into `device`. Types collected
 * before a failing step are kept, so a sequence that aborted part-way still
 * contributes what it had. Returns DALI_ERR_MALFORMED when no type was stored.
 */
DaliError dali_discovery_device_types_from_sequence(const DaliSequenceResult *result,
                                                    DaliDiscoveryDeviceInfo *device);

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
