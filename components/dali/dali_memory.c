#include "dali_memory.h"
#include <string.h>

#define BANK0_IDENTITY_INDEX(offset) \
    ((uint8_t)((offset) - DALI_MEMORY_BANK0_IDENTITY_FIRST))

#define CONTROL_DEVICE_ENABLE_WRITE_MEMORY_OPCODE 0x15u
#define MEMORY_BANK_LOCK_OFFSET                   0x02u
#define MEMORY_BANK_UNLOCK_VALUE                  0x55u

_Static_assert(DALI_SEQUENCE_MAX_STEPS >= DALI_MEMORY_CONTROL_DEVICE_WRITE_STEPS,
               "scheduler sequence capacity must fit a control-device memory write");

DaliFrame dali_memory_build_dtr1_bank(uint8_t bank)
{
    return dali_cmd_dtr1_data(bank);
}

DaliFrame dali_memory_build_dtr0_offset(uint8_t offset)
{
    return dali_cmd_dtr0_data(offset);
}

DaliFrame dali_memory_build_read(uint8_t short_addr)
{
    DaliFrame frame = {0u, 0u};
    if (short_addr >= DALI_SHORT_ADDRESS_COUNT) {
        return frame;
    }
    dali_build_command(DALI_ADDR_SHORT, short_addr,
                       DALI_CMD_READ_MEMORY_LOCATION, 0u, &frame);
    return frame;
}

DaliError dali_memory_build_control_device_write_sequence(uint8_t short_addr,
                                                          uint8_t bank,
                                                          uint8_t offset,
                                                          uint8_t value,
                                                          DaliSequence *out)
{
    if (out == NULL ||
        short_addr >= DALI_SHORT_ADDRESS_COUNT ||
        bank == DALI_MEMORY_BANK0) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    DaliFrame enable_write = dali_cmd_device(
        short_addr, CONTROL_DEVICE_ENABLE_WRITE_MEMORY_OPCODE);

    out->steps[0].frame = dali_cmd_control_device_dtr1_data(bank);
    out->steps[1].frame = dali_cmd_control_device_dtr0_data(MEMORY_BANK_LOCK_OFFSET);
    out->steps[2].frame = enable_write;
    out->steps[2].send_twice = true;
    out->steps[3].frame =
        dali_cmd_control_device_write_memory_location_no_reply(MEMORY_BANK_UNLOCK_VALUE);
    out->steps[4].frame = dali_cmd_control_device_dtr0_data(offset);
    out->steps[5].frame = enable_write;
    out->steps[5].send_twice = true;
    out->steps[6].frame =
        dali_cmd_control_device_write_memory_location_no_reply(value);
    out->step_count = DALI_MEMORY_CONTROL_DEVICE_WRITE_STEPS;
    return DALI_OK;
}

static bool mem_transport_valid(const DaliMemoryTransport *transport)
{
    return transport != NULL && transport->transact != NULL;
}

static DaliError transact_no_reply(const DaliMemoryTransport *transport,
                                   const DaliFrame *frame)
{
    return transport->transact(frame, false, 0u, false, NULL, transport->ctx);
}

static DaliError transact_read(const DaliMemoryTransport *transport,
                               const DaliFrame *frame,
                               uint8_t *out)
{
    DaliFrame reply = {0u, 0u};
    DaliError err = transport->transact(frame, true, DALI_MEMORY_QUERY_RETRIES,
                                        false, &reply, transport->ctx);
    if (err != DALI_OK) {
        return err;
    }
    if (reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_MALFORMED;
    }
    *out = (uint8_t)(reply.data & 0xFFu);
    return DALI_OK;
}

DaliError dali_memory_read_byte(const DaliMemoryTransport *transport,
                                uint8_t short_addr,
                                uint8_t bank,
                                uint8_t offset,
                                uint8_t *out)
{
    if (!mem_transport_valid(transport) ||
        short_addr >= DALI_SHORT_ADDRESS_COUNT ||
        out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame dtr1 = dali_memory_build_dtr1_bank(bank);
    DaliError err = transact_no_reply(transport, &dtr1);
    if (err != DALI_OK) {
        return err;
    }

    DaliFrame dtr0 = dali_memory_build_dtr0_offset(offset);
    err = transact_no_reply(transport, &dtr0);
    if (err != DALI_OK) {
        return err;
    }

    DaliFrame read = dali_memory_build_read(short_addr);
    return transact_read(transport, &read, out);
}

DaliError dali_memory_read_bytes(const DaliMemoryTransport *transport,
                                 uint8_t short_addr,
                                 uint8_t bank,
                                 uint8_t offset,
                                 uint8_t *buf,
                                 uint8_t count)
{
    if (!mem_transport_valid(transport) ||
        short_addr >= DALI_SHORT_ADDRESS_COUNT ||
        buf == NULL) {
        return DALI_ERR_INVALID;
    }
    if (count == 0u) {
        return DALI_OK;
    }

    DaliFrame dtr1 = dali_memory_build_dtr1_bank(bank);
    DaliError err = transact_no_reply(transport, &dtr1);
    if (err != DALI_OK) {
        return err;
    }

    DaliFrame dtr0 = dali_memory_build_dtr0_offset(offset);
    err = transact_no_reply(transport, &dtr0);
    if (err != DALI_OK) {
        return err;
    }

    DaliFrame read = dali_memory_build_read(short_addr);
    for (uint8_t i = 0u; i < count; i++) {
        err = transact_read(transport, &read, &buf[i]);
        if (err != DALI_OK) {
            return err;
        }
    }
    return DALI_OK;
}

DaliError dali_memory_read_bank0_identity(const DaliMemoryTransport *transport,
                                          uint8_t short_addr,
                                          DaliMemoryBank0Identity *out)
{
    if (!mem_transport_valid(transport) || out == NULL) {
        return DALI_ERR_INVALID;
    }

    uint8_t raw[DALI_MEMORY_BANK0_IDENTITY_SIZE];
    DaliError err = dali_memory_read_bytes(transport, short_addr,
                                           DALI_MEMORY_BANK0,
                                           DALI_MEMORY_BANK0_IDENTITY_FIRST,
                                           raw,
                                           DALI_MEMORY_BANK0_IDENTITY_SIZE);
    if (err != DALI_OK) {
        return err;
    }

    memcpy(out->gtin,
           &raw[BANK0_IDENTITY_INDEX(DALI_MEMORY_BANK0_OFFSET_GTIN)],
           DALI_MEMORY_BANK0_GTIN_LEN);
    out->fw_major = raw[BANK0_IDENTITY_INDEX(DALI_MEMORY_BANK0_OFFSET_FW_MAJOR)];
    out->fw_minor = raw[BANK0_IDENTITY_INDEX(DALI_MEMORY_BANK0_OFFSET_FW_MINOR)];
    memcpy(out->serial,
           &raw[BANK0_IDENTITY_INDEX(DALI_MEMORY_BANK0_OFFSET_IDENTIFICATION)],
           DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
    out->hw_major = raw[BANK0_IDENTITY_INDEX(DALI_MEMORY_BANK0_OFFSET_HW_MAJOR)];
    out->hw_minor = raw[BANK0_IDENTITY_INDEX(DALI_MEMORY_BANK0_OFFSET_HW_MINOR)];
    return DALI_OK;
}
