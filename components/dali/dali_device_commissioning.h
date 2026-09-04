#pragma once

/*
 * dali_device_commissioning.h - IEC 62386-103 control-device commissioning
 *
 * The control-device counterpart of dali_commissioning.h. Same walk shape --
 * INITIALISE, RANDOMISE, then a binary search over the 24-bit random address
 * space with COMPARE, then PROGRAM SHORT ADDRESS and VERIFY -- over a different
 * command space with different encodings.
 *
 * A parallel module rather than a generalisation of the gear one, because the
 * two spaces genuinely differ: different opcodes, two inverted or re-based
 * parameter encodings, and an opposite cross-part guard. What they share is the
 * part that is subtle rather than the part that is different, and that part is
 * shared literally: the reply classification comes from
 * dali_commissioning_compare_from_sequence() and
 * dali_commissioning_verify_from_sequence(), which read a DaliSequenceResult and
 * know nothing about which space produced it. The three-outcome COMPARE and the
 * VERIFY duplicate detection therefore exist once, not twice.
 *
 * No ESP-IDF, ESPHome, UART or task dependency.
 */

#include "dali_commissioning.h"

/*
 * INITIALISE's parameter is a device address byte, and it reads inverted
 * against Part 102: here 0x00 selects only devices without a short address and
 * 0xFF selects every control device, where Part 102 INITIALISE uses 0x00 for
 * all gear and 0xFF for unaddressed gear.
 *
 * Getting this backwards does not fail: it opens an addressing window over the
 * whole bus, including devices that already have an address and are working.
 */
#define DALI_DEVICE_INITIALISE_UNADDRESSED_PARAM 0x00u
#define DALI_DEVICE_INITIALISE_ALL_PARAM         0xFFu

/*
 * PROGRAM SHORT ADDRESS carries the raw 6-bit address 0..63, not the
 * (a << 1) | 1 form the Part 102 special of the same name uses. 0xFF still
 * means "no short address", which is what the duplicate recovery writes.
 *
 * Note this disagrees with DALI_CMD_DEVICE_SET_SHORT_ADDRESS_DTR0 in the same
 * part, which does use the encoded form because it reads DTR0 rather than
 * carrying the address itself.
 */
#define DALI_DEVICE_NO_SHORT_ADDRESS             0xFFu

typedef struct {
    uint8_t  first_short_address;
    uint8_t  max_devices;        /* 0 = fill every free short address */
    uint64_t used_address_mask;  /* bit N set: device short address N is taken */
    bool     query_short_address;
    /*
     * Bracket the run with a Part 102 TERMINATE, so control *gear* cannot sit
     * in its own addressing state while Part 103 COMPARE is being asked.
     *
     * The mirror image of DaliCommissioningOptions::terminate_control_devices,
     * and required for the same reason read the other way round: 0xC1 is both
     * the Part 102 ENABLE DEVICE TYPE opcode and the first byte of every
     * Part 103 special frame, so gear in an open initialise window can act on
     * the specials a device run emits. The guard was designed once, in both
     * directions.
     *
     * Off in a zero-initialized struct. Hardening rather than a precondition:
     * a failure is recorded and the run continues.
     */
    bool     terminate_control_gear;

    /*
     * Bracket the run with broadcast START/STOP QUIESCENT MODE, the way the
     * gear walk does.
     *
     * This walk refused the bracket until 2026-09-04, on the grounds that
     * quiescent mode silences control devices and control devices are what it
     * is searching. That reasoning was wrong, and it lost the argument to
     * dali_protocol.h, which had the semantics right all along: quiescent mode
     * stops a device transmitting on its own initiative, not responding to a
     * command it was addressed with. A real bus settled the addressed-query
     * half -- `discover` under quiescence enumerates control devices and their
     * instances normally.
     *
     * With replies unaffected, the trade runs entirely the other way. COMPARE
     * maps undecodable activity in its reply window to YES
     * (dali_commissioning_compare_from_sequence), so one event frame landing in
     * one window sends the 24-bit binary search down a branch no device is on.
     * This walk searches the event sources themselves and takes ~25 COMPARE
     * probes per device found, which makes it the walk with the most exposure
     * to that, not the least.
     *
     * What is still inferred rather than observed: COMPARE answered from inside
     * an open Part 103 addressing window, by a device with no short address.
     * Nothing separates it from the addressed-query case in the clause, and the
     * broadcast address byte 0xFF reaches unaddressed devices, but the bus has
     * not been asked that exact question yet.
     *
     * Off in a zero-initialized struct. Hardening rather than a precondition: a
     * failed START is recorded and the run continues.
     */
    bool     quiesce_control_devices;
} DaliDeviceCommissioningOptions;

typedef struct {
    uint8_t   assigned_count;
    uint8_t   free_address_count;
    bool      no_more_devices;
    bool      address_space_full;
    DaliError last_error;

    bool      termination_required;
    bool      termination_attempted;
    /* A no-reply TERMINATE was reported fully transmitted. This cannot prove
     * every device accepted it in the presence of other bus traffic. */
    bool      terminate_tx_succeeded;
    /* True when the safety TERMINATE could not be transmitted, so the
     * addressing state may still be active. */
    bool      initialisation_state_unknown;
    DaliError cleanup_error;

    /* The quiescence bracket, reported the way the gear walk reports its own:
     * transmission only. Nothing acknowledges START or STOP, so a bus with no
     * control devices and a bus that ignored both look identical from here.
     * quiescent_state_unknown is the one that matters operationally -- a START
     * that went out and a STOP that did not leaves an installation's sensors
     * silent until something releases them. */
    bool      quiescence_requested;
    bool      quiescence_started;
    bool      quiescence_release_attempted;
    bool      quiescent_state_unknown;
    DaliError quiescence_error;

    /* Cross-part Part 102 TERMINATE: what was transmitted, never what was
     * applied. Nothing acknowledges it, so a bus with no gear and a bus that
     * ignored it are indistinguishable from here. A failure means the
     * phantom-device hardening may not be in force, not that addressing is
     * wrong. */
    bool      cross_part_terminate_requested;
    bool      cross_part_terminate_attempted;
    DaliError cross_part_error;

    /* Random addresses held by more than one device, detected at VERIFY. The
     * pair is de-addressed with PROGRAM SHORT ADDRESS 0xFF and withdrawn, the
     * short address left unconsumed, and the run continues -- a re-run picks
     * them up with fresh random addresses. */
    uint8_t   duplicate_count;
    uint32_t  duplicate_random_addresses[DALI_COMMISSIONING_MAX_DUPLICATES];
    bool      duplicate_recovery_failed;

    DaliCommissioningAssignment assignments[DALI_COMMISSIONING_MAX_ASSIGNMENTS];
} DaliDeviceCommissioningResult;

/* ---------------------------------------------------------------------------
 * Sequence builders — pure, no bus traffic
 *
 * Step layouts deliberately match the Part 102 ones (see the
 * DALI_COMMISSIONING_*_STEP_* constants), which is what lets the shared reply
 * classifiers read a device sequence result unchanged.
 * --------------------------------------------------------------------------*/

DaliError dali_device_commissioning_build_start_sequence(DaliSequence *out);

DaliError dali_device_commissioning_build_search_sequence(uint32_t      random_address,
                                                          DaliSequence *out);

DaliError dali_device_commissioning_build_search_compare_sequence(
    uint32_t      random_address,
    DaliSequence *out);

DaliError dali_device_commissioning_build_program_verify_sequence(
    uint8_t       short_address,
    DaliSequence *out);

/* ---------------------------------------------------------------------------
 * Operations
 * --------------------------------------------------------------------------*/

/* Binary search for the lowest random address still answering COMPARE. */
DaliError dali_device_commissioning_find_next_random_address(
    const DaliTransport *transport,
    uint32_t            *random_address_out,
    bool                *found_out);

DaliError dali_device_commissioning_program_short_address(
    const DaliTransport *transport,
    uint8_t              short_address);

DaliError dali_device_commissioning_verify_short_address(
    const DaliTransport            *transport,
    uint8_t                         short_address,
    DaliCommissioningVerifyOutcome *outcome_out);

DaliError dali_device_commissioning_query_short_address(
    const DaliTransport *transport,
    uint8_t             *short_address_out,
    bool                *has_address_out);

/*
 * Assign short addresses to every control device that holds none.
 *
 * Requires a transport with both an atomic sequence channel and a delay, for
 * the same reasons the gear walk does: the opening must not be interleaved, and
 * the post-RANDOMISE settle must actually happen — a skipped settle presents as
 * a bus with no devices on it rather than as an error.
 */
DaliError dali_device_commissioning_commission_unaddressed(
    const DaliTransport                  *transport,
    const DaliDeviceCommissioningOptions *options,
    DaliDeviceCommissioningResult        *out,
    DaliCommissioningProgressCb           progress_cb,
    void                                 *progress_ctx);

/*
 * Device short addresses already occupied, from a discovery inventory.
 *
 * The device-space counterpart of dali_commissioning_used_mask_from_inventory()
 * and deliberately separate from it: the two address spaces are independent, so
 * control gear at numeric address N must never reserve device address N. An
 * address whose Part 103 probe drew undecodable activity is reserved, because
 * something is there even though nothing could be read from it.
 */
uint64_t dali_device_commissioning_used_mask_from_inventory(
    const DaliDiscoveryInventory *inventory);
