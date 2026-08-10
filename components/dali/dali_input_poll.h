#pragma once

/*
 * dali_input_poll.h - reusable DALI-2 input value polling helpers
 */

#include "dali_discovery.h"
#include "dali_scheduler.h"

/* Widest reading DaliInputValue can accumulate. */
#define DALI_INPUT_POLL_MAX_BYTES 4u

typedef struct {
    uint8_t        address;
    uint8_t        instance;
    uint8_t        requested_bytes;
    DaliInputValue value;
    DaliError      byte_errors[DALI_INPUT_POLL_MAX_BYTES];
} DaliInputPollResult;

uint8_t dali_input_poll_bytes_for_resolution(uint8_t resolution_bits);

DaliError dali_input_poll_value(const DaliDiscoveryTransport *transport,
                                uint8_t addr,
                                uint8_t instance,
                                uint8_t expected_bytes,
                                DaliInputPollResult *out);

/*
 * Build a multi-byte input-value read as one scheduler sequence.
 *
 * QUERY INPUT VALUE latches the instance's current value and returns its most
 * significant byte; each following QUERY INPUT VALUE LATCH returns the next
 * byte of that same latched reading. Enqueuing them as a single sequence keeps
 * the bytes of one reading from being separated by other bus traffic, and makes
 * admission all-or-nothing so a partial read cannot be left queued.
 *
 * Steps carry no retry budget, matching the single-shot polling callers use.
 */
DaliError dali_input_poll_build_value_sequence(uint8_t addr,
                                               uint8_t instance,
                                               uint8_t expected_bytes,
                                               DaliSequence *out);

/*
 * Assemble the reading from a sequence built by
 * dali_input_poll_build_value_sequence(). Every step must have produced a
 * backward frame, so a partially executed sequence never yields a half-formed
 * value. A failed sequence returns its own error.
 */
DaliError dali_input_poll_value_from_sequence(const DaliSequenceResult *result,
                                              uint8_t expected_bytes,
                                              DaliInputValue *out);
