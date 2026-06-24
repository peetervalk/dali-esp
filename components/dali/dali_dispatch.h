#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "dali_event.h"
#include "dali_control.h"

/* ── Match key ────────────────────────────────────────────────────────────── */

/* Use in event_code to match any opcode or DALI-2 event code. */
#define DALI_DISPATCH_OPCODE_ANY 0xFFu

/*
 * Selects which unsolicited frames trigger this entry.
 *
 * For BF6 / legacy couplers (DALI_EVENT_FRAME_LEGACY_16BIT):
 *   address_kind = DALI_EVENT_ADDRESS_GROUP, address = group number
 *   event_code   = DALI_DISPATCH_OPCODE_ANY  (coupler encodes ON/OFF itself)
 *
 * For phantom-address remapping (still LEGACY_16BIT, couplers reprogrammed):
 *   address_kind = DALI_EVENT_ADDRESS_SHORT, address = phantom short address
 *   event_code   = DALI_DISPATCH_OPCODE_ANY
 *
 * For DALI-2 push buttons (DALI_EVENT_FRAME_INPUT_24BIT):
 *   address_kind = DALI_EVENT_ADDRESS_SHORT, address = input device short addr
 *   event_code   = 0x02 (short-press) or 0x03 (double-press) per device config
 */
typedef struct {
    DaliEventFrameKind   frame_kind;
    DaliEventAddressKind address_kind;
    uint8_t              address;
    uint8_t              event_code;  /* DALI_DISPATCH_OPCODE_ANY = match all */
} DaliDispatchKey;

/* ── Actions ──────────────────────────────────────────────────────────────── */

typedef enum {
    /*
     * Re-issue the same legacy opcode received on the bus to the output target.
     * Only valid for DALI_EVENT_FRAME_LEGACY_16BIT command frames
     * (address_selector = 1). Supports: off, up, down, step-up, step-down,
     * recall-max, recall-min, step-down-and-off, on-and-step-up,
     * go-to-last-active-level, and go-to-scene N.
     *
     * BF6 use: coupler already drove the lights; ESP32 re-issues so its
     * internal toggle state stays in sync for any later TOGGLE entries.
     *
     * Phantom-address use: coupler sends to a free address; ESP32 translates
     * to the real group. output target differs from the key address.
     */
    DALI_DISPATCH_ACTION_MIRROR = 0,

    /* Always issue the named command regardless of input opcode. */
    DALI_DISPATCH_ACTION_RECALL_MAX,
    DALI_DISPATCH_ACTION_RECALL_MIN,
    DALI_DISPATCH_ACTION_OFF,
    DALI_DISPATCH_ACTION_GO_TO_LAST,
    DALI_DISPATCH_ACTION_DIM_UP,
    DALI_DISPATCH_ACTION_DIM_DOWN,
    DALI_DISPATCH_ACTION_SCENE,       /* set scene field in entry */

    /*
     * Stateful toggle: flip between RECALL_MAX and OFF using local bitmask.
     * Use for DALI-2 push buttons that send a press event per activation.
     * Not needed for BF6/phantom couplers — they carry ON/OFF in the frame.
     */
    DALI_DISPATCH_ACTION_TOGGLE,
} DaliDispatchAction;

/* ── Table entry ──────────────────────────────────────────────────────────── */

typedef struct {
    DaliDispatchKey    key;
    DaliTarget         output;
    DaliDispatchAction action;
    uint8_t            scene;  /* DALI_DISPATCH_ACTION_SCENE only; 0–15 */
} DaliDispatchEntry;

/* ── Toggle state ─────────────────────────────────────────────────────────── */

/*
 * Tracks on/off state per target for DALI_DISPATCH_ACTION_TOGGLE.
 * MIRROR also updates this so a mix of MIRROR and TOGGLE entries stays
 * consistent (e.g. one zone driven by BF6, another by a DALI-2 button).
 * Zero-initialise on startup; no persistence across power cycles.
 */
typedef struct {
    uint16_t group_on;  /* bit N = group N considered on */
    uint64_t short_on;  /* bit N = short address N considered on */
} DaliDispatchToggleState;

/* ── Inferred state result ────────────────────────────────────────────────── */

/*
 * Inferred bus state produced by a successful dispatch action.
 * has_state is false when the resulting brightness cannot be inferred without
 * querying the bus (e.g. GO_TO_SCENE, RECALL_MIN, DIM_UP/DOWN, GO_TO_LAST).
 */
typedef struct {
    bool       has_state;  /* false = brightness inference not possible */
    bool       matched;    /* true = a table entry fired (even if state unknown) */
    DaliTarget target;     /* output target that was commanded */
    bool       is_on;
    uint8_t    level;      /* DAPC level 1–254 when is_on; 0 when off */
} DaliDispatchResult;

/* ── Toggle state helpers ─────────────────────────────────────────────────── */

/*
 * Seed a single target's on/off bit in toggle_state.
 * Call at boot after querying ACTUAL_LEVEL so TOGGLE entries start from the
 * correct physical state rather than defaulting to "all off".
 */
void dali_dispatch_seed_toggle(DaliDispatchToggleState *state,
                               DaliTarget               target,
                               bool                     is_on);

/* ── API ──────────────────────────────────────────────────────────────────── */

/*
 * Scan the table for the first entry whose key matches event, then execute
 * its action by calling the appropriate dali_control_* function.
 *
 * Returns DALI_OK when a matching entry executes (even for fire-and-forget
 * scheduler transactions), or DALI_OK silently when no entry matches.
 * Returns DALI_ERR_INVALID for bad arguments or an unmappable frame
 * (e.g. MIRROR on a DAPC frame).
 *
 * toggle_state may be NULL when the table contains no TOGGLE entries.
 * result_out   may be NULL when the caller does not need inferred state.
 */
DaliError dali_dispatch(const DaliDispatchEntry  *table,
                        uint8_t                   count,
                        const DaliInputEvent     *event,
                        DaliDispatchToggleState  *toggle_state,
                        DaliDispatchResult       *result_out);
