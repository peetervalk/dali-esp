#include "dali_restore.h"

#include <string.h>

#define RESTORE_NO_ADDRESS 0xFFu

/*
 * Upper bound on main-loop passes. Each pass either places at least one unit or
 * stages exactly one, and every unit is placed once and staged at most once, so
 * the loop cannot need more than this. It exists so a logic error becomes a
 * reported incomplete plan rather than a hung shell holding the bus.
 */
#define RESTORE_LOOP_LIMIT ((2u * DALI_SHORT_ADDRESS_COUNT) + 8u)

typedef struct {
    uint8_t cur;
    uint8_t dst;
    bool    active;
} RestorePending;

typedef struct {
    bool    present[DALI_SHORT_ADDRESS_COUNT];
    bool    has_ident[DALI_SHORT_ADDRESS_COUNT];
    uint8_t ident[DALI_SHORT_ADDRESS_COUNT][DALI_MEMORY_BANK0_IDENTIFICATION_LEN];
} RestoreBusUnits;

static void restore_add_conflict(DaliRestorePlan        *plan,
                                 DaliRestoreConflictKind kind,
                                 DaliSnapshotSpace       space,
                                 uint8_t                 address,
                                 uint8_t                 other_address,
                                 bool                    has_identification,
                                 const uint8_t          *identification)
{
    plan->conflict_total++;
    if (plan->conflict_count >= DALI_RESTORE_MAX_CONFLICTS) {
        return;
    }

    DaliRestoreConflict *conflict = &plan->conflicts[plan->conflict_count];
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
    plan->conflict_count++;
}

static bool restore_add_move(DaliRestorePlan  *plan,
                             DaliSnapshotSpace space,
                             uint8_t           from,
                             uint8_t           to,
                             bool              is_staging)
{
    if (plan->move_count >= DALI_RESTORE_MAX_MOVES) {
        plan->incomplete = true;
        return false;
    }

    DaliRestoreMove *move = &plan->moves[plan->move_count];
    move->space      = space;
    move->from       = from;
    move->to         = to;
    move->is_staging = is_staging;
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

static void restore_plan_space(DaliRestorePlan    *plan,
                               const DaliSnapshot *snapshot,
                               DaliSnapshotSpace   space,
                               RestoreBusUnits    *units)
{
    RestorePending pending[DALI_SHORT_ADDRESS_COUNT];
    uint8_t        pending_count = 0u;

    uint64_t occupied  = 0u;
    uint64_t immovable = 0u;

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

        if (!units->has_ident[addr]) {
            restore_add_conflict(plan,
                                 DALI_RESTORE_CONFLICT_UNIDENTIFIED,
                                 space,
                                 addr,
                                 RESTORE_NO_ADDRESS,
                                 false,
                                 NULL);
            immovable |= ((uint64_t)1u << addr);
            continue;
        }

        bool bus_duplicate = false;
        const uint8_t first_on_bus =
            restore_find_on_bus(units, units->ident[addr], &bus_duplicate);
        if (bus_duplicate) {
            /*
             * Two units answering with one identification number. Moving either
             * would be a guess, and a wrong guess puts a fixture in the wrong
             * room with no way to tell from the bus which one moved.
             */
            restore_add_conflict(plan,
                                 DALI_RESTORE_CONFLICT_DUPLICATE_BUS,
                                 space,
                                 addr,
                                 first_on_bus,
                                 true,
                                 units->ident[addr]);
            immovable |= ((uint64_t)1u << addr);
            continue;
        }

        bool snapshot_duplicate = false;
        const DaliSnapshotEntry *entry =
            dali_snapshot_find_by_identification(snapshot,
                                                 space,
                                                 units->ident[addr],
                                                 &snapshot_duplicate);
        if (snapshot_duplicate) {
            restore_add_conflict(plan,
                                 DALI_RESTORE_CONFLICT_DUPLICATE_SNAPSHOT,
                                 space,
                                 addr,
                                 entry != NULL ? entry->short_address
                                               : RESTORE_NO_ADDRESS,
                                 true,
                                 units->ident[addr]);
            immovable |= ((uint64_t)1u << addr);
            continue;
        }

        if (entry == NULL) {
            restore_add_conflict(plan,
                                 DALI_RESTORE_CONFLICT_UNKNOWN_UNIT,
                                 space,
                                 addr,
                                 RESTORE_NO_ADDRESS,
                                 true,
                                 units->ident[addr]);
            immovable |= ((uint64_t)1u << addr);
            continue;
        }

        plan->matched_count++;
        if (entry->short_address == addr) {
            plan->already_correct_count++;
            immovable |= ((uint64_t)1u << addr);
            continue;
        }

        pending[pending_count].cur    = addr;
        pending[pending_count].dst    = entry->short_address;
        pending[pending_count].active = true;
        pending_count++;
    }

    /* ---- snapshot entries with nothing to match ---------------------------*/
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
            restore_add_conflict(plan,
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
            restore_add_conflict(plan,
                                 DALI_RESTORE_CONFLICT_MISSING,
                                 space,
                                 entry->short_address,
                                 RESTORE_NO_ADDRESS,
                                 true,
                                 entry->identification);
        }
    }

    /* ---- drop moves blocked by something that will never move -------------*/
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint8_t i = 0u; i < pending_count; i++) {
            if (!pending[i].active) {
                continue;
            }
            if ((immovable & ((uint64_t)1u << pending[i].dst)) == 0u) {
                continue;
            }
            restore_add_conflict(plan,
                                 DALI_RESTORE_CONFLICT_TARGET_OCCUPIED,
                                 space,
                                 pending[i].cur,
                                 pending[i].dst,
                                 units->has_ident[pending[i].cur],
                                 units->ident[pending[i].cur]);
            pending[i].active = false;
            /* It stays where it is, so it now blocks anyone aimed at its own
             * address. Re-run until that has propagated. */
            immovable |= ((uint64_t)1u << pending[i].cur);
            changed = true;
        }
    }

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
            if (!restore_add_move(plan, space, pending[i].cur, pending[i].dst, false)) {
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
         * Nothing can move directly, so everything left is a cycle. Vacate one
         * member to a free address; that frees the address someone else needed
         * and the rest unwinds. The staged unit is placed by a later pass.
         */
        uint8_t victim = RESTORE_NO_ADDRESS;
        for (uint8_t i = 0u; i < pending_count; i++) {
            if (pending[i].active) {
                victim = i;
                break;
            }
        }
        if (victim == RESTORE_NO_ADDRESS) {
            break;
        }

        uint64_t wanted = 0u;
        for (uint8_t i = 0u; i < pending_count; i++) {
            if (pending[i].active) {
                wanted |= ((uint64_t)1u << pending[i].dst);
            }
        }

        uint8_t stage = RESTORE_NO_ADDRESS;
        for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
            const uint64_t bit = ((uint64_t)1u << addr);
            if ((occupied & bit) == 0u && (wanted & bit) == 0u) {
                stage = addr;
                break;
            }
        }

        if (stage == RESTORE_NO_ADDRESS) {
            restore_add_conflict(plan,
                                 DALI_RESTORE_CONFLICT_NO_STAGING_ADDRESS,
                                 space,
                                 pending[victim].cur,
                                 pending[victim].dst,
                                 units->has_ident[pending[victim].cur],
                                 units->ident[pending[victim].cur]);
            plan->incomplete = true;
            break;
        }

        if (!restore_add_move(plan, space, pending[victim].cur, stage, true)) {
            return;
        }
        occupied &= ~((uint64_t)1u << pending[victim].cur);
        occupied |= ((uint64_t)1u << stage);
        pending[victim].cur = stage;
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
