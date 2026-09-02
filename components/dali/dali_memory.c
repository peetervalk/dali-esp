#include "dali_memory.h"
#include <string.h>

#define BANK0_IDENTITY_INDEX(offset) \
    ((uint8_t)((offset) - DALI_MEMORY_BANK0_IDENTITY_FIRST))

#define CONTROL_DEVICE_ENABLE_WRITE_MEMORY_OPCODE 0x15u
#define CONTROL_DEVICE_READ_MEMORY_OPCODE         0x3Cu
#define MEMORY_BANK_LOCK_OFFSET                   0x02u
#define MEMORY_BANK_UNLOCK_VALUE                  0x55u

_Static_assert(DALI_SEQUENCE_MAX_STEPS >= DALI_MEMORY_CONTROL_DEVICE_WRITE_STEPS,
               "scheduler sequence capacity must fit a control-device memory write");
_Static_assert(DALI_MEMORY_MAX_SEQUENCE_READ_BYTES >= 1u,
               "a sequence must fit its DTR setup plus at least one read");

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

static DaliError memory_build_read_sequence(uint8_t short_addr,
                                            uint8_t bank,
                                            uint8_t offset,
                                            uint8_t count,
                                            bool control_device,
                                            DaliSequence *out)
{
    if (out == NULL ||
        short_addr >= DALI_SHORT_ADDRESS_COUNT ||
        count == 0u ||
        count > DALI_MEMORY_MAX_SEQUENCE_READ_BYTES) {
        return DALI_ERR_INVALID;
    }

    DaliFrame dtr1;
    DaliFrame dtr0;
    DaliFrame read;

    if (control_device) {
        dtr1 = dali_cmd_control_device_dtr1_data(bank);
        dtr0 = dali_cmd_control_device_dtr0_data(offset);
        read = dali_cmd_device(short_addr, CONTROL_DEVICE_READ_MEMORY_OPCODE);
    } else {
        dtr1 = dali_memory_build_dtr1_bank(bank);
        dtr0 = dali_memory_build_dtr0_offset(offset);
        read = dali_memory_build_read(short_addr);
    }

    if (dtr1.bit_length == 0u || dtr0.bit_length == 0u || read.bit_length == 0u) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->steps[0].frame = dtr1;
    out->steps[1].frame = dtr0;

    /* No retries: a repeated READ would return the following location because
     * the device has already advanced DTR0. Retry the whole sequence instead. */
    for (uint8_t i = 0u; i < count; i++) {
        DaliSequenceStep *step = &out->steps[DALI_MEMORY_READ_SETUP_STEPS + i];
        step->frame       = read;
        step->needs_reply = true;
    }

    out->step_count = (uint8_t)(DALI_MEMORY_READ_SETUP_STEPS + count);
    return DALI_OK;
}

DaliError dali_memory_build_read_sequence(uint8_t short_addr,
                                          uint8_t bank,
                                          uint8_t offset,
                                          uint8_t count,
                                          DaliSequence *out)
{
    return memory_build_read_sequence(short_addr, bank, offset, count, false, out);
}

DaliError dali_memory_build_control_device_read_sequence(uint8_t short_addr,
                                                         uint8_t bank,
                                                         uint8_t offset,
                                                         uint8_t count,
                                                         DaliSequence *out)
{
    return memory_build_read_sequence(short_addr, bank, offset, count, true, out);
}

DaliError dali_memory_read_from_sequence(const DaliSequenceResult *result,
                                         uint8_t                   count,
                                         uint8_t                  *buf)
{
    if (result == NULL || buf == NULL ||
        count == 0u ||
        count > DALI_MEMORY_MAX_SEQUENCE_READ_BYTES) {
        return DALI_ERR_INVALID;
    }
    if (result->result != DALI_OK) {
        return result->result;
    }

    for (uint8_t i = 0u; i < count; i++) {
        DaliFrame reply;
        uint8_t   step = (uint8_t)(DALI_MEMORY_READ_SETUP_STEPS + i);
        if (!dali_sequence_result_reply(result, step, &reply) ||
            reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
            return DALI_ERR_MALFORMED;
        }
        buf[i] = (uint8_t)(reply.data & 0xFFu);
    }

    return DALI_OK;
}

DaliError dali_memory_read_byte(const DaliMemoryTransport *transport,
                                uint8_t short_addr,
                                uint8_t bank,
                                uint8_t offset,
                                uint8_t *out)
{
    return dali_memory_read_bytes(transport, short_addr, bank, offset, out, 1u);
}

/*
 * Shared block read. `control_device` selects the Part 103 24-bit framing over
 * the Part 102 16-bit form; everything else about the two is identical,
 * including the two DTR setup steps dali_memory_read_from_sequence() skips past.
 * One loop rather than two, so a fix to the chunking cannot reach only one
 * address space.
 */
static DaliError memory_read_bytes(const DaliMemoryTransport *transport,
                                   uint8_t short_addr,
                                   uint8_t bank,
                                   uint8_t offset,
                                   uint8_t *buf,
                                   uint8_t count,
                                   bool    control_device)
{
    if (!dali_transport_valid(transport) ||
        short_addr >= DALI_SHORT_ADDRESS_COUNT ||
        buf == NULL) {
        return DALI_ERR_INVALID;
    }
    if (count == 0u) {
        return DALI_OK;
    }
    /* The block must stay inside the 8-bit address space DTR0 can express. */
    if ((uint32_t)offset + (uint32_t)count > 256u) {
        return DALI_ERR_INVALID;
    }

    /* Read in sequence-sized chunks. Each chunk re-issues its own DTR1/DTR0, so
     * a chunk boundary re-establishes the offset instead of trusting that the
     * device's auto-increment survived whatever ran in between. */
    uint8_t done = 0u;
    while (done < count) {
        uint8_t chunk = (uint8_t)(count - done);
        if (chunk > DALI_MEMORY_MAX_SEQUENCE_READ_BYTES) {
            chunk = DALI_MEMORY_MAX_SEQUENCE_READ_BYTES;
        }

        DaliSequence seq;
        DaliError err = control_device
            ? dali_memory_build_control_device_read_sequence(
                  short_addr, bank, (uint8_t)(offset + done), chunk, &seq)
            : dali_memory_build_read_sequence(
                  short_addr, bank, (uint8_t)(offset + done), chunk, &seq);
        if (err != DALI_OK) {
            return err;
        }

        DaliSequenceResult result;
        err = dali_transport_run_sequence_atomic(transport, &seq, &result);
        if (err != DALI_OK) {
            return err;
        }

        err = dali_memory_read_from_sequence(&result, chunk, &buf[done]);
        if (err != DALI_OK) {
            return err;
        }

        done = (uint8_t)(done + chunk);
    }

    return DALI_OK;
}

DaliError dali_memory_read_bytes(const DaliMemoryTransport *transport,
                                 uint8_t short_addr,
                                 uint8_t bank,
                                 uint8_t offset,
                                 uint8_t *buf,
                                 uint8_t count)
{
    return memory_read_bytes(transport, short_addr, bank, offset, buf, count, false);
}

DaliError dali_memory_read_device_bytes(const DaliMemoryTransport *transport,
                                        uint8_t short_addr,
                                        uint8_t bank,
                                        uint8_t offset,
                                        uint8_t *buf,
                                        uint8_t count)
{
    return memory_read_bytes(transport, short_addr, bank, offset, buf, count, true);
}

/* Bank 0 has one layout, whichever space it was read from. */
static void memory_unpack_bank0_identity(const uint8_t           *raw,
                                         DaliMemoryBank0Identity *out)
{
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
}

DaliError dali_memory_read_bank0_identity(const DaliMemoryTransport *transport,
                                          uint8_t short_addr,
                                          DaliMemoryBank0Identity *out)
{
    if (!dali_transport_valid(transport) || out == NULL) {
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

    memory_unpack_bank0_identity(raw, out);
    return DALI_OK;
}

DaliError dali_memory_read_device_bank0_identity(const DaliMemoryTransport *transport,
                                                 uint8_t short_addr,
                                                 DaliMemoryBank0Identity *out)
{
    if (!dali_transport_valid(transport) || out == NULL) {
        return DALI_ERR_INVALID;
    }

    uint8_t raw[DALI_MEMORY_BANK0_IDENTITY_SIZE];
    DaliError err = dali_memory_read_device_bytes(transport, short_addr,
                                                  DALI_MEMORY_BANK0,
                                                  DALI_MEMORY_BANK0_IDENTITY_FIRST,
                                                  raw,
                                                  DALI_MEMORY_BANK0_IDENTITY_SIZE);
    if (err != DALI_OK) {
        return err;
    }

    memory_unpack_bank0_identity(raw, out);
    return DALI_OK;
}
