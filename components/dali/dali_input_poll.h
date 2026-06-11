#pragma once

/*
 * dali_input_poll.h - reusable DALI-2 input value polling helpers
 */

#include "dali_discovery.h"

typedef struct {
    uint8_t        address;
    uint8_t        instance;
    uint8_t        requested_bytes;
    DaliInputValue value;
    DaliError      byte_errors[4];
} DaliInputPollResult;

uint8_t dali_input_poll_bytes_for_resolution(uint8_t resolution_bits);

DaliError dali_input_poll_value(const DaliDiscoveryTransport *transport,
                                uint8_t addr,
                                uint8_t instance,
                                uint8_t expected_bytes,
                                DaliInputPollResult *out);
