#include "dali_restore.h"

#include <string.h>

#define RESTORE_NO_ADDRESS 0xFFu

/*
 * Upper bound on main-loop passes. Every pass either emits at least one move —
 * one or more when it places, exactly one when it stages or displaces — or
 * reclassifies one address as immovable, and neither can happen more times than
 * there are moves in a plan or addresses in a space. It exists so a logic error
 * becomes a reported incomplete plan rather than a hung shell holding the bus.
 */
#define RESTORE_LOOP_LIMIT \
    (DALI_RESTORE_MAX_MOVES + DALI_SHORT_ADDRESS_COUNT + 8u)

typedef struct {
    /*
     * Where the unit was found. `cur` follows it through a staging hop; the
     * identification read off the bus is still filed under `origin`, so a
     * conflict reported after a hop still names the right unit.
     */
    uint8_t origin;
    uint8_t cur;
    uint8_t dst;
    bool    active;
} RestorePending;

typedef struct {
    bool     present[DALI_SHORT_ADDRESS_COUNT];
    bool     has_ident[DALI_SHORT_ADDRESS_COUNT];
    uint8_t  ident[DALI_SHORT_ADDRESS_COUNT][DALI_MEMORY_BANK0_IDENTIFICATION_LEN];
    /* Gear only, and only where the scan's QUERY GROUPS actually answered. The
     * address planner ignores both; dali_restore_plan_groups() needs them. */
    bool     has_groups[DALI_SHORT_ADDRESS_COUNT];
    uint16_t groups[DALI_SHORT_ADDRESS_COUNT];
} RestoreBusUnits;

/*
 * Where conflicts are written. The address plan and the group plan are separate
 * structures that report the same kinds of trouble in the same way, so the
 * reporting is written against the list rather than against either plan --
 * which is what lets the two share the matching below, and stops a fix to one
 * planner's idea of "which unit is this" from missing the other.
 */
typedef struct {
    DaliRestoreConflict *items;
    uint8_t             *count;
    uint16_t            *total;
} RestoreConflictSink;

static void restore_add_conflict(const RestoreConflictSink *sink,
                                 DaliRestoreConflictKind    kind,
                                 DaliSnapshotSpace          space,
                                 uint8_t                    address,
                                 uint8_t                    other_address,
                                 bool                       has_identification,
                                 const uint8_t             *identification)
{
    (*sink->total)++;
    if (*sink->count >= DALI_RESTORE_MAX_CONFLICTS) {
        return;
    }

    DaliRestoreConflict *conflict = &sink->items[*sink->count];
    memset(conflict, 0, sizeof(*conflict));
    conflict->kind               = kind;
    conflict->space              = space;
    conflict->address            = address;
    conflict->other_address      = other_address;
    conflict->has_identification = has_identification;
    if (has_identification && identification != NULL) {
        memcpy(conflict->identification,
               identification,
               DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
    }
    (*sink->count)++;
}

static bool restore_add_move(DaliRestorePlan    *plan,
                             DaliSnapshotSpace   space,
                             uint8_t             from,
                             uint8_t             to,
                             DaliRestoreMoveKind kind)
{
    if (plan->move_count >= DALI_RESTORE_MAX_MOVES) {
        plan->incomplete = true;
        return false;
    }

    DaliRestoreMove *move = &plan->moves[plan->move_count];
    move->space = space;
    move->from  = from;
    move->to    = to;
    move->kind  = kind;
    plan->move_count++;
    return true;
}

/*
 * Collect what is on the bus in one address space.
 *
 * Each space reads its own Bank 0 identity. Control gear at numeric address N
 * and a control device at numeric address N are different units unless their
 * identification numbers say otherwise, so neither space ever borrows the
 * other's anchor. A unit whose identity could not be read is collected as
 * present but unidentified and reported rather than silently skipped: an
 * operator needs to know the plan cannot place it.
 */
static void restore_collect_bus(RestoreBusUnits              *units,
                                DaliSnapshotSpace             space,
                                const DaliDiscoveryInventory *inventory)
{
    memset(units, 0, sizeof(*units));

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device =
            dali_discovery_inventory_get(inventory, addr);
        if (device == NULL || !device->present) {
            continue;
        }

        if (space == DALI_SNAPSHOT_SPACE_GEAR) {
            if (!device->has_control_gear) {
                continue;
            }
            units->present[addr] = true;
            if (device->has_identity &&
                !dali_snapshot_identification_is_null(device->identity.serial)) {
                units->has_ident[addr] = true;
                memcpy(units->ident[addr],
                       device->identity.serial,
                       DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
            }
            if (device->has_groups) {
                units->has_groups[addr] = true;
                units->groups[addr]     = device->groups;
            }
        } else {
            if (!device->has_input_device) {
                continue;
            }
            units->present[addr] = true;
            if (device->has_device_identity &&
                !dali_snapshot_identification_is_null(device->device_identity.serial)) {
                units->has_ident[addr] = true;
                memcpy(units->ident[addr],
                       device->device_identity.serial,
                       DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
            }
        }
    }
}

/* Address of the single bus unit carrying this identification number, or
 * RESTORE_NO_ADDRESS. Sets *duplicate when more than one carries it. */
static uint8_t restore_find_on_bus(const RestoreBusUnits *units,
                                   const uint8_t         *identification,
                                   bool                  *duplicate)
{
    uint8_t found = RESTORE_NO_ADDRESS;
    if (duplicate != NULL) {
        *duplicate = false;
    }

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        if (!units->present[addr] || !units->has_ident[addr]) {
            continue;
        }
        if (!dali_snapshot_identification_equal(units->ident[addr], identification)) {
            continue;
        }
        if (found != RESTORE_NO_ADDRESS) {
            if (duplicate != NULL) {
                *duplicate = true;
            }
            return found;
        }
        found = addr;
    }
    return found;
}

/*
 * Which snapshot entry the bus unit at `addr` is, or NULL having reported why
 * not. Both planners go through here so there is exactly one answer to "which
 * recorded unit is this", and a caller that gets NULL must leave the unit alone.
 *
 * The failures are deliberately not collapsed into one "cannot match": an
 * operator's next step is different for each. An unreadable identity is a bus
 * or driver problem, a duplicate is two units that must be told apart by hand,
 * and a unit absent from the backup is usually just newer than it.
 *
 * `out_kind` receives the conflict that was reported, or is left alone on
 * success; pass NULL when the distinction does not matter. The address planner
 * needs it because only one of these failures leaves a unit that can be moved
 * and found again afterwards.
 */
static const DaliSnapshotEntry *restore_match_unit(const RestoreBusUnits     *units,
                                                   const DaliSnapshot        *snapshot,
                                                   DaliSnapshotSpace          space,
                                                   uint8_t                    addr,
                                                   const RestoreConflictSink *sink,
                                                   DaliRestoreConflictKind   *out_kind)
{
    if (!units->has_ident[addr]) {
        restore_add_conflict(sink,
                             DALI_RESTORE_CONFLICT_UNIDENTIFIED,
                             space,
                             addr,
                             RESTORE_NO_ADDRESS,
                             false,
                             NULL);
        if (out_kind != NULL) {
            *out_kind = DALI_RESTORE_CONFLICT_UNIDENTIFIED;
        }
        return NULL;
    }

    bool bus_duplicate = false;
    const uint8_t first_on_bus =
        restore_find_on_bus(units, units->ident[addr], &bus_duplicate);
    if (bus_duplicate) {
        /*
         * Two units answering with one identification number. Acting on either
         * would be a guess, and a wrong guess changes the wrong fixture with no
         * way to tell from the bus which one it was.
         */
        restore_add_conflict(sink,
                             DALI_RESTORE_CONFLICT_DUPLICATE_BUS,
                             space,
                             addr,
                             first_on_bus,
                             true,
                             units->ident[addr]);
        if (out_kind != NULL) {
            *out_kind = DALI_RESTORE_CONFLICT_DUPLICATE_BUS;
        }
        return NULL;
    }

    bool snapshot_duplicate = false;
    const DaliSnapshotEntry *entry =
        dali_snapshot_find_by_identification(snapshot,
                                             space,
                                             units->ident[addr],
                                             &snapshot_duplicate);
    if (snapshot_duplicate) {
        restore_add_conflict(sink,
                             DALI_RESTORE_CONFLICT_DUPLICATE_SNAPSHOT,
                             space,
                             addr,
                             entry != NULL ? entry->short_address
                                           : RESTORE_NO_ADDRESS,
                             true,
                             units->ident[addr]);
        if (out_kind != NULL) {
            *out_kind = DALI_RESTORE_CONFLICT_DUPLICATE_SNAPSHOT;
        }
        return NULL;
    }

    if (entry == NULL) {
        restore_add_conflict(sink,
                             DALI_RESTORE_CONFLICT_UNKNOWN_UNIT,
                             space,
                             addr,
                             RESTORE_NO_ADDRESS,
                             true,
                             units->ident[addr]);
        if (out_kind != NULL) {
            *out_kind = DALI_RESTORE_CONFLICT_UNKNOWN_UNIT;
        }
        return NULL;
    }

    return entry;
}

/* Report the snapshot entries that no bus unit answered for. Shared for the
 * same reason as the matcher: what "not on the bus" means does not depend on
 * whether the caller wanted to move an address or rewrite a group mask. */
static void restore_report_unmatched_entries(const RestoreBusUnits     *units,
                                             const DaliSnapshot        *snapshot,
                                             DaliSnapshotSpace          space,
                                             const RestoreConflictSink *sink)
{
    for (uint8_t i = 0u; i < snapshot->entry_count; i++) {
        const DaliSnapshotEntry *entry = &snapshot->entries[i];
        if (entry->space != space) {
            continue;
        }

        if (!entry->has_identification) {
            /*
             * Recorded without an anchor, so it can never be matched. Reported
             * against the address it was recorded at, which is the only useful
             * thing known about it.
             */
            restore_add_conflict(sink,
                                 DALI_RESTORE_CONFLICT_UNIDENTIFIED,
                                 space,
                                 entry->short_address,
                                 RESTORE_NO_ADDRESS,
                                 false,
                                 NULL);
            continue;
        }

        bool duplicate = false;
        if (restore_find_on_bus(units, entry->identification, &duplicate) ==
            RESTORE_NO_ADDRESS) {
            restore_add_conflict(sink,
                                 DALI_RESTORE_CONFLICT_MISSING,
                                 space,
                                 entry->short_address,
                                 RESTORE_NO_ADDRESS,
                                 true,
                                 entry->identification);
        }
    }
}

/* Index of the active move whose unit sits on `addr` right now, or
 * RESTORE_NO_ADDRESS. */
static uint8_t restore_pending_at(const RestorePending *pending,
                                  uint8_t               count,
                                  uint8_t               addr)
{
    for (uint8_t i = 0u; i < count; i++) {
        if (pending[i].active && pending[i].cur == addr) {
            return i;
        }
    }
    return RESTORE_NO_ADDRESS;
}

/*
 * An active move that is genuinely part of a cycle, or RESTORE_NO_ADDRESS.
 *
 * A move that cannot proceed is not by itself evidence of a cycle: it may be
 * the head of a chain that ends on a unit waiting to be displaced, which is
 * unblocked by moving that unit and not by staging anything. Following each
 * chain to its end is what separates the two, and staging a chain member would
 * spend a free address to make no progress at all.
 */
static uint8_t restore_find_cycle_member(const RestorePending *pending, uint8_t count)
{
    for (uint8_t i = 0u; i < count; i++) {
        if (!pending[i].active) {
            continue;
        }

        uint8_t step = i;
        /* A chain that has not closed within `count` hops has entered some
         * other cycle, not this one; that cycle is found from its own members. */
        for (uint8_t hops = 0u; hops <= count; hops++) {
            const uint8_t next = restore_pending_at(pending, count, pending[step].dst);
            if (next == RESTORE_NO_ADDRESS) {
                break;
            }
            if (next == i) {
                return i;
            }
            step = next;
        }
    }
    return RESTORE_NO_ADDRESS;
}

/*
 * A free address that no remaining move wants, or RESTORE_NO_ADDRESS. The
 * staging hop and the displacement need exactly the same thing: somewhere a
 * unit can sit without becoming the next obstacle.
 */
static uint8_t restore_find_spare(uint64_t              occupied,
                                  const RestorePending *pending,
                                  uint8_t               count)
{
    uint64_t wanted = 0u;
    for (uint8_t i = 0u; i < count; i++) {
        if (pending[i].active) {
            wanted |= ((uint64_t)1u << pending[i].dst);
        }
    }

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const uint64_t bit = ((uint64_t)1u << addr);
        if ((occupied & bit) == 0u && (wanted & bit) == 0u) {
            return addr;
        }
    }
    return RESTORE_NO_ADDRESS;
}

/*
 * Drop the moves whose target is held by something that will never vacate it,
 * and propagate: a unit that stays put now blocks whoever was aimed at its own
 * address. Runs once before ordering, and again if a unit that looked
 * displaceable turns out to have nowhere to go.
 */
static void restore_drop_blocked(DaliSnapshotSpace          space,
                                 const RestoreBusUnits     *units,
                                 RestorePending            *pending,
                                 uint8_t                    pending_count,
                                 uint64_t                  *immovable,
                                 const RestoreConflictSink *sink)
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint8_t i = 0u; i < pending_count; i++) {
            if (!pending[i].active) {
                continue;
            }
            if ((*immovable & ((uint64_t)1u << pending[i].dst)) == 0u) {
                continue;
            }
            restore_add_conflict(sink,
                                 DALI_RESTORE_CONFLICT_TARGET_OCCUPIED,
                                 space,
                                 pending[i].cur,
                                 pending[i].dst,
                                 units->has_ident[pending[i].origin],
                                 units->ident[pending[i].origin]);
            pending[i].active = false;
            *immovable |= ((uint64_t)1u << pending[i].cur);
            changed = true;
        }
    }
}

static void restore_plan_space(DaliRestorePlan    *plan,
                               const DaliSnapshot *snapshot,
                               DaliSnapshotSpace   space,
                               RestoreBusUnits    *units)
{
    RestorePending pending[DALI_SHORT_ADDRESS_COUNT];
    uint8_t        pending_count = 0u;

    uint64_t occupied  = 0u;
    uint64_t immovable = 0u;
    /* Identified, but no snapshot entry claims it. Not a blocker in its own
     * right: it can be moved aside and still be found afterwards. */
    uint64_t displaceable = 0u;

    const RestoreConflictSink sink = {
        plan->conflicts, &plan->conflict_count, &plan->conflict_total,
    };

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        if (units->present[addr]) {
            occupied |= ((uint64_t)1u << addr);
        }
    }

    /* ---- match each bus unit to a snapshot entry ---------------------------*/
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        if (!units->present[addr]) {
            continue;
        }

        DaliRestoreConflictKind  why   = DALI_RESTORE_CONFLICT_UNIDENTIFIED;
        const DaliSnapshotEntry *entry =
            restore_match_unit(units, snapshot, space, addr, &sink, &why);
        if (entry == NULL) {
            /*
             * Reported by the matcher. A unit absent from the backup read back
             * an identification number, so moving it aside loses nothing — it
             * is still identifiable wherever it lands. Every other failure here
             * is a unit that cannot be told apart from another, or cannot be
             * read at all; moving one of those would put a fixture somewhere
             * nothing could confirm, so it stays and blocks its own address.
             */
            if (why == DALI_RESTORE_CONFLICT_UNKNOWN_UNIT) {
                displaceable |= ((uint64_t)1u << addr);
            } else {
                immovable |= ((uint64_t)1u << addr);
            }
            continue;
        }

        plan->matched_count++;
        if (entry->short_address == addr) {
            plan->already_correct_count++;
            immovable |= ((uint64_t)1u << addr);
            continue;
        }

        pending[pending_count].origin = addr;
        pending[pending_count].cur    = addr;
        pending[pending_count].dst    = entry->short_address;
        pending[pending_count].active = true;
        pending_count++;
    }

    /* ---- snapshot entries with nothing to match ---------------------------*/
    restore_report_unmatched_entries(units, snapshot, space, &sink);

    /* ---- drop moves blocked by something that will never move -------------*/
    restore_drop_blocked(space, units, pending, pending_count, &immovable, &sink);

    /* ---- order the remaining moves ----------------------------------------*/
    uint32_t passes = 0u;
    for (;;) {
        bool any_active = false;
        for (uint8_t i = 0u; i < pending_count; i++) {
            if (pending[i].active) {
                any_active = true;
                break;
            }
        }
        if (!any_active) {
            break;
        }

        if (passes++ >= RESTORE_LOOP_LIMIT) {
            plan->incomplete = true;
            break;
        }

        bool progress = false;
        for (uint8_t i = 0u; i < pending_count; i++) {
            if (!pending[i].active) {
                continue;
            }
            if ((occupied & ((uint64_t)1u << pending[i].dst)) != 0u) {
                continue;
            }
            if (!restore_add_move(plan, space, pending[i].cur, pending[i].dst,
                                  DALI_RESTORE_MOVE_PLACE)) {
                return;
            }
            occupied &= ~((uint64_t)1u << pending[i].cur);
            occupied |= ((uint64_t)1u << pending[i].dst);
            pending[i].active = false;
            progress = true;
        }

        if (progress) {
            continue;
        }

        /*
         * Nothing can move directly. Break a cycle before displacing anything:
         * a staging hop borrows its free address and hands it back when the
         * cycle unwinds, while a displacement keeps the address it takes. On a
         * bus with exactly one address to spare, displacing first strands the
         * cycle with nowhere to stage and turns a restore that would have
         * converged into an incomplete plan.
         */
        const uint8_t victim = restore_find_cycle_member(pending, pending_count);
        if (victim != RESTORE_NO_ADDRESS) {
            const uint8_t stage = restore_find_spare(occupied, pending, pending_count);
            if (stage == RESTORE_NO_ADDRESS) {
                restore_add_conflict(&sink,
                                     DALI_RESTORE_CONFLICT_NO_STAGING_ADDRESS,
                                     space,
                                     pending[victim].cur,
                                     pending[victim].dst,
                                     units->has_ident[pending[victim].origin],
                                     units->ident[pending[victim].origin]);
                plan->incomplete = true;
                break;
            }

            if (!restore_add_move(plan, space, pending[victim].cur, stage,
                                  DALI_RESTORE_MOVE_STAGE)) {
                return;
            }
            occupied &= ~((uint64_t)1u << pending[victim].cur);
            occupied |= ((uint64_t)1u << stage);
            pending[victim].cur = stage;
            continue;
        }

        /*
         * No cycle left, so every stalled move is waiting on a unit the backup
         * never recorded. Move it aside to an address nothing wants: it is
         * still powered, addressed and discoverable there, which is what makes
         * this different from retiring it. The operator finds out what it is
         * from the UNKNOWN_UNIT conflict the matcher already reported.
         */
        uint8_t blocked = RESTORE_NO_ADDRESS;
        for (uint8_t i = 0u; i < pending_count; i++) {
            if (pending[i].active &&
                (displaceable & ((uint64_t)1u << pending[i].dst)) != 0u) {
                blocked = i;
                break;
            }
        }
        if (blocked == RESTORE_NO_ADDRESS) {
            /* Stalled on neither a cycle nor a displaceable unit. The drop pass
             * clears every other reason a move cannot proceed, so this is a
             * logic error; refuse rather than emit a move the occupancy model
             * cannot account for. */
            plan->incomplete = true;
            break;
        }

        const uint8_t squatter = pending[blocked].dst;
        const uint8_t spare    = restore_find_spare(occupied, pending, pending_count);
        if (spare == RESTORE_NO_ADDRESS) {
            /*
             * The space is full, so it will never move after all. Reclassify it
             * as immovable and let the ordinary blocked-move pass report it —
             * refusing is the only safe answer when there is nowhere to put it,
             * and that is the same TARGET_OCCUPIED an operator saw before
             * displacement existed.
             */
            displaceable &= ~((uint64_t)1u << squatter);
            immovable |= ((uint64_t)1u << squatter);
            restore_drop_blocked(space, units, pending, pending_count, &immovable, &sink);
            continue;
        }

        if (!restore_add_move(plan, space, squatter, spare, DALI_RESTORE_MOVE_DISPLACE)) {
            return;
        }
        occupied &= ~((uint64_t)1u << squatter);
        occupied |= ((uint64_t)1u << spare);
        displaceable &= ~((uint64_t)1u << squatter);
    }
}

DaliError dali_restore_plan(DaliRestorePlan              *out,
                            const DaliSnapshot           *snapshot,
                            const DaliDiscoveryInventory *inventory)
{
    if (out == NULL || snapshot == NULL || inventory == NULL || !inventory->valid) {
        return DALI_ERR_INVALID;
    }
    if (snapshot->entry_count > DALI_SNAPSHOT_MAX_ENTRIES) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    static const DaliSnapshotSpace spaces[2] = {
        DALI_SNAPSHOT_SPACE_GEAR,
        DALI_SNAPSHOT_SPACE_DEVICE,
    };

    for (uint8_t s = 0u; s < 2u; s++) {
        RestoreBusUnits units;
        restore_collect_bus(&units, spaces[s], inventory);
        restore_plan_space(out, snapshot, spaces[s], &units);
    }

    return DALI_OK;
}

DaliError dali_restore_plan_groups(DaliRestoreGroupPlan         *out,
                                   const DaliSnapshot           *snapshot,
                                   const DaliDiscoveryInventory *inventory)
{
    if (out == NULL || snapshot == NULL || inventory == NULL || !inventory->valid) {
        return DALI_ERR_INVALID;
    }
    if (snapshot->entry_count > DALI_SNAPSHOT_MAX_ENTRIES) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    const RestoreConflictSink sink = {
        out->conflicts, &out->conflict_count, &out->conflict_total,
    };

    /* Gear only. IEC 62386-103 control devices have their own group scheme,
     * which nothing here reads and nothing here may guess at. */
    RestoreBusUnits units;
    restore_collect_bus(&units, DALI_SNAPSHOT_SPACE_GEAR, inventory);

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        if (!units.present[addr]) {
            continue;
        }

        /* Why the match failed changes nothing here: group membership is only
         * ever written to gear a snapshot entry claims. */
        const DaliSnapshotEntry *entry = restore_match_unit(
            &units, snapshot, DALI_SNAPSHOT_SPACE_GEAR, addr, &sink, NULL);
        if (entry == NULL) {
            continue;
        }

        out->matched_count++;

        if (!entry->has_groups) {
            /*
             * The backup does not say what this gear belonged to. Treating the
             * silence as "no groups" would issue REMOVE FROM GROUP for every
             * group it is in now, destroying membership on the strength of a
             * reading that never happened -- the one outcome a restore must
             * never produce. Report it and leave the gear alone.
             */
            restore_add_conflict(&sink,
                                 DALI_RESTORE_CONFLICT_NO_RECORDED_GROUPS,
                                 DALI_SNAPSHOT_SPACE_GEAR,
                                 addr,
                                 entry->short_address,
                                 true,
                                 units.ident[addr]);
            continue;
        }

        if (!units.has_groups[addr]) {
            /*
             * The gear answered enough of the scan to be identified but not its
             * QUERY GROUPS. The recorded mask could still be written blind, but
             * only the additions would be right: without the current mask there
             * is no way to know which groups it must leave, so the result would
             * be a fixture in the recorded rooms *and* whatever else it had
             * drifted into. A half restore is worse than a reported one.
             */
            restore_add_conflict(&sink,
                                 DALI_RESTORE_CONFLICT_GROUPS_UNREADABLE,
                                 DALI_SNAPSHOT_SPACE_GEAR,
                                 addr,
                                 entry->short_address,
                                 true,
                                 units.ident[addr]);
            continue;
        }

        const uint16_t current  = units.groups[addr];
        const uint16_t recorded = entry->groups;
        if (current == recorded) {
            out->already_correct_count++;
            continue;
        }

        /* One change per short address at most, and the array is sized by the
         * same constant the loop runs over, so this cannot overflow. */
        DaliRestoreGroupChange *change = &out->changes[out->change_count];
        change->address          = addr;
        change->recorded_address = entry->short_address;
        change->current          = current;
        change->recorded         = recorded;
        change->add_mask         = (uint16_t)(recorded & (uint16_t)~current);
        change->remove_mask      = (uint16_t)(current & (uint16_t)~recorded);
        out->change_count++;
    }

    restore_report_unmatched_entries(&units, snapshot, DALI_SNAPSHOT_SPACE_GEAR, &sink);

    return DALI_OK;
}

bool dali_restore_group_plan_is_clean(const DaliRestoreGroupPlan *plan)
{
    return plan != NULL &&
           plan->change_count == 0u &&
           plan->conflict_total == 0u;
}

bool dali_restore_plan_is_clean(const DaliRestorePlan *plan)
{
    return plan != NULL &&
           plan->move_count == 0u &&
           plan->conflict_total == 0u &&
           !plan->incomplete;
}

const char *dali_restore_conflict_name(DaliRestoreConflictKind kind)
{
    switch (kind) {
        case DALI_RESTORE_CONFLICT_UNIDENTIFIED:       return "identity unknown";
        case DALI_RESTORE_CONFLICT_UNKNOWN_UNIT:       return "not in backup";
        case DALI_RESTORE_CONFLICT_MISSING:            return "not on bus";
        case DALI_RESTORE_CONFLICT_DUPLICATE_SNAPSHOT: return "duplicate in backup";
        case DALI_RESTORE_CONFLICT_DUPLICATE_BUS:      return "duplicate on bus";
        case DALI_RESTORE_CONFLICT_TARGET_OCCUPIED:    return "target occupied";
        case DALI_RESTORE_CONFLICT_NO_STAGING_ADDRESS: return "no free address to stage";
        case DALI_RESTORE_CONFLICT_NO_RECORDED_GROUPS: return "no group data in backup";
        case DALI_RESTORE_CONFLICT_GROUPS_UNREADABLE:  return "groups unreadable";
        default:                                       return "unknown";
    }
}

const char *dali_restore_space_name(DaliSnapshotSpace space)
{
    switch (space) {
        case DALI_SNAPSHOT_SPACE_GEAR:   return "gear";
        case DALI_SNAPSHOT_SPACE_DEVICE: return "device";
        default:                         return "unknown";
    }
}
