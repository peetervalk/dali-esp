#pragma once

/*
 * dali_restore.h - turn a snapshot plus a live bus into an ordered move list
 *
 * The restore itself is ordinary addressed traffic: SET SHORT ADDRESS DTR0
 * against a unit that already answers. No INITIALISE window is opened, nothing
 * is randomised, and nothing needs terminating, so a restore is interruptible at
 * any point and re-running it converges. That is the whole reason a scrambled
 * bus is recoverable: a commissioning walk gives every unit *some* address, and
 * this module works out how to permute those addresses back.
 *
 * Ordering is the hard part and the reason this is a separate module with its
 * own vectors. Addresses are a scarce shared resource: moving a unit onto an
 * address another unit still holds creates the contested-address fault that
 * nothing on the bus can undo remotely. So the plan is computed in full, against
 * a model of which addresses are occupied at each step, before a single frame is
 * sent — and a cycle (a5 wants a8, a8 wants a5) is broken by staging one member
 * through a free address, exactly as swapping two variables needs a third.
 *
 * This module performs no bus traffic and has no ESP-IDF, ESPHome, FreeRTOS or
 * task dependency. It decides; the front end executes.
 */

#include "dali_snapshot.h"

/*
 * Worst case is one 64-unit cycle per address space: 64 final moves plus one
 * staging hop, twice over. Sized so a plan is never truncated — a partial plan
 * executed in full would leave the bus in a state neither the snapshot nor the
 * inventory describes.
 */
#define DALI_RESTORE_MAX_MOVES     ((2u * DALI_SHORT_ADDRESS_COUNT) + 4u)

/* Conflicts are reported, not resolved. Stored up to this many; the total keeps
 * counting past the array so a truncated list still states the true number. */
#define DALI_RESTORE_MAX_CONFLICTS 32u

typedef enum {
    /*
     * No identification number, so nothing can be matched to it and it is left
     * exactly where it is. Reported from either side: a unit on the bus whose
     * Bank 0 read failed, or a snapshot entry recorded without an anchor. The
     * address says which one it is about; the two cases have the same remedy,
     * which is to find out why the identity cannot be read.
     */
    DALI_RESTORE_CONFLICT_UNIDENTIFIED = 0,
    /* On the bus and readable, but absent from the snapshot: gear added since
     * the backup. Left alone deliberately — a restore reverts addressing, it
     * does not retire units nobody recorded. */
    DALI_RESTORE_CONFLICT_UNKNOWN_UNIT,
    /* In the snapshot but not on the bus: powered down, removed, or unaddressed
     * and therefore unreachable by addressed commands. */
    DALI_RESTORE_CONFLICT_MISSING,
    /* Two snapshot entries share one identification number. */
    DALI_RESTORE_CONFLICT_DUPLICATE_SNAPSHOT,
    /* Two units on the bus report one identification number. */
    DALI_RESTORE_CONFLICT_DUPLICATE_BUS,
    /* The recorded address is held by a unit that will never move. */
    DALI_RESTORE_CONFLICT_TARGET_OCCUPIED,
    /* A cycle needs a free address to stage through and the space has none. */
    DALI_RESTORE_CONFLICT_NO_STAGING_ADDRESS,
} DaliRestoreConflictKind;

typedef struct {
    DaliRestoreConflictKind kind;
    DaliSnapshotSpace       space;
    /* The address the conflict is about: where the unit is now, or for MISSING,
     * where the snapshot says it should be. */
    uint8_t                 address;
    /* The second address involved, where the kind has one (the blocker for
     * TARGET_OCCUPIED, the other claimant for a duplicate). 0xFF when unused. */
    uint8_t                 other_address;
    bool                    has_identification;
    uint8_t                 identification[DALI_MEMORY_BANK0_IDENTIFICATION_LEN];
} DaliRestoreConflict;

typedef struct {
    DaliSnapshotSpace space;
    uint8_t           from;
    uint8_t           to;
    /*
     * True when this hop exists only to vacate an address inside a cycle. The
     * unit is not at its recorded address afterwards; a later move in the same
     * plan puts it there. An operator watching the run should not read a
     * staging hop as a finished placement.
     */
    bool              is_staging;
} DaliRestoreMove;

typedef struct {
    DaliRestoreMove     moves[DALI_RESTORE_MAX_MOVES];
    uint8_t             move_count;

    DaliRestoreConflict conflicts[DALI_RESTORE_MAX_CONFLICTS];
    uint8_t             conflict_count;   /* how many are stored */
    uint16_t            conflict_total;   /* how many were found */

    /* Units matched to a snapshot entry, and how many of those already hold the
     * recorded address. matched == already_correct with no moves means the bus
     * is exactly as recorded. */
    uint8_t             matched_count;
    uint8_t             already_correct_count;

    /* Set when the plan could not be completed and must not be executed:
     * the move list overflowed, or a cycle had nowhere to stage. Distinct from
     * merely having conflicts, which is a normal and executable outcome. */
    bool                incomplete;
} DaliRestorePlan;

/*
 * Compute the plan. Reads both address spaces. Never emits a move that would
 * place a unit onto an address occupied at that point in the sequence.
 *
 * Returns DALI_ERR_INVALID on a bad argument or an inventory that is not valid
 * (a scan that failed leaves a stale inventory, and planning from one would move
 * fixtures based on a bus that is no longer there). Conflicts are reported
 * through the plan, not through the return code: a plan with conflicts is still
 * a plan, and executing it does the part that is unambiguous.
 */
DaliError dali_restore_plan(DaliRestorePlan              *out,
                            const DaliSnapshot           *snapshot,
                            const DaliDiscoveryInventory *inventory);

/* True when there is nothing to do and nothing wrong: no moves, no conflicts. */
bool dali_restore_plan_is_clean(const DaliRestorePlan *plan);

const char *dali_restore_conflict_name(DaliRestoreConflictKind kind);
const char *dali_restore_space_name(DaliSnapshotSpace space);
