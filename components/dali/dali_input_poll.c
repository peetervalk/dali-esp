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

static DaliError poll_query_byte(const DaliDiscoveryTransport *transport,
                                 uint8_t addr,
                                 uint8_t instance,
                                 DaliCommandId command,
                                 uint8_t *out)
{
    if (transport == NULL || transport->transact == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliError err = dali_build_instance_command(addr, instance, command, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_discovery_query_u8(transport, &frame, out);
}

DaliError dali_input_poll_value(const DaliDiscoveryTransport *transport,
                                uint8_t addr,
                                uint8_t instance,
                                uint8_t expected_bytes,
                                DaliInputPollResult *out)
{
    if (transport == NULL || transport->transact == NULL ||
        out == NULL ||
        addr >= DALI_SHORT_ADDRESS_COUNT ||
        instance >= DALI_INPUT_MAX_INSTANCES ||
        expected_bytes == 0u ||
        expected_bytes > 4u) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->address = addr;
    out->instance = instance;
    out->requested_bytes = expected_bytes;
    for (uint8_t i = 0u; i < 4u; i++) {
        out->byte_errors[i] = DALI_ERR_INVALID;
    }

    DaliError err = dali_input_value_start(&out->value, expected_bytes);
    if (err != DALI_OK) {
        return err;
    }

    for (uint8_t i = 0u; i < expected_bytes; i++) {
        uint8_t raw = 0u;
        DaliCommandId command = i == 0u
                              ? DALI_CMD_QUERY_INPUT_VALUE
                              : DALI_CMD_QUERY_INPUT_VALUE_LATCH;
        err = poll_query_byte(transport, addr, instance, command, &raw);
        out->byte_errors[i] = err;
        if (err != DALI_OK) {
            return err;
        }

        err = dali_input_value_push(&out->value, raw);
        if (err != DALI_OK) {
            out->byte_errors[i] = err;
            return err;
        }
    }

    return DALI_OK;
}
