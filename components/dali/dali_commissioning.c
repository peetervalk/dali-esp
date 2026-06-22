#include "dali_commissioning.h"

#include <string.h>

#ifndef DALI_HOST_BUILD
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define DALI_RANDOM_ADDRESS_LIMIT (DALI_RANDOM_ADDRESS_MAX + 1u)

static bool comm_transport_valid(const DaliDiscoveryTransport *transport)
{
    return transport != NULL && transport->transact != NULL;
}

static DaliError transact_frame(const DaliDiscoveryTransport *transport,
                                const DaliFrame *frame,
                                bool needs_reply,
                                uint8_t retries_left,
                                bool send_twice,
                                DaliFrame *reply_out)
{
    if (!comm_transport_valid(transport) || frame == NULL) {
        return DALI_ERR_INVALID;
    }

    return transport->transact(frame,
                               needs_reply,
                               retries_left,
                               send_twice,
                               reply_out,
                               transport->ctx);
}

static DaliError send_special_no_reply(const DaliDiscoveryTransport *transport,
                                       DaliCommandId id,
                                       uint8_t param,
                                       bool send_twice)
{
    DaliFrame frame;
    DaliError err = dali_build_special(id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return transact_frame(transport, &frame, false, 0u, send_twice, NULL);
}

static DaliError query_special_u8(const DaliDiscoveryTransport *transport,
                                  DaliCommandId id,
                                  uint8_t param,
                                  uint8_t *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliFrame reply = {0u, 0u};
    DaliError err = dali_build_special(id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }

    err = transact_frame(transport,
                         &frame,
                         true,
                         DALI_COMMISSIONING_QUERY_RETRIES_LEFT,
                         false,
                         &reply);
    if (err != DALI_OK) {
        return err;
    }
    if (reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_MALFORMED;
    }

    *out = (uint8_t)(reply.data & 0xFFu);
    return DALI_OK;
}

static void emit_progress(DaliCommissioningProgressCb cb,
                          void *ctx,
                          DaliCommissioningEventKind kind,
                          uint32_t random_address,
                          uint8_t short_address,
                          uint8_t assigned_count)
{
    if (cb == NULL) {
        return;
    }

    DaliCommissioningEvent event = {
        .kind = kind,
        .random_address = random_address,
        .short_address = short_address,
        .assigned_count = assigned_count,
    };
    cb(&event, ctx);
}

uint8_t dali_commissioning_encode_short_address(uint8_t short_address)
{
    return (uint8_t)(((short_address & DALI_MAX_SHORT_ADDRESS) << 1u) | 0x01u);
}

DaliError dali_commissioning_decode_short_address(uint8_t encoded,
                                                  uint8_t *short_address_out)
{
    if (short_address_out == NULL ||
        encoded == 0xFFu ||
        (encoded & 0x01u) == 0u ||
        (encoded >> 1u) >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    *short_address_out = (uint8_t)(encoded >> 1u);
    return DALI_OK;
}

DaliError dali_commissioning_set_search_address(
    const DaliDiscoveryTransport *transport,
    uint32_t random_address)
{
    if (!comm_transport_valid(transport) || random_address > DALI_RANDOM_ADDRESS_MAX) {
        return DALI_ERR_INVALID;
    }

    DaliError err = send_special_no_reply(transport,
                                          DALI_CMD_SEARCH_ADDRH,
                                          (uint8_t)((random_address >> 16u) & 0xFFu),
                                          false);
    if (err != DALI_OK) {
        return err;
    }

    err = send_special_no_reply(transport,
                                DALI_CMD_SEARCH_ADDRM,
                                (uint8_t)((random_address >> 8u) & 0xFFu),
                                false);
    if (err != DALI_OK) {
        return err;
    }

    return send_special_no_reply(transport,
                                 DALI_CMD_SEARCH_ADDRL,
                                 (uint8_t)(random_address & 0xFFu),
                                 false);
}

DaliError dali_commissioning_compare(const DaliDiscoveryTransport *transport,
                                     bool *yes_out)
{
    if (!comm_transport_valid(transport) || yes_out == NULL) {
        return DALI_ERR_INVALID;
    }

    uint8_t raw = 0u;
    DaliError err = query_special_u8(transport, DALI_CMD_COMPARE, 0u, &raw);
    if (err == DALI_ERR_TIMEOUT) {
        *yes_out = false;
        return DALI_OK;
    }
    if (err != DALI_OK) {
        return err;
    }

    *yes_out = dali_is_yes(raw);
    return DALI_OK;
}

DaliError dali_commissioning_find_next_random_address(
    const DaliDiscoveryTransport *transport,
    uint32_t *random_address_out,
    bool *found_out)
{
    if (!comm_transport_valid(transport) ||
        random_address_out == NULL ||
        found_out == NULL) {
        return DALI_ERR_INVALID;
    }

    uint32_t low = 0u;
    uint32_t high = DALI_RANDOM_ADDRESS_MAX;

    while (low < high) {
        uint32_t mid = low + ((high - low) / 2u);
        DaliError err = dali_commissioning_set_search_address(transport, mid);
        if (err != DALI_OK) {
            return err;
        }

        bool yes = false;
        err = dali_commissioning_compare(transport, &yes);
        if (err != DALI_OK) {
            return err;
        }

        if (yes) {
            high = mid;
        } else {
            low = mid + 1u;
        }
    }

    DaliError err = dali_commissioning_set_search_address(transport, low);
    if (err != DALI_OK) {
        return err;
    }

    bool yes = false;
    err = dali_commissioning_compare(transport, &yes);
    if (err != DALI_OK) {
        return err;
    }

    *random_address_out = low;
    *found_out = yes;
    return DALI_OK;
}

DaliError dali_commissioning_program_short_address(
    const DaliDiscoveryTransport *transport,
    uint8_t short_address)
{
    if (!comm_transport_valid(transport) || short_address >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    return send_special_no_reply(transport,
                                 DALI_CMD_PROGRAM_SHORT_ADDRESS,
                                 dali_commissioning_encode_short_address(short_address),
                                 false);
}

DaliError dali_commissioning_verify_short_address(
    const DaliDiscoveryTransport *transport,
    uint8_t short_address,
    bool *verified_out)
{
    if (!comm_transport_valid(transport) ||
        short_address >= DALI_SHORT_ADDRESS_COUNT ||
        verified_out == NULL) {
        return DALI_ERR_INVALID;
    }

    uint8_t raw = 0u;
    DaliError err = query_special_u8(
        transport,
        DALI_CMD_VERIFY_SHORT_ADDRESS,
        dali_commissioning_encode_short_address(short_address),
        &raw);
    if (err == DALI_ERR_TIMEOUT) {
        *verified_out = false;
        return DALI_OK;
    }
    if (err != DALI_OK) {
        return err;
    }

    *verified_out = dali_is_yes(raw);
    return DALI_OK;
}

DaliError dali_commissioning_query_short_address(
    const DaliDiscoveryTransport *transport,
    uint8_t *encoded_out)
{
    if (!comm_transport_valid(transport) || encoded_out == NULL) {
        return DALI_ERR_INVALID;
    }

    return query_special_u8(transport, DALI_CMD_QUERY_SHORT_ADDRESS, 0u, encoded_out);
}

uint64_t dali_commissioning_used_mask_from_inventory(
    const DaliDiscoveryInventory *inventory)
{
    if (inventory == NULL) {
        return 0u;
    }

    uint64_t mask = 0u;
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device =
            dali_discovery_inventory_get(inventory, addr);
        if (device != NULL && device->present) {
            mask |= ((uint64_t)1u << addr);
        }
    }
    return mask;
}

static bool address_used(uint64_t mask, uint8_t address)
{
    return (mask & ((uint64_t)1u << address)) != 0u;
}

static bool allocate_next_address(uint64_t used_mask,
                                  uint8_t first_address,
                                  uint8_t *address_out)
{
    if (address_out == NULL || first_address >= DALI_SHORT_ADDRESS_COUNT) {
        return false;
    }

    for (uint8_t addr = first_address; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        if (!address_used(used_mask, addr)) {
            *address_out = addr;
            return true;
        }
    }
    return false;
}

static uint8_t count_free_addresses(uint64_t used_mask, uint8_t first_address)
{
    if (first_address >= DALI_SHORT_ADDRESS_COUNT) {
        return 0u;
    }

    uint8_t count = 0u;
    for (uint8_t addr = first_address; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        if (!address_used(used_mask, addr)) {
            count++;
        }
    }
    return count;
}

static DaliError commissioning_start_unaddressed(
    const DaliDiscoveryTransport *transport)
{
    DaliError err = send_special_no_reply(transport,
                                          DALI_CMD_TERMINATE,
                                          0u,
                                          false);
    if (err != DALI_OK) {
        return err;
    }

    err = send_special_no_reply(transport,
                                DALI_CMD_INITIALISE,
                                DALI_INITIALISE_UNADDRESSED_PARAM,
                                true);
    if (err != DALI_OK) {
        return err;
    }

    err = send_special_no_reply(transport, DALI_CMD_RANDOMIZE, 0u, true);
    if (err != DALI_OK) {
        return err;
    }
#ifndef DALI_HOST_BUILD
    vTaskDelay(pdMS_TO_TICKS(15u));
#endif
    return DALI_OK;
}

static DaliError commissioning_finish(const DaliDiscoveryTransport *transport)
{
    return send_special_no_reply(transport, DALI_CMD_TERMINATE, 0u, false);
}

DaliError dali_commissioning_commission_unaddressed(
    const DaliDiscoveryTransport *transport,
    const DaliCommissioningOptions *options,
    DaliCommissioningResult *out,
    DaliCommissioningProgressCb progress_cb,
    void *progress_ctx)
{
    if (!comm_transport_valid(transport) || options == NULL || out == NULL ||
        options->first_short_address >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->last_error = DALI_OK;

    uint64_t used_mask = options->used_address_mask;
    out->free_address_count = count_free_addresses(used_mask,
                                                   options->first_short_address);
    if (out->free_address_count == 0u) {
        out->address_space_full = true;
        emit_progress(progress_cb,
                      progress_ctx,
                      DALI_COMMISSIONING_EVENT_ADDRESS_SPACE_FULL,
                      0u,
                      0u,
                      0u);
        return DALI_OK;
    }

    uint8_t requested_count = options->max_devices;
    if (requested_count == 0u || requested_count > out->free_address_count) {
        requested_count = out->free_address_count;
    }

    DaliError err = commissioning_start_unaddressed(transport);
    if (err != DALI_OK) {
        out->last_error = err;
        return err;
    }
    emit_progress(progress_cb,
                  progress_ctx,
                  DALI_COMMISSIONING_EVENT_INITIALISED,
                  0u,
                  0u,
                  0u);
    emit_progress(progress_cb,
                  progress_ctx,
                  DALI_COMMISSIONING_EVENT_RANDOMISED,
                  0u,
                  0u,
                  0u);

    uint8_t next_search_from = options->first_short_address;
    while (out->assigned_count < requested_count) {
        uint8_t short_address = 0u;
        if (!allocate_next_address(used_mask, next_search_from, &short_address)) {
            out->address_space_full = true;
            emit_progress(progress_cb,
                          progress_ctx,
                          DALI_COMMISSIONING_EVENT_ADDRESS_SPACE_FULL,
                          0u,
                          0u,
                          out->assigned_count);
            break;
        }

        uint32_t random_address = 0u;
        bool found = false;
        err = dali_commissioning_find_next_random_address(transport,
                                                          &random_address,
                                                          &found);
        if (err != DALI_OK) {
            out->last_error = err;
            (void)commissioning_finish(transport);
            return err;
        }
        if (!found) {
            out->no_more_devices = true;
            emit_progress(progress_cb,
                          progress_ctx,
                          DALI_COMMISSIONING_EVENT_NO_MORE_DEVICES,
                          0u,
                          0u,
                          out->assigned_count);
            break;
        }

        emit_progress(progress_cb,
                      progress_ctx,
                      DALI_COMMISSIONING_EVENT_SEARCH_FOUND,
                      random_address,
                      short_address,
                      out->assigned_count);

        err = dali_commissioning_program_short_address(transport, short_address);
        if (err != DALI_OK) {
            out->last_error = err;
            (void)commissioning_finish(transport);
            return err;
        }

        bool verified = false;
        err = dali_commissioning_verify_short_address(transport,
                                                      short_address,
                                                      &verified);
        if (err != DALI_OK) {
            out->last_error = err;
            (void)commissioning_finish(transport);
            return err;
        }
        if (!verified) {
            out->last_error = DALI_ERR_MALFORMED;
            (void)commissioning_finish(transport);
            return DALI_ERR_MALFORMED;
        }

        DaliCommissioningAssignment *assignment =
            &out->assignments[out->assigned_count];
        assignment->random_address = random_address;
        assignment->short_address = short_address;
        assignment->has_query_short = false;
        assignment->query_short_raw = 0u;
        assignment->query_short_address = 0u;

        if (options->query_short_address) {
            uint8_t encoded = 0u;
            err = dali_commissioning_query_short_address(transport, &encoded);
            if (err != DALI_OK) {
                out->last_error = err;
                (void)commissioning_finish(transport);
                return err;
            }

            assignment->query_short_raw = encoded;
            err = dali_commissioning_decode_short_address(
                encoded,
                &assignment->query_short_address);
            if (err != DALI_OK ||
                assignment->query_short_address != short_address) {
                out->last_error = DALI_ERR_MALFORMED;
                (void)commissioning_finish(transport);
                return DALI_ERR_MALFORMED;
            }
            assignment->has_query_short = true;
        }

        err = send_special_no_reply(transport, DALI_CMD_WITHDRAW, 0u, false);
        if (err != DALI_OK) {
            out->last_error = err;
            (void)commissioning_finish(transport);
            return err;
        }

        used_mask |= ((uint64_t)1u << short_address);
        out->assigned_count++;
        next_search_from = (uint8_t)(short_address + 1u);
        emit_progress(progress_cb,
                      progress_ctx,
                      DALI_COMMISSIONING_EVENT_ASSIGNED,
                      random_address,
                      short_address,
                      out->assigned_count);
    }

    err = commissioning_finish(transport);
    if (err != DALI_OK) {
        out->last_error = err;
        return err;
    }
    emit_progress(progress_cb,
                  progress_ctx,
                  DALI_COMMISSIONING_EVENT_TERMINATED,
                  0u,
                  0u,
                  out->assigned_count);

    return DALI_OK;
}
