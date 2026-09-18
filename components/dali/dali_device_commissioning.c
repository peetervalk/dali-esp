#include "dali_device_commissioning.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Transport plumbing
 *
 * Mirrors dali_commissioning.c step for step, over dali_build_device_special()
 * instead of dali_build_special(). The two builders are deliberately separate:
 * the spaces share command names and some opcode numbers while meaning
 * different things, so one builder taking a flag would be one edit away from
 * sending a Part 102 frame into a Part 103 walk.
 * --------------------------------------------------------------------------*/

static bool dev_transport_valid(const DaliTransport *transport)
{
    return transport != NULL && transport->transact != NULL;
}

static DaliError dev_transact_frame(const DaliTransport *transport,
                                    const DaliFrame     *frame,
                                    bool                 needs_reply,
                                    uint8_t              retries_left,
                                    bool                 send_twice,
                                    DaliFrame           *reply_out)
{
    if (!dev_transport_valid(transport) || frame == NULL) {
        return DALI_ERR_INVALID;
    }
    return transport->transact(frame,
                               needs_reply,
                               retries_left,
                               send_twice,
                               reply_out,
                               transport->ctx);
}

static DaliError dev_send_special(const DaliTransport *transport,
                                  DaliCommandId        id,
                                  uint8_t              param,
                                  bool                 send_twice)
{
    DaliFrame frame;
    DaliError err = dali_build_device_special(id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dev_transact_frame(transport, &frame, false, 0u, send_twice, NULL);
}

/* The cleanup channel bypasses front-end cancellation but still reports real
 * scheduler, PHY and bus failures, so closing a terminal cannot cancel the
 * safety unwind. */
static DaliError dev_send_special_cleanup(const DaliTransport *transport,
                                          DaliCommandId        id,
                                          uint8_t              param)
{
    DaliFrame frame;
    DaliError err = dali_build_device_special(id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return dali_transport_transact_cleanup(transport, &frame, false, 0u, false, NULL);
}

static DaliError dev_query_special_u8(const DaliTransport *transport,
                                      DaliCommandId        id,
                                      uint8_t              param,
                                      uint8_t             *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliFrame reply = {0u, 0u};
    DaliError err = dali_build_device_special(id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }

    err = dev_transact_frame(transport,
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
 * Cross-part guard: Part 102 TERMINATE
 *
 * The mirror of the Part 103 TERMINATE the gear walk sends. Control gear that
 * mis-frames a 0xC1-prefixed 24-bit special as a 16-bit forward frame, or that
 * was left in an initialise window by another tool, can answer this walk's
 * COMPARE as a device that is not there.
 * --------------------------------------------------------------------------*/

static DaliError dev_terminate_control_gear(const DaliTransport *transport,
                                            bool                 cleanup)
{
    DaliFrame frame;
    DaliError err = dali_build_special(DALI_CMD_TERMINATE, 0u, &frame);
    if (err != DALI_OK) {
        return err;
    }
    if (cleanup) {
        return dali_transport_transact_cleanup(transport, &frame, false, 0u,
                                               false, NULL);
    }
    return dev_transact_frame(transport, &frame, false, 0u, false, NULL);
}

static void dev_try_terminate_control_gear(const DaliTransport           *transport,
                                           DaliDeviceCommissioningResult *out,
                                           bool                           cleanup)
{
    if (!out->cross_part_terminate_requested) {
        return;
    }
    out->cross_part_terminate_attempted = true;
    DaliError err = dev_terminate_control_gear(transport, cleanup);
    /* Keep the first failure. Later sends are the same hardening, and a run
     * that could not send one of three is in the same state as one that could
     * not send any: the guard may not be in force. */
    if (err != DALI_OK && out->cross_part_error == DALI_OK) {
        out->cross_part_error = err;
    }
}

/* ---------------------------------------------------------------------------
 * Sequence builders
 * --------------------------------------------------------------------------*/

static DaliError dev_set_step(DaliSequenceStep *step,
                              DaliCommandId     id,
                              uint8_t           param)
{
    DaliFrame frame;
    DaliError err = dali_build_device_special(id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }
    step->frame = frame;
    return DALI_OK;
}

static DaliError dev_fill_search_steps(DaliSequenceStep *steps,
                                       uint32_t          random_address)
{
    DaliError err = dev_set_step(&steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRH],
                                 DALI_CMD_DEVICE_SEARCH_ADDRH,
                                 (uint8_t)((random_address >> 16u) & 0xFFu));
    if (err != DALI_OK) {
        return err;
    }
    err = dev_set_step(&steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRM],
                       DALI_CMD_DEVICE_SEARCH_ADDRM,
                       (uint8_t)((random_address >> 8u) & 0xFFu));
    if (err != DALI_OK) {
        return err;
    }
    return dev_set_step(&steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRL],
                        DALI_CMD_DEVICE_SEARCH_ADDRL,
                        (uint8_t)(random_address & 0xFFu));
}

DaliError dali_device_commissioning_build_start_sequence(DaliSequence *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    DaliError err = dev_set_step(&out->steps[DALI_COMMISSIONING_START_STEP_TERMINATE],
                                 DALI_CMD_DEVICE_TERMINATE,
                                 0u);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceStep *initialise =
        &out->steps[DALI_COMMISSIONING_START_STEP_INITIALISE];
    /* 0x00, not 0xFF. Part 103 INITIALISE is inverted against Part 102: this
     * selects devices *without* a short address. Passing the Part 102 sentinel
     * here would open an addressing window over every control device on the
     * bus, including the ones already working. */
    err = dev_set_step(initialise,
                       DALI_CMD_DEVICE_INITIALISE,
                       DALI_DEVICE_INITIALISE_UNADDRESSED_PARAM);
    if (err != DALI_OK) {
        return err;
    }
    initialise->send_twice = true;

    DaliSequenceStep *randomise =
        &out->steps[DALI_COMMISSIONING_START_STEP_RANDOMISE];
    err = dev_set_step(randomise, DALI_CMD_DEVICE_RANDOMISE, 0u);
    if (err != DALI_OK) {
        return err;
    }
    randomise->send_twice = true;

    out->step_count = DALI_COMMISSIONING_START_SEQUENCE_STEPS;
    return DALI_OK;
}

DaliError dali_device_commissioning_build_search_sequence(uint32_t      random_address,
                                                          DaliSequence *out)
{
    if (out == NULL || random_address > DALI_RANDOM_ADDRESS_MAX) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    DaliError err = dev_fill_search_steps(out->steps, random_address);
    if (err != DALI_OK) {
        return err;
    }

    out->step_count = DALI_COMMISSIONING_SEARCH_SEQUENCE_STEPS;
    return DALI_OK;
}

DaliError dali_device_commissioning_build_search_compare_sequence(
    uint32_t      random_address,
    DaliSequence *out)
{
    if (out == NULL || random_address > DALI_RANDOM_ADDRESS_MAX) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    DaliError err = dev_fill_search_steps(out->steps, random_address);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceStep *compare =
        &out->steps[DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE];
    err = dev_set_step(compare, DALI_CMD_DEVICE_COMPARE, 0u);
    if (err != DALI_OK) {
        return err;
    }
    compare->needs_reply = true;
    compare->retries_left = DALI_COMMISSIONING_QUERY_RETRIES_LEFT;

    out->step_count = DALI_COMMISSIONING_SEARCH_COMPARE_SEQUENCE_STEPS;
    return DALI_OK;
}

DaliError dali_device_commissioning_build_program_verify_sequence(
    uint8_t       short_address,
    DaliSequence *out)
{
    if (out == NULL ||
        (short_address >= DALI_SHORT_ADDRESS_COUNT &&
         short_address != DALI_DEVICE_NO_SHORT_ADDRESS)) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    /*
     * Raw 6-bit address, not (a << 1) | 1. This is the encoding that differs
     * from the Part 102 special of the same name, and getting it wrong programs
     * address 2n+1 without reporting anything.
     */
    DaliError err = dev_set_step(&out->steps[DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_PROGRAM],
                                 DALI_CMD_DEVICE_PROGRAM_SHORT_ADDRESS,
                                 short_address);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceStep *verify =
        &out->steps[DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY];
    err = dev_set_step(verify, DALI_CMD_DEVICE_VERIFY_SHORT_ADDRESS, short_address);
    if (err != DALI_OK) {
        return err;
    }
    verify->needs_reply = true;

    out->step_count = DALI_COMMISSIONING_PROGRAM_VERIFY_SEQUENCE_STEPS;
    return DALI_OK;
}

/* ---------------------------------------------------------------------------
 * Operations
 *
 * Reply classification is dali_commissioning_compare_from_sequence() and
 * dali_commissioning_verify_from_sequence(), unchanged. They read a
 * DaliSequenceResult at a step index and know nothing about which space
 * produced it, and the step layouts above are deliberately identical to the
 * Part 102 ones so they can. That keeps the three-outcome COMPARE and the
 * VERIFY duplicate inference in one place rather than two.
 * --------------------------------------------------------------------------*/

static DaliError dev_search_compare_probe(const DaliTransport *transport,
                                          uint32_t             random_address,
                                          bool                *yes_out)
{
    DaliSequence seq;
    DaliError err = dali_device_commissioning_build_search_compare_sequence(
        random_address, &seq);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceResult result;
    (void)dali_transport_run_sequence_atomic(transport, &seq, &result);
    return dali_commissioning_compare_from_sequence(&result, yes_out);
}

DaliError dali_device_commissioning_find_next_random_address(
    const DaliTransport *transport,
    uint32_t            *random_address_out,
    bool                *found_out)
{
    if (!dev_transport_valid(transport) ||
        random_address_out == NULL ||
        found_out == NULL) {
        return DALI_ERR_INVALID;
    }

    uint32_t low  = 0u;
    uint32_t high = DALI_RANDOM_ADDRESS_MAX;

    while (low < high) {
        uint32_t mid = low + ((high - low) / 2u);
        bool yes = false;
        DaliError err = dev_search_compare_probe(transport, mid, &yes);
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
    DaliError err = dev_search_compare_probe(transport, low, &yes);
    if (err != DALI_OK) {
        return err;
    }

    *random_address_out = low;
    *found_out = yes;
    return DALI_OK;
}

DaliError dali_device_commissioning_program_short_address(
    const DaliTransport *transport,
    uint8_t              short_address)
{
    if (!dev_transport_valid(transport) ||
        (short_address >= DALI_SHORT_ADDRESS_COUNT &&
         short_address != DALI_DEVICE_NO_SHORT_ADDRESS)) {
        return DALI_ERR_INVALID;
    }
    return dev_send_special(transport,
                            DALI_CMD_DEVICE_PROGRAM_SHORT_ADDRESS,
                            short_address,
                            false);
}

DaliError dali_device_commissioning_verify_short_address(
    const DaliTransport            *transport,
    uint8_t                         short_address,
    DaliCommissioningVerifyOutcome *outcome_out)
{
    if (!dev_transport_valid(transport) || outcome_out == NULL ||
        short_address >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliFrame reply = {0u, 0u};
    DaliError err = dali_build_device_special(DALI_CMD_DEVICE_VERIFY_SHORT_ADDRESS,
                                              short_address,
                                              &frame);
    if (err != DALI_OK) {
        return err;
    }

    err = dev_transact_frame(transport, &frame, true, 0u, false, &reply);
    if (err == DALI_ERR_TIMEOUT) {
        *outcome_out = DALI_COMMISSIONING_VERIFY_SILENT;
        return DALI_OK;
    }
    /* Undecodable activity here means more than one device is selected, which
     * is the same inference the sequence form makes on its VERIFY step. */
    if (err == DALI_ERR_RX_ACTIVITY) {
        *outcome_out = DALI_COMMISSIONING_VERIFY_MULTIPLE;
        return DALI_OK;
    }
    if (err != DALI_OK) {
        return err;
    }
    if (reply.bit_length != DALI_BACKWARD_FRAME_BITS ||
        !dali_is_yes((uint8_t)(reply.data & 0xFFu))) {
        return DALI_ERR_MALFORMED;
    }

    *outcome_out = DALI_COMMISSIONING_VERIFY_CONFIRMED;
    return DALI_OK;
}

DaliError dali_device_commissioning_query_short_address(
    const DaliTransport *transport,
    uint8_t             *short_address_out,
    bool                *has_address_out)
{
    if (!dev_transport_valid(transport) || short_address_out == NULL ||
        has_address_out == NULL) {
        return DALI_ERR_INVALID;
    }

    uint8_t raw = 0u;
    DaliError err = dev_query_special_u8(transport,
                                         DALI_CMD_DEVICE_QUERY_SHORT_ADDRESS,
                                         0u,
                                         &raw);
    if (err != DALI_OK) {
        return err;
    }

    /*
     * Returned raw, matching the PROGRAM encoding in this part rather than the
     * Part 102 (a << 1) | 1 form. 0xFF means the device holds no short address.
     *
     * Transcribed from the same source as the opcodes and not read from the
     * standard here, so a value that is neither 0xFF nor a valid 0..63 is
     * reported as malformed rather than being decoded on a guess.
     */
    if (raw == DALI_DEVICE_NO_SHORT_ADDRESS) {
        *has_address_out = false;
        *short_address_out = 0u;
        return DALI_OK;
    }
    if (raw >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_MALFORMED;
    }

    *has_address_out = true;
    *short_address_out = raw;
    return DALI_OK;
}

uint64_t dali_device_commissioning_used_mask_from_inventory(
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
         * Device space only. Control gear at this numeric address reserves
         * nothing here: the two spaces are independent, and treating a lamp as
         * an occupied device address would refuse addresses that are free.
         *
         * Undecodable Part 103 activity does reserve, for the reason the gear
         * mask reserves its own: something answered, so the address is taken
         * even though nothing could be read from it, and assigning a third
         * device onto it is the fault this prevents.
         */
        if (device->has_undecodable_device_activity ||
            (device->present && device->has_input_device)) {
            mask |= ((uint64_t)1u << addr);
        }
    }
    return mask;
}

/* ---------------------------------------------------------------------------
 * The walk
 * --------------------------------------------------------------------------*/

static bool dev_address_used(uint64_t mask, uint8_t address)
{
    return (mask & ((uint64_t)1u << address)) != 0u;
}

static bool dev_allocate_next_address(uint64_t used_mask,
                                      uint8_t  first_address,
                                      uint8_t *address_out)
{
    if (address_out == NULL || first_address >= DALI_SHORT_ADDRESS_COUNT) {
        return false;
    }
    for (uint8_t addr = first_address; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        if (!dev_address_used(used_mask, addr)) {
            *address_out = addr;
            return true;
        }
    }
    return false;
}

static uint8_t dev_count_free_addresses(uint64_t used_mask, uint8_t first_address)
{
    if (first_address >= DALI_SHORT_ADDRESS_COUNT) {
        return 0u;
    }
    uint8_t count = 0u;
    for (uint8_t addr = first_address; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        if (!dev_address_used(used_mask, addr)) {
            count++;
        }
    }
    return count;
}

static void dev_emit_progress(DaliCommissioningProgressCb cb,
                              void                       *ctx,
                              DaliCommissioningEventKind  kind,
                              uint32_t                    random_address,
                              uint8_t                     short_address,
                              uint8_t                     assigned_count)
{
    if (cb == NULL) {
        return;
    }
    DaliCommissioningEvent event;
    memset(&event, 0, sizeof(event));
    event.kind           = kind;
    event.random_address = random_address;
    event.short_address  = short_address;
    event.assigned_count = assigned_count;
    cb(&event, ctx);
}

static DaliError dev_start_unaddressed(const DaliTransport *transport,
                                       bool                *termination_required_out)
{
    if (termination_required_out == NULL) {
        return DALI_ERR_INVALID;
    }
    *termination_required_out = false;

    DaliSequence seq;
    DaliError err = dali_device_commissioning_build_start_sequence(&seq);
    if (err != DALI_OK) {
        return err;
    }

    /* Same two preconditions the gear walk enforces, and for the same reasons:
     * a transport with no atomic channel has admitted nothing, and one with no
     * delay would skip the post-RANDOMISE settle — which presents as a bus with
     * no devices on it rather than as an error. */
    if (!dali_transport_supports_atomic_sequence(transport) ||
        !dali_transport_supports_delay(transport)) {
        return DALI_ERR_INVALID;
    }

    *termination_required_out = true;
    DaliSequenceResult result;
    err = dali_transport_run_sequence_atomic(transport, &seq, &result);
    if (err != DALI_OK) {
        return err;
    }

    return dali_transport_delay_ms(transport,
                                   DALI_COMMISSIONING_RANDOMISE_SETTLE_MS);
}

static DaliError dev_finish(const DaliTransport *transport)
{
    return dev_send_special_cleanup(transport, DALI_CMD_DEVICE_TERMINATE, 0u);
}

static DaliError dev_program_and_verify(const DaliTransport            *transport,
                                        uint8_t                         short_address,
                                        DaliCommissioningVerifyOutcome *outcome_out)
{
    DaliSequence seq;
    DaliError err = dali_device_commissioning_build_program_verify_sequence(
        short_address, &seq);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequenceResult result;
    (void)dali_transport_run_sequence_atomic(transport, &seq, &result);
    return dali_commissioning_verify_from_sequence(&result, outcome_out);
}

/* Take the address back from a pair sharing one random address and drop them
 * out of the search, so the walk does not converge on them forever. */
static DaliError dev_deaddress_and_withdraw(const DaliTransport *transport)
{
    DaliError err = dev_send_special(transport,
                                     DALI_CMD_DEVICE_PROGRAM_SHORT_ADDRESS,
                                     DALI_DEVICE_NO_SHORT_ADDRESS,
                                     false);
    DaliError withdraw_err =
        dev_send_special(transport, DALI_CMD_DEVICE_WITHDRAW, 0u, false);
    return err != DALI_OK ? err : withdraw_err;
}

static void dev_record_duplicate(DaliDeviceCommissioningResult *out,
                                 uint32_t                       random_address)
{
    if (out->duplicate_count < DALI_COMMISSIONING_MAX_DUPLICATES) {
        out->duplicate_random_addresses[out->duplicate_count] = random_address;
    }
    if (out->duplicate_count < UINT8_MAX) {
        out->duplicate_count++;
    }
}

DaliError dali_device_commissioning_commission_unaddressed(
    const DaliTransport                  *transport,
    const DaliDeviceCommissioningOptions *options,
    DaliDeviceCommissioningResult        *out,
    DaliCommissioningProgressCb           progress_cb,
    void                                 *progress_ctx)
{
    if (!dev_transport_valid(transport) || options == NULL || out == NULL ||
        options->first_short_address >= DALI_SHORT_ADDRESS_COUNT) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->last_error = DALI_OK;
    out->cleanup_error = DALI_OK;

    uint64_t used_mask = options->used_address_mask;
    out->free_address_count = dev_count_free_addresses(used_mask,
                                                       options->first_short_address);
    if (out->free_address_count == 0u) {
        out->address_space_full = true;
        dev_emit_progress(progress_cb, progress_ctx,
                          DALI_COMMISSIONING_EVENT_ADDRESS_SPACE_FULL,
                          0u, 0u, 0u);
        return DALI_OK;
    }

    uint8_t requested_count = options->max_devices;
    if (requested_count == 0u || requested_count > out->free_address_count) {
        requested_count = out->free_address_count;
    }

    /*
     * Quiescence goes first, before INITIALISE, for the reason the gear walk
     * takes it: an event frame landing in a COMPARE reply window reads as YES
     * and sends the search after a device that is not there.
     *
     * This walk silences the population it is about to search, which is why it
     * refused the bracket at first. Quiescent mode does not gate replies -- see
     * DaliDeviceCommissioningOptions::quiesce_control_devices -- so the
     * silencing costs nothing here and the noise it removes is this walk's own
     * search targets firing events into its own reply windows.
     *
     * The capability check is hoisted above the START so the no-traffic
     * rejection contract still holds: dev_start_unaddressed() makes the same
     * check, but by then the START would already be on the bus.
     */
    if (options->quiesce_control_devices) {
        if (!dali_transport_supports_atomic_sequence(transport) ||
            !dali_transport_supports_delay(transport)) {
            return DALI_ERR_INVALID;
        }
        out->quiescence_requested = true;
        bool started = false;
        out->quiescence_error = dali_discovery_quiescence_start(transport,
                                                                &started);
        out->quiescence_started = started;
        /*
         * A failed START does not end the run, and the release still runs:
         * START may have been transmitted and then failed its settle, and
         * anything short of "certainly nothing left the scheduler" has to be
         * unwound or the installation stays silent.
         */
    }

    /* Close any Part 102 addressing window another tool left open, before the
     * specials this walk emits can be misread inside one. */
    out->cross_part_terminate_requested = options->terminate_control_gear;
    dev_try_terminate_control_gear(transport, out, false);

    bool termination_required = false;
    DaliError err = dev_start_unaddressed(transport, &termination_required);
    out->termination_required = termination_required;
    if (err != DALI_OK) {
        out->last_error = err;
        goto cleanup;
    }
    out->termination_required = true;

    /* And again, now the Part 103 INITIALISE has been on the wire: gear that
     * mis-framed it is in an initialise window this closes. The first send only
     * closed a window someone else opened. */
    dev_try_terminate_control_gear(transport, out, false);

    dev_emit_progress(progress_cb, progress_ctx,
                      DALI_COMMISSIONING_EVENT_INITIALISED, 0u, 0u, 0u);
    dev_emit_progress(progress_cb, progress_ctx,
                      DALI_COMMISSIONING_EVENT_RANDOMISED, 0u, 0u, 0u);

    uint8_t  next_search_from = options->first_short_address;
    uint32_t last_duplicate_random = 0u;
    bool     has_last_duplicate = false;

    while (out->assigned_count < requested_count) {
        uint8_t short_address = 0u;
        if (!dev_allocate_next_address(used_mask, next_search_from, &short_address)) {
            out->address_space_full = true;
            dev_emit_progress(progress_cb, progress_ctx,
                              DALI_COMMISSIONING_EVENT_ADDRESS_SPACE_FULL,
                              0u, 0u, out->assigned_count);
            break;
        }

        uint32_t random_address = 0u;
        bool found = false;
        err = dali_device_commissioning_find_next_random_address(transport,
                                                                 &random_address,
                                                                 &found);
        if (err != DALI_OK) {
            out->last_error = err;
            goto cleanup;
        }
        if (!found) {
            out->no_more_devices = true;
            dev_emit_progress(progress_cb, progress_ctx,
                              DALI_COMMISSIONING_EVENT_NO_MORE_DEVICES,
                              0u, 0u, out->assigned_count);
            break;
        }

        dev_emit_progress(progress_cb, progress_ctx,
                          DALI_COMMISSIONING_EVENT_SEARCH_FOUND,
                          random_address, short_address, out->assigned_count);

        DaliCommissioningVerifyOutcome outcome = DALI_COMMISSIONING_VERIFY_SILENT;
        err = dev_program_and_verify(transport, short_address, &outcome);
        if (err != DALI_OK) {
            out->last_error = err;
            goto cleanup;
        }

        if (outcome == DALI_COMMISSIONING_VERIFY_MULTIPLE) {
            /* Two devices share this random address. Hand the short address
             * back, drop them out of the search, and carry on with the address
             * unconsumed — one collision must not cost the rest of the bus its
             * addressing. */
            if (has_last_duplicate && last_duplicate_random == random_address) {
                /* The pair did not leave the search, so the walk cannot get
                 * past this address and looping is worse than stopping. */
                out->duplicate_recovery_failed = true;
                err = DALI_ERR_MALFORMED;
                out->last_error = err;
                goto cleanup;
            }
            last_duplicate_random = random_address;
            has_last_duplicate = true;

            dev_record_duplicate(out, random_address);
            dev_emit_progress(progress_cb, progress_ctx,
                              DALI_COMMISSIONING_EVENT_DUPLICATE_RANDOM_ADDRESS,
                              random_address, short_address, out->assigned_count);

            err = dev_deaddress_and_withdraw(transport);
            if (err != DALI_OK) {
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
        assignment->random_address     = random_address;
        assignment->short_address      = short_address;
        assignment->has_query_short    = false;
        assignment->query_short_raw    = 0u;
        assignment->query_short_address = 0u;

        if (options->query_short_address) {
            uint8_t queried = 0u;
            bool    has_address = false;
            err = dali_device_commissioning_query_short_address(transport,
                                                                &queried,
                                                                &has_address);
            if (err != DALI_OK) {
                out->last_error = err;
                goto cleanup;
            }
            /* Raw in this space, so query_short_raw and query_short_address
             * carry the same number rather than the encoded/decoded pair the
             * Part 102 walk records. */
            assignment->query_short_raw = queried;
            if (!has_address || queried != short_address) {
                err = DALI_ERR_MALFORMED;
                out->last_error = err;
                goto cleanup;
            }
            assignment->query_short_address = queried;
            assignment->has_query_short = true;
        }

        err = dev_send_special(transport, DALI_CMD_DEVICE_WITHDRAW, 0u, false);
        if (err != DALI_OK) {
            out->last_error = err;
            goto cleanup;
        }

        used_mask |= ((uint64_t)1u << short_address);
        out->assigned_count++;
        next_search_from = (uint8_t)(short_address + 1u);
        dev_emit_progress(progress_cb, progress_ctx,
                          DALI_COMMISSIONING_EVENT_ASSIGNED,
                          random_address, short_address, out->assigned_count);
    }

cleanup:
    if (out->termination_required) {
        out->termination_attempted = true;
        out->cleanup_error = dev_finish(transport);
        out->terminate_tx_succeeded = out->cleanup_error == DALI_OK;
        out->initialisation_state_unknown = !out->terminate_tx_succeeded;
        if (out->terminate_tx_succeeded) {
            dev_emit_progress(progress_cb, progress_ctx,
                              DALI_COMMISSIONING_EVENT_TERMINATED,
                              0u, 0u, out->assigned_count);
        }
    }

    /* The cross-part unwind, attempted however the Part 103 TERMINATE went:
     * control gear left in an addressing state answers the next tool's
     * COMPARE just as a control device would. */
    dev_try_terminate_control_gear(transport, out, true);

    /*
     * Released last, and attempted however everything above went. The
     * addressing states are the more dangerous things to leave set, so they are
     * unwound first; but a bus left quiescent is an installation whose sensors
     * have stopped, so this runs on every exit path that started it.
     */
    if (out->quiescence_requested) {
        out->quiescence_release_attempted = true;
        DaliError release_err = dali_discovery_quiescence_release(transport);
        out->quiescent_state_unknown =
            out->quiescence_started && release_err != DALI_OK;
        if (out->quiescence_error == DALI_OK) {
            out->quiescence_error = release_err;
        }
    }

    if (out->last_error != DALI_OK) {
        return out->last_error;
    }
    if (out->cleanup_error != DALI_OK) {
        out->last_error = out->cleanup_error;
        return out->cleanup_error;
    }
    return DALI_OK;
}
