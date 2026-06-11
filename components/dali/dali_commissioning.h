#pragma once

/*
 * dali_commissioning.h - reusable DALI control-gear commissioning helpers
 *
 * This module drives the standard random-address commissioning sequence over
 * an abstract transaction function. It has no ESP-IDF, ESPHome, UART, or task
 * dependency.
 */

#include "dali_discovery.h"

#define DALI_RANDOM_ADDRESS_MAX              0xFFFFFFu
#define DALI_INITIALISE_UNADDRESSED_PARAM    0xFFu
#define DALI_COMMISSIONING_MAX_ASSIGNMENTS   DALI_SHORT_ADDRESS_COUNT
#define DALI_COMMISSIONING_QUERY_RETRIES_LEFT 1u

typedef struct {
    uint32_t random_address;
    uint8_t  short_address;
    bool     has_query_short;
    uint8_t  query_short_raw;
    uint8_t  query_short_address;
} DaliCommissioningAssignment;

typedef struct {
    uint8_t first_short_address;
    uint8_t max_devices;       /* 0 = fill every free short address */
    uint64_t used_address_mask; /* bit N set means short address N is unavailable */
    bool query_short_address;
} DaliCommissioningOptions;

typedef struct {
    uint8_t assigned_count;
    uint8_t free_address_count;
    bool    no_more_devices;
    bool    address_space_full;
    DaliError last_error;
    DaliCommissioningAssignment assignments[DALI_COMMISSIONING_MAX_ASSIGNMENTS];
} DaliCommissioningResult;

typedef enum {
    DALI_COMMISSIONING_EVENT_INITIALISED = 0,
    DALI_COMMISSIONING_EVENT_RANDOMISED,
    DALI_COMMISSIONING_EVENT_SEARCH_FOUND,
    DALI_COMMISSIONING_EVENT_ASSIGNED,
    DALI_COMMISSIONING_EVENT_NO_MORE_DEVICES,
    DALI_COMMISSIONING_EVENT_ADDRESS_SPACE_FULL,
    DALI_COMMISSIONING_EVENT_TERMINATED,
} DaliCommissioningEventKind;

typedef struct {
    DaliCommissioningEventKind kind;
    uint32_t random_address;
    uint8_t  short_address;
    uint8_t  assigned_count;
} DaliCommissioningEvent;

typedef void (*DaliCommissioningProgressCb)(const DaliCommissioningEvent *event,
                                            void *ctx);

uint8_t dali_commissioning_encode_short_address(uint8_t short_address);
DaliError dali_commissioning_decode_short_address(uint8_t encoded,
                                                  uint8_t *short_address_out);

DaliError dali_commissioning_set_search_address(
    const DaliDiscoveryTransport *transport,
    uint32_t random_address);

DaliError dali_commissioning_compare(const DaliDiscoveryTransport *transport,
                                     bool *yes_out);

DaliError dali_commissioning_find_next_random_address(
    const DaliDiscoveryTransport *transport,
    uint32_t *random_address_out,
    bool *found_out);

DaliError dali_commissioning_program_short_address(
    const DaliDiscoveryTransport *transport,
    uint8_t short_address);

DaliError dali_commissioning_verify_short_address(
    const DaliDiscoveryTransport *transport,
    uint8_t short_address,
    bool *verified_out);

DaliError dali_commissioning_query_short_address(
    const DaliDiscoveryTransport *transport,
    uint8_t *encoded_out);

uint64_t dali_commissioning_used_mask_from_inventory(
    const DaliDiscoveryInventory *inventory);

DaliError dali_commissioning_commission_unaddressed(
    const DaliDiscoveryTransport *transport,
    const DaliCommissioningOptions *options,
    DaliCommissioningResult *out,
    DaliCommissioningProgressCb progress_cb,
    void *progress_ctx);
