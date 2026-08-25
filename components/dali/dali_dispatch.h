#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "dali_event.h"
#include "dali_control.h"

/* ── Match key ────────────────────────────────────────────────────────────── */

/* Use in event_information to match any legacy data or Part-103 event. */
#define DALI_DISPATCH_EVENT_ANY    0xFFFFu
/* Backwards-compatible name for existing legacy dispatch tables. */
#define DALI_DISPATCH_OPCODE_ANY   DALI_DISPATCH_EVENT_ANY
/* Use in instance to match any Part-103 instance, including an absent one. */
#define DALI_DISPATCH_INSTANCE_ANY 0xFFu

/*
 * Selects which unsolicited frames trigger this entry.
 *
 * For BF6 / legacy couplers (DALI_EVENT_FRAME_LEGACY_16BIT):
 *   address_kind = DALI_EVENT_ADDRESS_GROUP, address = group number
 *   event_information = DALI_DISPATCH_EVENT_ANY (coupler encodes ON/OFF itself)
 *   instance     = DALI_DISPATCH_INSTANCE_ANY (not used for 16-bit frames)
 *
 * For phantom-address remapping (still LEGACY_16BIT, couplers reprogrammed):
 *   address_kind = DALI_EVENT_ADDRESS_SHORT, address = phantom short address
 *   event_information = DALI_DISPATCH_EVENT_ANY
 *   instance     = DALI_DISPATCH_INSTANCE_ANY
 *
 * Part-103 sources are projected onto the same five-field key as follows:
 *
 *   Device / Device-Instance:
 *     address_kind = DALI_EVENT_ADDRESS_SHORT
 *     address      = source device short address
 *
 *   Device-Group / Instance-Group:
 *     address_kind = DALI_EVENT_ADDRESS_GROUP
 *     address      = source device-group or instance-group number
 *
 *   Instance (no device or group identifier):
 *     address_kind = DALI_EVENT_ADDRESS_INVALID
 *     address      = ignored
 *
 * instance matches the canonical source instance when one is present; use
 * DALI_DISPATCH_INSTANCE_ANY when it is absent or irrelevant. The compact key
 * cannot distinguish Device-Group from Instance-Group or match instance type.
 * event_information matches all 10 Part-103 information bits (0x000-0x3FF).
 * For DT301, short press is 0x002 and double press is 0x005.
 */
typedef struct {
    DaliEventFrameKind   frame_kind;
    DaliEventAddressKind address_kind;
    uint8_t              address;
    uint16_t             event_information; /* exact value or DALI_DISPATCH_EVENT_ANY */
    uint8_t              instance;          /* exact value or DALI_DISPATCH_INSTANCE_ANY */
} DaliDispatchKey;

/* ── Actions ──────────────────────────────────────────────────────────────── */

typedef enum {
    /*
     * Observe a legacy 16-bit command frame and infer state without issuing any
     * DALI command. Use this for direct-control BF6 couplers where the coupler
     * already targeted the real group and the ESP32 only needs to update its
     * local/HA state.
     */
    DALI_DISPATCH_ACTION_OBSERVE = 0,

    /*
     * Re-issue the same legacy command opcode or DAPC level received on the bus
     * to the output target. Only valid for DALI_EVENT_FRAME_LEGACY_16BIT.
     * Supports: DAPC levels 0-254, off, up, down, step-up, step-down,
     * recall-max, recall-min, step-down-and-off, on-and-step-up,
     * go-to-last-active-level, and go-to-scene N.
     *
     * Phantom-address use: coupler sends to a free address; ESP32 translates
     * to the real group. output target differs from the key address.
     */
    DALI_DISPATCH_ACTION_MIRROR,

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
 * OBSERVE and MIRROR also update this so a mix of legacy direct-control,
 * phantom-translation, and TOGGLE entries stays consistent.
 * Zero-initialize on startup; no persistence across power cycles.
 */
typedef struct {
    uint16_t group_on;  /* bit N = group N considered on */
    uint64_t short_on;  /* bit N = short address N considered on */
    bool     broadcast_on; /* broadcast target considered on */
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
 * (e.g. MIRROR on DAPC level 0xFF).
 *
 * toggle_state may be NULL when the table contains no TOGGLE entries.
 * result_out   may be NULL when the caller does not need inferred state.
 */
DaliError dali_dispatch(const DaliDispatchEntry  *table,
                        uint8_t                   count,
                        const DaliInputEvent     *event,
                        DaliDispatchToggleState  *toggle_state,
                        DaliDispatchResult       *result_out);
