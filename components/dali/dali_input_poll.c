#include "dali_input_poll.h"

#include <string.h>

uint8_t dali_input_poll_bytes_for_resolution(uint8_t resolution_bits)
{
    if (resolution_bits == 0u) {
        return 1u;
    }

    uint8_t bytes = (uint8_t)((resolution_bits + 7u) / 8u);
    return bytes > 4u ? 4u : bytes;
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
