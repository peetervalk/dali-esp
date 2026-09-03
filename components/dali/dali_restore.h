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
 * Group membership is planned separately, by dali_restore_plan_groups() at the
 * bottom of this file. It is a different repair for a different accident, and
 * folding it into the move list would make the ordinary address restore write
 * to gear it has no reason to touch.
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
    /* Group planning only: the gear is matched, but the backup recorded no
     * group data for it. Restoring it as "no groups" would wipe membership on
     * the strength of a reading that never happened, so it is left alone. */
    DALI_RESTORE_CONFLICT_NO_RECORDED_GROUPS,
    /* Group planning only: the gear is matched and the backup has its groups,
     * but the current membership could not be read, so there is nothing to
     * diff against. Writing the recorded mask blind would add the groups it
     * should be in without removing the ones it should not. */
    DALI_RESTORE_CONFLICT_GROUPS_UNREADABLE,
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

/* ---------------------------------------------------------------------------
 * Group membership
 *
 * Group membership lives in each gear's own non-volatile memory, keyed to the
 * gear and not to the address it answers on. A commissioning walk changes short
 * addresses and nothing else, so after a re-address the groups are already
 * right and restoring them is a no-op. This exists for the case where the
 * membership itself was destroyed: a RESET, a driver that lost its memory, or a
 * group-addressed edit that emptied more than it was meant to.
 *
 * Two consequences follow, and both are the reason this is not part of
 * dali_restore_plan():
 *
 *   - It does not depend on the address restore having run, or on ever running.
 *     A gear is matched by identification number and the edits are addressed to
 *     wherever it answers *now*, so it is correct on a scrambled bus and on a
 *     restored one alike.
 *
 *   - It is destructive in a way the address restore is not. An address move is
 *     reverted by moving it back; a removed group membership is only recovered
 *     from a record of what it was. A backup taken before a deliberate
 *     regrouping will undo that regrouping, which is why the plan reports the
 *     mask on both sides per fixture rather than a count of edits.
 *
 * Control gear only. Control devices have their own group scheme in IEC
 * 62386-103 which the discovery scan does not read, so device entries carry no
 * group data and no device is ever considered here.
 * --------------------------------------------------------------------------*/

/* One entry per control gear, so a full bus needs no more than this. */
#define DALI_RESTORE_MAX_GROUP_CHANGES DALI_SHORT_ADDRESS_COUNT

typedef struct {
    /* Where the gear answers now, and therefore where the edits are sent. Not
     * the address the backup recorded: the two differ whenever the group
     * restore runs before the address restore, or instead of it. */
    uint8_t  address;
    uint8_t  recorded_address;

    uint16_t current;      /* membership read from the bus  */
    uint16_t recorded;     /* membership from the backup    */

    /* current and recorded differ in these bits. Both are zero only for a gear
     * already correct, which is not stored as a change. */
    uint16_t add_mask;     /* recorded & ~current: groups to join  */
    uint16_t remove_mask;  /* current & ~recorded: groups to leave */
} DaliRestoreGroupChange;

typedef struct {
    DaliRestoreGroupChange changes[DALI_RESTORE_MAX_GROUP_CHANGES];
    uint8_t                change_count;

    DaliRestoreConflict    conflicts[DALI_RESTORE_MAX_CONFLICTS];
    uint8_t                conflict_count;   /* how many are stored */
    uint16_t               conflict_total;   /* how many were found */

    /* Gear matched to a backup entry, and how many of those already hold the
     * recorded membership. */
    uint8_t                matched_count;
    uint8_t                already_correct_count;
} DaliRestoreGroupPlan;

/*
 * Compute the group plan. Control gear only; the device space is not read.
 *
 * Returns DALI_ERR_INVALID on a bad argument or an inventory that is not valid.
 * As with dali_restore_plan(), conflicts are reported through the plan rather
 * than the return code: gear that cannot be matched, or whose membership is
 * unknown on either side, is reported and left untouched while the rest is
 * still planned.
 */
DaliError dali_restore_plan_groups(DaliRestoreGroupPlan         *out,
                                   const DaliSnapshot           *snapshot,
                                   const DaliDiscoveryInventory *inventory);

/* True when there is nothing to do and nothing wrong: no changes, no conflicts. */
bool dali_restore_group_plan_is_clean(const DaliRestoreGroupPlan *plan);
