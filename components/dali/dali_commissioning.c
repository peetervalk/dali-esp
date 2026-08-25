#include "dali_commissioning.h"

#include <string.h>

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

static DaliError send_special_cleanup_no_reply(
    const DaliDiscoveryTransport *transport,
    DaliCommandId id,
    uint8_t param)
{
    DaliFrame frame;
    DaliError err = dali_build_special(id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_transport_transact_cleanup(transport,
                                           &frame,
                                           false,
                                           0u,
                                           false,
                                           NULL);
}

/*
 * Broadcast START QUIESCENT MODE, then wait for any event frame already on the
 * wire to finish.
 *
 * Send-twice, no reply, address byte 0xFF: every control device, control gear
 * untouched. Nothing acknowledges it, so success here means transmitted, not
 * quiesced — a bus with no control devices on it reports exactly the same.
 *
 * transmitted_out is set the moment the frame is reported sent, separately from
 * the return value, because the settle that follows can fail on its own. The
 * bus is quiet either way at that point, and the release must run; collapsing
 * the two into one result would lose exactly the case that leaves an
 * installation silent.
 */
static DaliError commissioning_start_quiescence(
    const DaliDiscoveryTransport *transport,
    bool *transmitted_out)
{
    if (transmitted_out == NULL) {
        return DALI_ERR_INVALID;
    }
    *transmitted_out = false;

    DaliFrame frame;
    DaliError err = dali_build_device_broadcast_command(
        DALI_CMD_START_QUIESCENT_MODE, &frame);
    if (err != DALI_OK) {
        return err;
    }

    err = transact_frame(transport, &frame, false, 0u, true, NULL);
    if (err != DALI_OK) {
        return err;
    }
    *transmitted_out = true;

    return dali_transport_delay_ms(transport,
                                   DALI_COMMISSIONING_QUIESCENT_SETTLE_MS);
}

/*
 * Broadcast STOP QUIESCENT MODE through the cleanup path.
 *
 * Same reasoning as the TERMINATE unwind: this has to be attempted even when a
 * front-end cancellation is latched, because the alternative is an installation
 * whose sensors stay silent after an aborted run. No settle follows — the
 * caller is on its way out and nothing else is about to transmit.
 */
static DaliError commissioning_release_quiescence(
    const DaliDiscoveryTransport *transport)
{
    DaliFrame frame;
    DaliError err = dali_build_device_broadcast_command(
        DALI_CMD_STOP_QUIESCENT_MODE, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_transport_transact_cleanup(transport, &frame, false, 0u, true,
                                           NULL);
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

/* ---------------------------------------------------------------------------
 * Sequence builders and result readers
 * --------------------------------------------------------------------------*/

/* Fill one no-reply special-command step. */
static DaliError set_special_step(DaliSequenceStep *step,
                                  DaliCommandId id,
                                  uint8_t param)
{
    DaliFrame frame;
    DaliError err = dali_build_special(id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }

    step->frame = frame;
    return DALI_OK;
}

/* The three search-address writes, shared by both search sequences. */
static DaliError fill_search_steps(DaliSequenceStep *steps, uint32_t random_address)
{
    DaliError err = set_special_step(&steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRH],
                                     DALI_CMD_SEARCH_ADDRH,
                                     (uint8_t)((random_address >> 16u) & 0xFFu));
    if (err != DALI_OK) {
        return err;
    }
    err = set_special_step(&steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRM],
                           DALI_CMD_SEARCH_ADDRM,
                           (uint8_t)((random_address >> 8u) & 0xFFu));
    if (err != DALI_OK) {
        return err;
    }
    return set_special_step(&steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRL],
                            DALI_CMD_SEARCH_ADDRL,
                            (uint8_t)(random_address & 0xFFu));
}

DaliError dali_commissioning_build_start_sequence(DaliSequence *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    DaliError err = set_special_step(&out->steps[DALI_COMMISSIONING_START_STEP_TERMINATE],
                                     DALI_CMD_TERMINATE,
                                     0u);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceStep *initialise =
        &out->steps[DALI_COMMISSIONING_START_STEP_INITIALISE];
    err = set_special_step(initialise,
                           DALI_CMD_INITIALISE,
                           DALI_INITIALISE_UNADDRESSED_PARAM);
    if (err != DALI_OK) {
        return err;
    }
    initialise->send_twice = true;

    DaliSequenceStep *randomise =
        &out->steps[DALI_COMMISSIONING_START_STEP_RANDOMISE];
    err = set_special_step(randomise, DALI_CMD_RANDOMISE, 0u);
    if (err != DALI_OK) {
        return err;
    }
    randomise->send_twice = true;

    /* No retries: the scheduler already brackets each send-twice pair, and a
     * repeated RANDOMISE would hand out a fresh set of random addresses. */
    out->step_count = DALI_COMMISSIONING_START_SEQUENCE_STEPS;
    return DALI_OK;
}

DaliError dali_commissioning_build_search_sequence(uint32_t random_address,
                                                   DaliSequence *out)
{
    if (out == NULL || random_address > DALI_RANDOM_ADDRESS_MAX) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    DaliError err = fill_search_steps(out->steps, random_address);
    if (err != DALI_OK) {
        return err;
    }

    out->step_count = DALI_COMMISSIONING_SEARCH_SEQUENCE_STEPS;
    return DALI_OK;
}

DaliError dali_commissioning_build_search_compare_sequence(uint32_t random_address,
                                                           DaliSequence *out)
{
    if (out == NULL || random_address > DALI_RANDOM_ADDRESS_MAX) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    DaliError err = fill_search_steps(out->steps, random_address);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceStep *compare =
        &out->steps[DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE];
    err = set_special_step(compare, DALI_CMD_COMPARE, 0u);
    if (err != DALI_OK) {
        return err;
    }
    compare->needs_reply = true;
    /* COMPARE reads no device state and the search address survives it, so
     * retrying this step alone is safe. It is worth doing: a YES lost to noise
     * would otherwise read as NO and send the binary search the wrong way. */
    compare->retries_left = DALI_COMMISSIONING_QUERY_RETRIES_LEFT;

    out->step_count = DALI_COMMISSIONING_SEARCH_COMPARE_SEQUENCE_STEPS;
    return DALI_OK;
}

DaliError dali_commissioning_build_program_verify_sequence(uint8_t short_address,
                                                           DaliSequence *out)
{
    if (out == NULL || short_address >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    uint8_t encoded = dali_commissioning_encode_short_address(short_address);

    memset(out, 0, sizeof(*out));
    DaliError err =
        set_special_step(&out->steps[DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_PROGRAM],
                         DALI_CMD_PROGRAM_SHORT_ADDRESS,
                         encoded);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceStep *verify =
        &out->steps[DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY];
    err = set_special_step(verify, DALI_CMD_VERIFY_SHORT_ADDRESS, encoded);
    if (err != DALI_OK) {
        return err;
    }
    verify->needs_reply = true;
    /* VERIFY only reads back what PROGRAM wrote, so a lone retry is safe. */
    verify->retries_left = DALI_COMMISSIONING_QUERY_RETRIES_LEFT;

    out->step_count = DALI_COMMISSIONING_PROGRAM_VERIFY_SEQUENCE_STEPS;
    return DALI_OK;
}

/*
 * Shared reader for the two "no answer means no" queries. Only a timeout on the
 * expected step counts as a negative answer; anything else is a real error.
 */
static DaliError answer_from_sequence(const DaliSequenceResult *result,
                                      uint8_t query_step,
                                      bool *yes_out)
{
    if (result == NULL || yes_out == NULL) {
        return DALI_ERR_INVALID;
    }

    if (result->result != DALI_OK) {
        if (result->result == DALI_ERR_TIMEOUT &&
            result->failed_step == query_step) {
            *yes_out = false;
            return DALI_OK;
        }
        return result->result;
    }

    DaliFrame reply;
    if (!dali_sequence_result_reply(result, query_step, &reply) ||
        reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_MALFORMED;
    }

    if (!dali_is_yes((uint8_t)(reply.data & 0xFFu))) {
        /* COMPARE/VERIFY encode NO as silence. A decoded non-YES backward
         * frame is observed but invalid traffic, not a legitimate NO. */
        return DALI_ERR_MALFORMED;
    }
    *yes_out = true;
    return DALI_OK;
}

DaliError dali_commissioning_compare_from_sequence(const DaliSequenceResult *result,
                                                   bool *yes_out)
{
    if (result == NULL || yes_out == NULL) {
        return DALI_ERR_INVALID;
    }
    if (result->result == DALI_ERR_RX_ACTIVITY &&
        result->failed_step ==
            DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE) {
        *yes_out = true;
        return DALI_OK;
    }

    return answer_from_sequence(result,
                                DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE,
                                yes_out);
}

DaliError dali_commissioning_verify_from_sequence(
    const DaliSequenceResult *result,
    DaliCommissioningVerifyOutcome *outcome_out)
{
    if (result == NULL || outcome_out == NULL) {
        return DALI_ERR_INVALID;
    }

    /*
     * Activity on the VERIFY step alone. Exactly one device is selected when a
     * program-verify sequence runs, so undecodable reply-window activity here
     * is not the ambiguity it is everywhere else -- it says a second device
     * answered, which means a second device is selected, which means two gear
     * generated the same random address. On any earlier step it stays an error:
     * a PROGRAM that collided tells us nothing about how many devices there are.
     */
    if (result->result == DALI_ERR_RX_ACTIVITY &&
        result->failed_step == DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY) {
        *outcome_out = DALI_COMMISSIONING_VERIFY_MULTIPLE;
        return DALI_OK;
    }

    bool verified = false;
    DaliError err = answer_from_sequence(
        result,
        DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY,
        &verified);
    if (err != DALI_OK) {
        return err;
    }

    *outcome_out = verified ? DALI_COMMISSIONING_VERIFY_CONFIRMED
                            : DALI_COMMISSIONING_VERIFY_SILENT;
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

    DaliSequence seq;
    DaliError err = dali_commissioning_build_search_sequence(random_address, &seq);
    if (err != DALI_OK) {
        return err;
    }

    return dali_transport_run_sequence_atomic(transport, &seq, NULL);
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
    if (err == DALI_ERR_RX_ACTIVITY) {
        *yes_out = true;
        return DALI_OK;
    }
    if (err != DALI_OK) {
        return err;
    }

    if (!dali_is_yes(raw)) {
        return DALI_ERR_MALFORMED;
    }
    *yes_out = true;
    return DALI_OK;
}

/* One binary-search probe: load the search address and read COMPARE, with
 * nothing able to change the search address in between. */
static DaliError search_compare_probe(const DaliDiscoveryTransport *transport,
                                      uint32_t random_address,
                                      bool *yes_out)
{
    DaliSequence seq;
    DaliError err = dali_commissioning_build_search_compare_sequence(random_address,
                                                                     &seq);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceResult result;
    (void)dali_transport_run_sequence_atomic(transport, &seq, &result);
    return dali_commissioning_compare_from_sequence(&result, yes_out);
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
        bool yes = false;
        DaliError err = search_compare_probe(transport, mid, &yes);
        if (err != DALI_OK) {
            return err;
        }

        if (yes) {
            high = mid;
        } else {
            low = mid + 1u;
        }
    }

    bool yes = false;
    DaliError err = search_compare_probe(transport, low, &yes);
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
    DaliCommissioningVerifyOutcome *outcome_out)
{
    if (!comm_transport_valid(transport) ||
        short_address >= DALI_SHORT_ADDRESS_COUNT ||
        outcome_out == NULL) {
        return DALI_ERR_INVALID;
    }

    uint8_t raw = 0u;
    DaliError err = query_special_u8(
        transport,
        DALI_CMD_VERIFY_SHORT_ADDRESS,
        dali_commissioning_encode_short_address(short_address),
        &raw);
    if (err == DALI_ERR_TIMEOUT) {
        *outcome_out = DALI_COMMISSIONING_VERIFY_SILENT;
        return DALI_OK;
    }
    /* See dali_commissioning_verify_from_sequence(): on VERIFY, and only on
     * VERIFY, qualified activity is a count rather than an ambiguity. */
    if (err == DALI_ERR_RX_ACTIVITY) {
        *outcome_out = DALI_COMMISSIONING_VERIFY_MULTIPLE;
        return DALI_OK;
    }
    if (err != DALI_OK) {
        return err;
    }

    if (!dali_is_yes(raw)) {
        return DALI_ERR_MALFORMED;
    }
    *outcome_out = DALI_COMMISSIONING_VERIFY_CONFIRMED;
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
        if (device == NULL) {
            continue;
        }
        /*
         * Control gear and control devices have independent 0..63 short-
         * address spaces.  This mask feeds control-gear commissioning, so a
         * pure input device at the same numeric address must not reserve it.
         * Hybrid inventory entries still count because they contain gear.
         *
         * Undecodable activity also reserves the address: it answered a gear
         * QUERY STATUS, so the address is occupied in the gear space even
         * though nothing there could be read. Reserving it is the safe
         * direction — assigning a further device onto a contested address is
         * exactly the fault a commissioning run must not create.
         */
        if (device->has_undecodable_activity ||
            (device->present && device->has_control_gear)) {
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

static DaliError commissioning_finish(const DaliDiscoveryTransport *transport);

static DaliError commissioning_start_unaddressed(
    const DaliDiscoveryTransport *transport,
    bool *termination_required_out)
{
    if (termination_required_out == NULL) {
        return DALI_ERR_INVALID;
    }
    *termination_required_out = false;

    DaliSequence seq;
    DaliError err = dali_commissioning_build_start_sequence(&seq);
    if (err != DALI_OK) {
        return err;
    }

    /* A transport with no atomic-sequence entry point is known not to have
     * admitted any opening frame. Preserve the no-traffic rejection contract;
     * only an actual callback hand-off makes progress uncertain.
     *
     * The settle wait is checked in the same breath and for the same reason.
     * Without it the first COMPARE can outrun the gear's random-address
     * generation, and every device answers NO — a silent wrong answer that
     * looks exactly like an empty bus. Refusing here costs nothing; skipping
     * the wait costs a commissioning run that appears to succeed at finding
     * nothing. */
    if (!dali_transport_supports_atomic_sequence(transport) ||
        !dali_transport_supports_delay(transport)) {
        return DALI_ERR_INVALID;
    }

    /*
     * Once the opening sequence is handed to an atomic transport, a local
     * timeout can no longer prove that INITIALISE did not reach the bus: the
     * admitted sequence may already be active or may complete after the waiter
     * gives up. TERMINATE is harmless before INITIALISE, so unwind
     * conservatively on every return from the transport.
     */
    *termination_required_out = true;
    DaliSequenceResult result;
    err = dali_transport_run_sequence_atomic(transport, &seq, &result);
    if (err != DALI_OK) {
        return err;
    }

    /* The sequence ends with RANDOMISE; nothing may search until every gear has
     * finished generating its random address. The environment supplies the wait
     * so this module keeps no task dependency of its own. */
    return dali_transport_delay_ms(transport,
                                   DALI_COMMISSIONING_RANDOMISE_SETTLE_MS);
}

static DaliError commissioning_finish(const DaliDiscoveryTransport *transport)
{
    return send_special_cleanup_no_reply(transport, DALI_CMD_TERMINATE, 0u);
}

/* Assign one short address and read back the confirmation, with nothing able to
 * run between the write and the read-back. */
static DaliError program_and_verify(const DaliDiscoveryTransport *transport,
                                    uint8_t short_address,
                                    DaliCommissioningVerifyOutcome *outcome_out)
{
    DaliSequence seq;
    DaliError err = dali_commissioning_build_program_verify_sequence(short_address,
                                                                     &seq);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceResult result;
    (void)dali_transport_run_sequence_atomic(transport, &seq, &result);
    return dali_commissioning_verify_from_sequence(&result, outcome_out);
}

/*
 * Undo an assignment that turned out to have been written to two gear at once.
 *
 * PROGRAM SHORT ADDRESS 0xFF takes the address back from both -- selection is by
 * random address, so both are still addressed by it -- and WITHDRAW then removes
 * them from the search. The withdraw is not optional: without it the next
 * find_next_random_address() converges on the same random address and the walk
 * never terminates.
 *
 * The pair is left unaddressed rather than sharing an address, which is where
 * they started and what a later run knows how to handle. Nothing here can be
 * confirmed: two devices answering QUERY SHORT ADDRESS with the same 0xFF
 * collide exactly as they did before, so this reports transmission only.
 */
static DaliError deaddress_and_withdraw(const DaliDiscoveryTransport *transport)
{
    DaliError err = send_special_no_reply(transport,
                                          DALI_CMD_PROGRAM_SHORT_ADDRESS,
                                          DALI_COMMISSIONING_NO_SHORT_ADDRESS,
                                          false);
    DaliError withdraw_err =
        send_special_no_reply(transport, DALI_CMD_WITHDRAW, 0u, false);

    /*
     * Attempt both, keep the first failure. A failed de-address with a working
     * withdraw still has to leave the search, or the walk spins; a failed
     * withdraw after a working de-address is the case that spins, and the caller
     * turns either into a run-level error rather than continuing the loop.
     */
    return err != DALI_OK ? err : withdraw_err;
}

static void record_duplicate(DaliCommissioningResult *out,
                             uint32_t random_address)
{
    if (out->duplicate_count < DALI_COMMISSIONING_MAX_DUPLICATES) {
        out->duplicate_random_addresses[out->duplicate_count] = random_address;
    }
    if (out->duplicate_count < UINT8_MAX) {
        out->duplicate_count++;
    }
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
    out->cleanup_error = DALI_OK;

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

    /*
     * Quiescence goes first, before INITIALISE: a control device that is still
     * free to transmit can put an event frame into a COMPARE reply window,
     * where frame-like activity reads as YES and invents gear that is not
     * there. Bracketing narrows that window; it does not close it, because a
     * device that never receives the broadcast is unaffected and no physical
     * collision classification exists yet.
     *
     * The capability check is hoisted here so the no-traffic rejection contract
     * still holds: commissioning_start_unaddressed() makes the same check, but
     * by then the START would already be on the bus.
     */
    if (options->quiesce_control_devices) {
        if (!dali_transport_supports_atomic_sequence(transport) ||
            !dali_transport_supports_delay(transport)) {
            return DALI_ERR_INVALID;
        }
        out->quiescence_requested = true;
        bool started = false;
        out->quiescence_error = commissioning_start_quiescence(transport,
                                                               &started);
        out->quiescence_started = started;
        /*
         * A failed START does not end the run. Quiescence is hardening, not a
         * precondition: commissioning worked without it, and refusing to
         * commission because an optional silencing step failed would trade a
         * working operation for a noisy-bus risk the operator may not even
         * have. The result carries the failure instead.
         *
         * The release still runs. START may have been transmitted and then
         * reported an error on the settle, so anything short of "certainly
         * nothing left the scheduler" has to be unwound.
         */
    }

    bool termination_required = false;
    DaliError err = commissioning_start_unaddressed(
        transport,
        &termination_required);
    out->termination_required = termination_required;
    if (err != DALI_OK) {
        out->last_error = err;
        goto cleanup;
    }
    out->termination_required = true;
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
    /*
     * The random address of the last pair sent away as a duplicate. A duplicate
     * does not advance assigned_count, so it does not advance the loop bound
     * either: the only thing that stops the walk searching one address forever
     * is that WITHDRAW takes the pair out of the search. If the same address
     * comes back, it did not, and no amount of retrying will change that.
     */
    uint32_t last_duplicate_random = 0u;
    bool has_last_duplicate = false;
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
            goto cleanup;
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

        DaliCommissioningVerifyOutcome outcome =
            DALI_COMMISSIONING_VERIFY_SILENT;
        err = program_and_verify(transport, short_address, &outcome);
        if (err != DALI_OK) {
            out->last_error = err;
            goto cleanup;
        }
        if (outcome == DALI_COMMISSIONING_VERIFY_MULTIPLE) {
            /*
             * Two gear share this random address. Take the short address back
             * from both, drop them out of the search, and carry on: one
             * collision must not cost the rest of the bus its addressing, the
             * same reasoning the short-address scan applies to one contested
             * address among sixty-four.
             *
             * The address is deliberately left unconsumed -- used_mask and
             * next_search_from are untouched -- so the next device found takes
             * the address this one gave back.
             */
            if (has_last_duplicate && last_duplicate_random == random_address) {
                /*
                 * The pair did not leave the search. WITHDRAW was reported
                 * transmitted -- a failure would have aborted below -- so this
                 * is gear that did not act on it, or a bus master undoing the
                 * run. Either way the search cannot get past this address, and
                 * looping is worse than stopping.
                 */
                out->duplicate_recovery_failed = true;
                err = DALI_ERR_MALFORMED;
                out->last_error = err;
                goto cleanup;
            }
            last_duplicate_random = random_address;
            has_last_duplicate = true;

            record_duplicate(out, random_address);
            emit_progress(progress_cb,
                          progress_ctx,
                          DALI_COMMISSIONING_EVENT_DUPLICATE_RANDOM_ADDRESS,
                          random_address,
                          short_address,
                          out->assigned_count);

            err = deaddress_and_withdraw(transport);
            if (err != DALI_OK) {
                /*
                 * A pair that could not be withdrawn is still selectable, so
                 * continuing would search the same random address forever. Fail
                 * the run instead and say which half did not transmit.
                 */
                out->duplicate_recovery_failed = true;
                out->last_error = err;
                goto cleanup;
            }
            continue;
        }
        if (outcome != DALI_COMMISSIONING_VERIFY_CONFIRMED) {
            err = DALI_ERR_MALFORMED;
            out->last_error = err;
            goto cleanup;
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
                goto cleanup;
            }

            assignment->query_short_raw = encoded;
            err = dali_commissioning_decode_short_address(
                encoded,
                &assignment->query_short_address);
            if (err != DALI_OK ||
                assignment->query_short_address != short_address) {
                err = DALI_ERR_MALFORMED;
                out->last_error = err;
                goto cleanup;
            }
            assignment->has_query_short = true;
        }

        err = send_special_no_reply(transport, DALI_CMD_WITHDRAW, 0u, false);
        if (err != DALI_OK) {
            out->last_error = err;
            goto cleanup;
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

cleanup:
    if (out->termination_required) {
        out->termination_attempted = true;
        out->cleanup_error = commissioning_finish(transport);
        out->terminate_tx_succeeded = out->cleanup_error == DALI_OK;
        out->initialisation_state_unknown =
            !out->terminate_tx_succeeded;
        if (out->terminate_tx_succeeded) {
            emit_progress(progress_cb,
                          progress_ctx,
                          DALI_COMMISSIONING_EVENT_TERMINATED,
                          0u,
                          0u,
                          out->assigned_count);
        }
    }

    /*
     * Release after TERMINATE, and attempt it regardless of how TERMINATE went.
     * Order matters only in that the gear's fifteen-minute initialisation state
     * is the more dangerous one to leave set, so it is unwound first; both are
     * attempted either way.
     */
    if (out->quiescence_requested) {
        out->quiescence_release_attempted = true;
        DaliError release_err = commissioning_release_quiescence(transport);
        out->quiescent_state_unknown =
            out->quiescence_started && release_err != DALI_OK;
        if (out->quiescence_error == DALI_OK) {
            out->quiescence_error = release_err;
        }
    }

    /* A cleanup failure is secondary when another operation already failed.
     * Keep that primary result as the return value and expose the unwind
     * outcome independently in cleanup_error/initialisation_state_unknown. */
    if (out->last_error != DALI_OK) {
        return out->last_error;
    }
    if (out->cleanup_error != DALI_OK) {
        out->last_error = out->cleanup_error;
        return out->cleanup_error;
    }
    return DALI_OK;
}
