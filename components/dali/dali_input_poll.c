#include "dali_input_poll.h"

#include <string.h>

_Static_assert(DALI_INPUT_POLL_MAX_BYTES <= DALI_SEQUENCE_MAX_STEPS,
               "a full input-value read must fit one scheduler sequence");

uint8_t dali_input_poll_bytes_for_resolution(uint8_t resolution_bits)
{
    if (resolution_bits == 0u) {
        return 1u;
    }

    uint8_t bytes = (uint8_t)((resolution_bits + 7u) / 8u);
    return bytes > DALI_INPUT_POLL_MAX_BYTES ? DALI_INPUT_POLL_MAX_BYTES : bytes;
}

/* Byte 0 latches the reading; later bytes read out of that same latch. */
static DaliCommandId poll_command_for_byte(uint8_t index)
{
    return index == 0u ? DALI_CMD_QUERY_INPUT_VALUE
                       : DALI_CMD_QUERY_INPUT_VALUE_LATCH;
}

DaliError dali_input_poll_build_value_sequence(uint8_t addr,
                                               uint8_t instance,
                                               uint8_t expected_bytes,
                                               DaliSequence *out)
{
    if (out == NULL ||
        addr >= DALI_SHORT_ADDRESS_COUNT ||
        instance >= DALI_INPUT_MAX_INSTANCES ||
        expected_bytes == 0u ||
        expected_bytes > DALI_INPUT_POLL_MAX_BYTES) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    for (uint8_t i = 0u; i < expected_bytes; i++) {
        DaliError err = dali_build_instance_command(addr,
                                                    instance,
                                                    poll_command_for_byte(i),
                                                    &out->steps[i].frame);
        if (err != DALI_OK) {
            return err;
        }
        out->steps[i].needs_reply = true;
    }

    out->step_count = expected_bytes;
    return DALI_OK;
}

DaliError dali_input_poll_value_from_sequence(const DaliSequenceResult *result,
                                              uint8_t expected_bytes,
                                              DaliInputValue *out)
{
    if (result == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliError err = dali_input_value_start(out, expected_bytes);
    if (err != DALI_OK) {
        return err;
    }
    if (result->result != DALI_OK) {
        return result->result;
    }

    for (uint8_t i = 0u; i < expected_bytes; i++) {
        DaliFrame reply;
        if (!dali_sequence_result_reply(result, i, &reply)) {
            return DALI_ERR_MALFORMED;
        }
        err = dali_input_value_push_frame(out, &reply);
        if (err != DALI_OK) {
            return err;
        }
    }

    return out->complete ? DALI_OK : DALI_ERR_MALFORMED;
}

static void poll_record_byte_errors(DaliInputPollResult      *out,
                                    const DaliSequenceResult *result,
                                    uint8_t                   expected_bytes,
                                    DaliError                 run_error)
{
    for (uint8_t i = 0u; i < expected_bytes; i++) {
        DaliFrame reply;
        if (dali_sequence_result_reply(result, i, &reply)) {
            out->byte_errors[i] = reply.bit_length == DALI_BACKWARD_FRAME_BITS
                                ? DALI_OK
                                : DALI_ERR_INVALID;
        } else if (i < result->steps_run) {
            out->byte_errors[i] = DALI_ERR_MALFORMED;
        }
    }

    if (run_error != DALI_OK && result->failed_step < expected_bytes) {
        out->byte_errors[result->failed_step] = run_error;
    }
}

DaliError dali_input_poll_value(const DaliDiscoveryTransport *transport,
                                uint8_t addr,
                                uint8_t instance,
                                uint8_t expected_bytes,
                                DaliInputPollResult *out)
{
    if (!dali_transport_valid(transport) ||
        !dali_transport_supports_atomic_sequence(transport) ||
        out == NULL ||
        addr >= DALI_SHORT_ADDRESS_COUNT ||
        instance >= DALI_INPUT_MAX_INSTANCES ||
        expected_bytes == 0u ||
        expected_bytes > DALI_INPUT_POLL_MAX_BYTES) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->address = addr;
    out->instance = instance;
    out->requested_bytes = expected_bytes;
    for (uint8_t i = 0u; i < DALI_INPUT_POLL_MAX_BYTES; i++) {
        out->byte_errors[i] = DALI_ERR_INVALID;
    }

    DaliError err = dali_input_value_start(&out->value, expected_bytes);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequence seq;
    err = dali_input_poll_build_value_sequence(addr,
                                               instance,
                                               expected_bytes,
                                               &seq);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceResult result;
    err = dali_transport_run_sequence_atomic(transport, &seq, &result);
    poll_record_byte_errors(out, &result, expected_bytes, err);
    if (err != DALI_OK) {
        return err;
    }

    return dali_input_poll_value_from_sequence(&result,
                                               expected_bytes,
                                               &out->value);
}
