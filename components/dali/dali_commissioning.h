#pragma once

/*
 * dali_commissioning.h - reusable DALI control-gear commissioning helpers
 *
 * This module drives the standard random-address commissioning sequence over
 * an abstract transaction function. It has no ESP-IDF, ESPHome, UART, or task
 * dependency.
 */

#include "dali_discovery.h"

#define DALI_RANDOM_ADDRESS_MAX              0xFFFFFFu
#define DALI_INITIALISE_UNADDRESSED_PARAM    0xFFu
#define DALI_COMMISSIONING_MAX_ASSIGNMENTS   DALI_SHORT_ADDRESS_COUNT
#define DALI_COMMISSIONING_QUERY_RETRIES_LEFT 1u

/*
 * Settle time after RANDOMISE, before the first COMPARE. Gear needs this long to
 * finish generating its 24-bit random address, and gear that has not finished
 * does not answer COMPARE — which is indistinguishable from there being no gear
 * left to find. Raised from 15 ms on 2026-08-24; the 100 ms figure is Espressif's
 * `esp_dali` citing IEC 62386-102 §11.3, not a reading of the standard text
 * here, and it has not been exercised on a bus since the change.
 *
 * The wait itself comes from DaliTransport::delay_ms. A transport that supplies
 * none is refused before any frame is sent, because a skipped settle presents as
 * a bus with nothing on it rather than as an error.
 */
#define DALI_COMMISSIONING_RANDOMISE_SETTLE_MS 100u

/*
 * Settle after broadcast START QUIESCENT MODE, before INITIALISE.
 *
 * Unlike the RANDOMISE settle, this is not a standards figure and is not
 * presented as one. It is derived from the bus: a control device that began
 * transmitting an event frame just before START arrived is still clocking it
 * out, and a 24-bit frame at 1200 bps takes about 20 ms. Waiting two frame
 * times lets any such frame finish and its stop condition pass before
 * INITIALISE opens the window that a stray frame would corrupt.
 *
 * What it does not do: quiescent mode stops a device from starting new
 * transmissions, so nothing here needs the device to compute anything. If the
 * standard specifies an entry time, it is not read here and this constant may
 * be too short — but it is bounded below by the frame-duration argument, which
 * is checkable without the standard.
 */
#define DALI_COMMISSIONING_QUIESCENT_SETTLE_MS     ((2u * DALI_EXTENDED_FRAME_BITS * DALI_BIT_US) / 1000u)

typedef struct {
    uint32_t random_address;
    uint8_t  short_address;
    bool     has_query_short;
    uint8_t  query_short_raw;
    uint8_t  query_short_address;
} DaliCommissioningAssignment;

typedef struct {
    uint8_t first_short_address;
    uint8_t max_devices;       /* 0 = fill every free short address */
    uint64_t used_address_mask; /* bit N set means short address N is unavailable */
    bool query_short_address;
    /*
     * Bracket the run with broadcast START/STOP QUIESCENT MODE, so control
     * devices cannot put event frames into a COMPARE reply window and be
     * counted as gear.
     *
     * Off when the struct is zero-initialized, which keeps an out-of-tree
     * caller on the behaviour it already had. Two things to know before turning
     * it on: the release at the end of the run is unconditional, so it also
     * releases a quiescence an operator started by hand with the `quiescent`
     * verb; and a failed START does not abort the run, because quiescence is
     * hardening rather than a correctness requirement — the result says what
     * happened instead.
     */
    bool quiesce_control_devices;
} DaliCommissioningOptions;

typedef struct {
    uint8_t assigned_count;
    uint8_t free_address_count;
    bool    no_more_devices;
    bool    address_space_full;
    DaliError last_error;
    bool    termination_required;
    bool    termination_attempted;
    /* A no-reply TERMINATE frame was reported fully transmitted. This cannot
     * prove that every gear accepted it in the presence of other bus traffic. */
    bool    terminate_tx_succeeded;
    /* True when the safety TERMINATE could not be transmitted, so the
     * fifteen-minute initialisation state may still be active. */
    bool    initialisation_state_unknown;
    DaliError cleanup_error;
    /* Quiescence bracketing, reported the same way TERMINATE is: what was
     * transmitted, never what was applied. No control device acknowledges
     * either command, so a bus with none present and a bus that ignored the
     * broadcast are indistinguishable from here. */
    bool    quiescence_requested;
    bool    quiescence_started;
    bool    quiescence_release_attempted;
    /* True when quiescence was started and the release could not be
     * transmitted, so control devices may still be silent. The counterpart of
     * initialisation_state_unknown, and the more visible failure of the two:
     * an installation whose sensors stay quiet looks broken. */
    bool    quiescent_state_unknown;
    DaliError quiescence_error;
    DaliCommissioningAssignment assignments[DALI_COMMISSIONING_MAX_ASSIGNMENTS];
} DaliCommissioningResult;

typedef enum {
    DALI_COMMISSIONING_EVENT_INITIALISED = 0,
    DALI_COMMISSIONING_EVENT_RANDOMISED,
    DALI_COMMISSIONING_EVENT_SEARCH_FOUND,
    DALI_COMMISSIONING_EVENT_ASSIGNED,
    DALI_COMMISSIONING_EVENT_NO_MORE_DEVICES,
    DALI_COMMISSIONING_EVENT_ADDRESS_SPACE_FULL,
    DALI_COMMISSIONING_EVENT_TERMINATED,
} DaliCommissioningEventKind;

typedef struct {
    DaliCommissioningEventKind kind;
    uint32_t random_address;
    uint8_t  short_address;
    uint8_t  assigned_count;
} DaliCommissioningEvent;

typedef void (*DaliCommissioningProgressCb)(const DaliCommissioningEvent *event,
                                            void *ctx);

uint8_t dali_commissioning_encode_short_address(uint8_t short_address);
DaliError dali_commissioning_decode_short_address(uint8_t encoded,
                                                  uint8_t *short_address_out);

/* ---------------------------------------------------------------------------
 * Sequenced commissioning steps
 *
 * Commissioning is a chain of commands whose meaning depends on what preceded
 * them: COMPARE answers about whatever the last SEARCH ADDRH/M/L triple loaded,
 * and VERIFY SHORT ADDRESS answers about whatever PROGRAM SHORT ADDRESS just
 * wrote. Those groups are built as sequences and run through
 * dali_transport_run_sequence_atomic(), so no other locally scheduled
 * transaction can separate them. A frame-only transport is rejected before any
 * dependent group is issued. A separate physical bus master can still
 * interpose; that requires bus-level arbitration beyond this transport API.
 *
 * The PHY/scheduler preserve frame-like undecodable activity as
 * DALI_ERR_RX_ACTIVITY. COMPARE alone interprets that as YES; other queries
 * retain the ambiguity as an error. Atomicity keeps other local traffic out,
 * but a separate bus master can still invalidate the operation.
 * --------------------------------------------------------------------------*/

/* TERMINATE, INITIALISE(unaddressed), RANDOMISE. */
#define DALI_COMMISSIONING_START_SEQUENCE_STEPS  3u
#define DALI_COMMISSIONING_START_STEP_TERMINATE  0u
#define DALI_COMMISSIONING_START_STEP_INITIALISE 1u
#define DALI_COMMISSIONING_START_STEP_RANDOMISE  2u

/* SEARCH ADDRH, SEARCH ADDRM, SEARCH ADDRL. */
#define DALI_COMMISSIONING_SEARCH_SEQUENCE_STEPS 3u
#define DALI_COMMISSIONING_SEARCH_STEP_ADDRH     0u
#define DALI_COMMISSIONING_SEARCH_STEP_ADDRM     1u
#define DALI_COMMISSIONING_SEARCH_STEP_ADDRL     2u

/* The search triple plus the COMPARE it exists to answer. */
#define DALI_COMMISSIONING_SEARCH_COMPARE_SEQUENCE_STEPS 4u
#define DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE   3u

/* PROGRAM SHORT ADDRESS then VERIFY SHORT ADDRESS. */
#define DALI_COMMISSIONING_PROGRAM_VERIFY_SEQUENCE_STEPS 2u
#define DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_PROGRAM   0u
#define DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY    1u

/*
 * Build the opening of an unaddressed commissioning run. Grouping these matters
 * for what it prevents on failure as much as for ordering: if INITIALISE is
 * admitted and RANDOMISE is not, the gear sits in initialisation state for
 * fifteen minutes with nothing on the bus aware of it.
 *
 * INITIALISE and RANDOMISE are send-twice commands; the scheduler expands each
 * into an adjacent pair, so three logical steps become five forward frames.
 */
DaliError dali_commissioning_build_start_sequence(DaliSequence *out);

/* Build the three search-address writes for one 24-bit random address. */
DaliError dali_commissioning_build_search_sequence(uint32_t random_address,
                                                   DaliSequence *out);

/*
 * Build one complete binary-search probe: load the search address, then ask
 * COMPARE about it. This is the form the automated search uses, because a frame
 * landing between the triple and the COMPARE would have it answer about a
 * different search address.
 */
DaliError dali_commissioning_build_search_compare_sequence(uint32_t random_address,
                                                           DaliSequence *out);

/* Build the assignment pair for one short address. */
DaliError dali_commissioning_build_program_verify_sequence(uint8_t short_address,
                                                           DaliSequence *out);

/*
 * Read the COMPARE answer out of a completed search-compare sequence.
 *
 * A reply-window timeout on the COMPARE step is the standard way of saying NO,
 * so it is reported as yes = false with DALI_OK. A failure on any earlier step
 * is a real transport error and is returned as such — without that distinction
 * a failed search-address write would silently read as "no device here".
 */
DaliError dali_commissioning_compare_from_sequence(const DaliSequenceResult *result,
                                                   bool *yes_out);

/*
 * Read the VERIFY answer out of a completed program-verify sequence. A timeout
 * on the VERIFY step means the device did not confirm, reported as
 * verified = false with DALI_OK; an earlier failure is returned as an error.
 */
DaliError dali_commissioning_verify_from_sequence(const DaliSequenceResult *result,
                                                  bool *verified_out);

DaliError dali_commissioning_set_search_address(
    const DaliDiscoveryTransport *transport,
    uint32_t random_address);

DaliError dali_commissioning_compare(const DaliDiscoveryTransport *transport,
                                     bool *yes_out);

DaliError dali_commissioning_find_next_random_address(
    const DaliDiscoveryTransport *transport,
    uint32_t *random_address_out,
    bool *found_out);

DaliError dali_commissioning_program_short_address(
    const DaliDiscoveryTransport *transport,
    uint8_t short_address);

DaliError dali_commissioning_verify_short_address(
    const DaliDiscoveryTransport *transport,
    uint8_t short_address,
    bool *verified_out);

DaliError dali_commissioning_query_short_address(
    const DaliDiscoveryTransport *transport,
    uint8_t *encoded_out);

uint64_t dali_commissioning_used_mask_from_inventory(
    const DaliDiscoveryInventory *inventory);

DaliError dali_commissioning_commission_unaddressed(
    const DaliDiscoveryTransport *transport,
    const DaliCommissioningOptions *options,
    DaliCommissioningResult *out,
    DaliCommissioningProgressCb progress_cb,
    void *progress_ctx);
