#include "unity.h"
#include "dali_restore.h"

#include <string.h>

static DaliSnapshot           s_snapshot;
static DaliDiscoveryInventory s_inventory;
static DaliRestorePlan        s_plan;

void setUp(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(&s_inventory, 0, sizeof(s_inventory));
    memset(&s_plan, 0, sizeof(s_plan));
    dali_snapshot_reset(&s_snapshot);
    s_inventory.valid = true;
}

void tearDown(void) {}

/* Identification numbers are built from a seed so a test can say "unit 3" and
 * mean the same physical unit on both sides of the match. */
static void fill_identification(uint8_t *out, uint8_t seed)
{
    for (uint8_t i = 0u; i < DALI_MEMORY_BANK0_IDENTIFICATION_LEN; i++) {
        out[i] = (uint8_t)(0x40u + seed + i);
    }
}

static void record(DaliSnapshotSpace space, uint8_t addr, uint8_t seed)
{
    DaliSnapshotEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.space              = space;
    entry.short_address      = addr;
    entry.has_identification = true;
    fill_identification(entry.identification, seed);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));
}

static void record_without_identity(DaliSnapshotSpace space, uint8_t addr)
{
    DaliSnapshotEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.space         = space;
    entry.short_address = addr;
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));
}

static void on_bus(uint8_t addr, uint8_t seed)
{
    DaliDiscoveryDeviceInfo *device = &s_inventory.devices[addr];
    device->present          = true;
    device->has_control_gear = true;
    device->has_identity     = true;
    fill_identification(device->identity.serial, seed);
}

static void on_bus_without_identity(uint8_t addr)
{
    DaliDiscoveryDeviceInfo *device = &s_inventory.devices[addr];
    device->present          = true;
    device->has_control_gear = true;
    device->has_identity     = false;
}

/* Two or more units sharing one short address: reply-window activity that did
 * not decode. Deliberately not `present` — that is how the scan records it, and
 * the whole point is that nothing there can be read. */
static void contested_on_bus(uint8_t addr)
{
    s_inventory.devices[addr].has_undecodable_activity = true;
    s_inventory.undecodable_count++;
}

static void contested_device_on_bus(uint8_t addr)
{
    s_inventory.devices[addr].has_undecodable_device_activity = true;
    s_inventory.undecodable_device_count++;
}

static void device_on_bus(uint8_t addr)
{
    DaliDiscoveryDeviceInfo *device = &s_inventory.devices[addr];
    device->present          = true;
    device->has_input_device = true;
}

static void device_on_bus_with_identity(uint8_t addr, uint8_t seed)
{
    DaliDiscoveryDeviceInfo *device = &s_inventory.devices[addr];
    device->present             = true;
    device->has_input_device    = true;
    device->has_device_identity = true;
    fill_identification(device->device_identity.serial, seed);
}

static uint16_t count_conflicts(const DaliRestorePlan  *plan,
                                DaliRestoreConflictKind kind)
{
    uint16_t count = 0u;
    for (uint8_t i = 0u; i < plan->conflict_count; i++) {
        if (plan->conflicts[i].kind == kind) {
            count++;
        }
    }
    return count;
}

/*
 * Replay the plan against the starting bus and assert the invariant that makes a
 * restore safe: no move ever writes a unit onto an address something else still
 * holds. Then check every matched unit ended where the snapshot says.
 *
 * This is the test that matters. A plan can look plausible move by move and
 * still put two fixtures on one address halfway through, which is the one fault
 * nothing on the bus can undo remotely.
 */
static void replay_and_assert_safe(DaliSnapshotSpace space)
{
    /* occupant[addr] = identification seed + 1, or 0 for empty. */
    uint8_t occupant[DALI_SHORT_ADDRESS_COUNT];
    memset(occupant, 0, sizeof(occupant));

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device = &s_inventory.devices[addr];
        /*
         * A contested address holds units too — more than one, which is the
         * problem — so it starts occupied here. Without this the replay would
         * wave through the exact move this file exists to catch: a third unit
         * written onto an address that already answers as two.
         */
        const bool contested = (space == DALI_SNAPSHOT_SPACE_GEAR)
                                   ? device->has_undecodable_activity
                                   : device->has_undecodable_device_activity;
        if (contested) {
            occupant[addr] = (uint8_t)(addr + 1u);
            continue;
        }
        if (!device->present) {
            continue;
        }
        const bool in_space = (space == DALI_SNAPSHOT_SPACE_GEAR)
                                  ? device->has_control_gear
                                  : device->has_input_device;
        if (in_space) {
            occupant[addr] = (uint8_t)(addr + 1u);   /* identity by start address */
        }
    }

    for (uint8_t i = 0u; i < s_plan.move_count; i++) {
        const DaliRestoreMove *move = &s_plan.moves[i];
        if (move->space != space) {
            continue;
        }
        TEST_ASSERT_NOT_EQUAL_UINT8_MESSAGE(0u, occupant[move->from],
                                            "move from an empty address");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, occupant[move->to],
                                        "move onto an occupied address");
        occupant[move->to]   = occupant[move->from];
        occupant[move->from] = 0u;
    }
}

/*
 * Replay the plan and report where the unit that began at `start` ended up.
 *
 * Asserting on the end state rather than on the order of the move list keeps
 * these tests honest about what a restore promises: every unit at its recorded
 * address. Which member of a cycle gets staged, and in what order the rest
 * unwind, is the planner's business and may change.
 */
static uint8_t final_address_of(DaliSnapshotSpace space, uint8_t start)
{
    uint8_t where = start;
    for (uint8_t i = 0u; i < s_plan.move_count; i++) {
        const DaliRestoreMove *move = &s_plan.moves[i];
        if (move->space == space && move->from == where) {
            where = move->to;
        }
    }
    return where;
}

/* --------------------------------------------------------------------------
 * The quiet cases
 * -------------------------------------------------------------------------*/

void test_a_bus_matching_its_backup_needs_no_moves(void)
{
    record(DALI_SNAPSHOT_SPACE_GEAR, 0u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 1u, 2u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 5u, 3u);
    on_bus(0u, 1u);
    on_bus(1u, 2u);
    on_bus(5u, 3u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_TRUE(dali_restore_plan_is_clean(&s_plan));
    TEST_ASSERT_EQUAL_UINT8(3u, s_plan.matched_count);
    TEST_ASSERT_EQUAL_UINT8(3u, s_plan.already_correct_count);
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
}

void test_one_displaced_unit_produces_exactly_one_move(void)
{
    /* HW-3: a4 is free on the 2k bus, so a5 parked there comes straight back. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 5u, 1u);
    on_bus(4u, 1u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT8(4u, s_plan.moves[0].from);
    TEST_ASSERT_EQUAL_UINT8(5u, s_plan.moves[0].to);
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_MOVE_PLACE, s_plan.moves[0].kind);
    TEST_ASSERT_EQUAL_UINT16(0u, s_plan.conflict_total);
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
    TEST_ASSERT_EQUAL_UINT8(5u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 4u));
}

void test_a_chain_is_ordered_so_each_target_is_free_when_used(void)
{
    /* 1->2, 2->3, 3 free. Moving 1 first would collide with 2. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 2u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 3u, 2u);
    on_bus(1u, 1u);
    on_bus(2u, 2u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(2u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT8(2u, s_plan.moves[0].from);
    TEST_ASSERT_EQUAL_UINT8(3u, s_plan.moves[0].to);
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.moves[1].from);
    TEST_ASSERT_EQUAL_UINT8(2u, s_plan.moves[1].to);
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
}

/* --------------------------------------------------------------------------
 * Cycles
 * -------------------------------------------------------------------------*/

void test_a_two_cycle_is_broken_by_staging_through_a_free_address(void)
{
    /* The swap: unit at 5 belongs at 8, unit at 8 belongs at 5. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 8u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 5u, 2u);
    on_bus(5u, 1u);
    on_bus(8u, 2u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_FALSE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT8(3u, s_plan.move_count);   /* stage + two placements */
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_MOVE_STAGE, s_plan.moves[0].kind);
    TEST_ASSERT_EQUAL_UINT16(0u, s_plan.conflict_total);
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);

    /* And both really land where the backup says. */
    TEST_ASSERT_EQUAL_UINT8(8u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 5u));
    TEST_ASSERT_EQUAL_UINT8(5u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 8u));
}

void test_a_three_cycle_is_resolved_with_one_staging_hop(void)
{
    record(DALI_SNAPSHOT_SPACE_GEAR, 2u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 3u, 2u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 1u, 3u);
    on_bus(1u, 1u);
    on_bus(2u, 2u);
    on_bus(3u, 3u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_FALSE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT8(4u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(0u, s_plan.conflict_total);
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);

    TEST_ASSERT_EQUAL_UINT8(2u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 1u));
    TEST_ASSERT_EQUAL_UINT8(3u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 2u));
    TEST_ASSERT_EQUAL_UINT8(1u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 3u));
}

void test_staging_skips_an_occupied_address_to_reach_a_free_one(void)
{
    /*
     * The staging address must be free, not merely unwanted. Here a0 is held by
     * a squatter that is not the destination of anything, so a planner that only
     * checked "is anyone aiming at this?" would park the staged unit on top of
     * it. The lowest legal choice is a3.
     *
     * The squatter is also absent from the backup, so it could be moved aside —
     * and must not be, because nothing needs its address. Displacement is a
     * repair for a blocked move, not a tidy-up.
     */
    on_bus(0u, 55u);                            /* squatter, absent from backup */
    record(DALI_SNAPSHOT_SPACE_GEAR, 2u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 1u, 2u);
    on_bus(1u, 1u);
    on_bus(2u, 2u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_FALSE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT8(3u, s_plan.move_count);
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_MOVE_STAGE, s_plan.moves[0].kind);
    TEST_ASSERT_NOT_EQUAL_UINT8(0u, s_plan.moves[0].to);
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);

    TEST_ASSERT_EQUAL_UINT8(2u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 1u));
    TEST_ASSERT_EQUAL_UINT8(1u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 2u));
    /* And the squatter never moved. */
    TEST_ASSERT_EQUAL_UINT8(0u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 0u));
}

void test_a_cycle_with_no_free_address_fails_closed(void)
{
    /* Every address occupied and every unit displaced: nothing can be staged,
     * so the plan says so rather than emitting a colliding move. */
    for (uint16_t i = 0u; i < DALI_SHORT_ADDRESS_COUNT; i++) {
        const uint8_t addr = (uint8_t)i;
        const uint8_t dest = (uint8_t)((i + 1u) % DALI_SHORT_ADDRESS_COUNT);
        record(DALI_SNAPSHOT_SPACE_GEAR, dest, addr);
        on_bus(addr, addr);
    }

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_TRUE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_NO_STAGING_ADDRESS));
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
}

/* --------------------------------------------------------------------------
 * Conflicts
 * -------------------------------------------------------------------------*/

void test_a_unit_absent_from_the_backup_is_reported_and_left_alone(void)
{
    record(DALI_SNAPSHOT_SPACE_GEAR, 0u, 1u);
    on_bus(0u, 1u);
    on_bus(9u, 77u);          /* added since the backup */

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_UNKNOWN_UNIT));
}

void test_a_recorded_unit_missing_from_the_bus_is_reported(void)
{
    record(DALI_SNAPSHOT_SPACE_GEAR, 0u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 6u, 2u);
    on_bus(0u, 1u);           /* the unit recorded at 6 is powered down */

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan, DALI_RESTORE_CONFLICT_MISSING));
    TEST_ASSERT_EQUAL_UINT8(6u, s_plan.conflicts[0].address);
}

void test_a_unit_whose_identity_cannot_be_read_is_never_moved(void)
{
    record(DALI_SNAPSHOT_SPACE_GEAR, 3u, 1u);
    on_bus_without_identity(3u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_UNIDENTIFIED));
    /* The recorded unit is also unaccounted for, which is a separate fact. */
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan, DALI_RESTORE_CONFLICT_MISSING));
}

void test_a_backup_entry_without_an_anchor_is_reported_as_unidentified(void)
{
    /* Control devices before Phase 2: recorded, but with no identity to match. */
    record_without_identity(DALI_SNAPSHOT_SPACE_DEVICE, 0u);
    device_on_bus(0u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    /* One for the bus unit, one for the backup entry. */
    TEST_ASSERT_EQUAL_UINT16(2u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_UNIDENTIFIED));
}

void test_two_units_sharing_an_identification_number_are_never_moved(void)
{
    record(DALI_SNAPSHOT_SPACE_GEAR, 4u, 1u);
    on_bus(1u, 1u);
    on_bus(2u, 1u);           /* same identification number as a1 */

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(2u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_DUPLICATE_BUS));
}

void test_two_backup_entries_sharing_an_identification_number_block_the_move(void)
{
    record(DALI_SNAPSHOT_SPACE_GEAR, 4u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 7u, 1u);
    on_bus(1u, 1u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_DUPLICATE_SNAPSHOT));
}

void test_a_target_held_by_an_immovable_unit_is_reported_not_overwritten(void)
{
    /*
     * The squatter's identity cannot be read, so it cannot be moved aside: put
     * it anywhere and nothing could confirm which unit went where. That is the
     * distinction that decides whether a blocked move is repaired or reported —
     * readable identity, not membership in the backup.
     */
    record(DALI_SNAPSHOT_SPACE_GEAR, 9u, 1u);
    on_bus(1u, 1u);                 /* wants 9 */
    on_bus_without_identity(9u);    /* squatter, unreadable */

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_OCCUPIED));
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_UNIDENTIFIED));
}

void test_blocking_cascades_to_units_waiting_behind_the_blocked_one(void)
{
    /* 1 wants 9 (held by an unreadable squatter, so 1 stays); 2 wants 1, which 1
     * still holds. Both must be reported rather than one being moved onto the
     * other. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 9u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 1u, 2u);
    on_bus(1u, 1u);
    on_bus(2u, 2u);
    on_bus_without_identity(9u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(2u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_OCCUPIED));
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
}

/* --------------------------------------------------------------------------
 * Moving aside a unit the backup never saw
 * -------------------------------------------------------------------------*/

void test_a_unit_the_backup_never_saw_is_moved_aside_so_the_restore_converges(void)
{
    /*
     * The 2026-09-03 bus, exactly. An LED driver held a4 while unpowered
     * through every `backup save`, so it is in no snapshot; a collision and a
     * re-commission then left the unit recorded at a4 sitting on a5. The old
     * planner filed the driver as immovable and came back with zero moves and
     * two conflicts, and the operator did the staging by hand.
     */
    for (uint8_t addr = 0u; addr < 4u; addr++) {
        record(DALI_SNAPSHOT_SPACE_GEAR, addr, addr);
        on_bus(addr, addr);
    }
    record(DALI_SNAPSHOT_SPACE_GEAR, 4u, 100u);
    on_bus(5u, 100u);         /* recorded at a4, answering on a5 */
    on_bus(4u, 200u);         /* the driver no backup has ever seen */

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_FALSE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT8(2u, s_plan.move_count);

    /* Aside first, into the lowest address nothing wants, then the placement. */
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_MOVE_DISPLACE, s_plan.moves[0].kind);
    TEST_ASSERT_EQUAL_UINT8(4u, s_plan.moves[0].from);
    TEST_ASSERT_EQUAL_UINT8(6u, s_plan.moves[0].to);
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_MOVE_PLACE, s_plan.moves[1].kind);
    TEST_ASSERT_EQUAL_UINT8(5u, s_plan.moves[1].from);
    TEST_ASSERT_EQUAL_UINT8(4u, s_plan.moves[1].to);

    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
    TEST_ASSERT_EQUAL_UINT8(4u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 5u));
    TEST_ASSERT_EQUAL_UINT8(6u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 4u));

    /*
     * Moving it does not make it known. The operator still has to be told there
     * is gear on this bus that no backup accounts for, and the move alone would
     * not say so.
     */
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_UNKNOWN_UNIT));
    TEST_ASSERT_EQUAL_UINT16(0u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_OCCUPIED));
}

void test_moving_one_unit_aside_unblocks_the_chain_waiting_behind_it(void)
{
    /* a1 is unrecorded and in the way; a2 belongs at a1 and a3 belongs at a2.
     * One displacement has to release both. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 1u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 2u, 2u);
    on_bus(2u, 1u);
    on_bus(3u, 2u);
    on_bus(1u, 200u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_FALSE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT8(3u, s_plan.move_count);
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_MOVE_DISPLACE, s_plan.moves[0].kind);

    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
    TEST_ASSERT_EQUAL_UINT8(0u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 1u));
    TEST_ASSERT_EQUAL_UINT8(1u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 2u));
    TEST_ASSERT_EQUAL_UINT8(2u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 3u));
}

void test_a_cycle_is_broken_before_any_unit_is_moved_aside(void)
{
    /*
     * One address to spare, and two things that want it: a cycle needs it as a
     * staging hop and an unrecorded unit needs it to get out of the way. The
     * cycle must go first, because it hands the address back when it unwinds
     * and the displacement keeps it. Displacing first strands the cycle with
     * nowhere to stage and turns a restore that converges into an incomplete
     * plan.
     *
     * a0 and a1 are a swap; a2 holds an unrecorded unit; a3 belongs at a2;
     * a4..a62 are already correct; a63 is the only free address.
     */
    record(DALI_SNAPSHOT_SPACE_GEAR, 1u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 0u, 2u);
    on_bus(0u, 1u);
    on_bus(1u, 2u);

    on_bus(2u, 200u);                            /* unrecorded, in the way */
    record(DALI_SNAPSHOT_SPACE_GEAR, 2u, 3u);
    on_bus(3u, 3u);

    for (uint8_t addr = 4u; addr < 63u; addr++) {
        record(DALI_SNAPSHOT_SPACE_GEAR, addr, addr);
        on_bus(addr, addr);
    }

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_FALSE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT16(0u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_NO_STAGING_ADDRESS));
    TEST_ASSERT_EQUAL_UINT16(0u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_OCCUPIED));
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_MOVE_STAGE, s_plan.moves[0].kind);

    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
    TEST_ASSERT_EQUAL_UINT8(1u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 0u));
    TEST_ASSERT_EQUAL_UINT8(0u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 1u));
    TEST_ASSERT_EQUAL_UINT8(2u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 3u));
    TEST_ASSERT_EQUAL_UINT8(63u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 2u));
}

void test_a_unit_the_backup_never_saw_stays_put_when_the_space_is_full(void)
{
    /*
     * Every address occupied, so there is nowhere to move the blocker to. This
     * is the case where refusing really is the only safe answer, and it must
     * report exactly what it reported before displacement existed: the move
     * dropped as TARGET_OCCUPIED, and a plan that is merely conflicted rather
     * than incomplete, so the unambiguous part of a larger restore still runs.
     */
    on_bus(0u, 200u);                            /* unrecorded, holding a0 */
    record(DALI_SNAPSHOT_SPACE_GEAR, 0u, 1u);
    on_bus(1u, 1u);                              /* recorded at a0, sitting on a1 */

    for (uint16_t addr = 2u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        record(DALI_SNAPSHOT_SPACE_GEAR, (uint8_t)addr, (uint8_t)addr);
        on_bus((uint8_t)addr, (uint8_t)addr);
    }

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_FALSE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_OCCUPIED));
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_UNKNOWN_UNIT));
}

void test_a_unit_that_cannot_be_told_from_another_is_never_moved_aside(void)
{
    /*
     * Two units answering with one identification number, holding an address a
     * recorded unit is owed. Displacing one would move a fixture nothing could
     * afterwards confirm the identity of, so this keeps the refusal even though
     * the space has room.
     */
    record(DALI_SNAPSHOT_SPACE_GEAR, 9u, 1u);
    on_bus(1u, 1u);           /* wants 9 */
    on_bus(9u, 77u);
    on_bus(10u, 77u);         /* the same number as a9 */

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_OCCUPIED));
    TEST_ASSERT_EQUAL_UINT16(2u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_DUPLICATE_BUS));
}

/* --------------------------------------------------------------------------
 * Contested addresses
 *
 * An address that answered undecodably is occupied by two or more units that
 * answer as one. The scan deliberately does not mark it present — nothing there
 * can be read — and the danger is that "not present" reads as "free". Writing a
 * third unit onto it is the one fault a restore cannot undo by moving anything
 * back, so every route to an address has to see it: the direct placement, the
 * staging hop, and the displacement.
 * -------------------------------------------------------------------------*/

void test_a_move_onto_a_contested_address_is_reported_not_made(void)
{
    /* The backup says this unit belongs at a4, and a4 now answers as two. The
     * move must be dropped and named for what blocks it — the remedy is
     * `address a4 clear`, which no other conflict kind points at. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 4u, 1u);
    on_bus(1u, 1u);
    contested_on_bus(4u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_CONTESTED));
    TEST_ASSERT_EQUAL_UINT16(0u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_OCCUPIED));
    TEST_ASSERT_EQUAL_UINT8(4u, s_plan.conflicts[0].other_address);
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
}

void test_a_contested_address_is_never_borrowed_to_stage_a_cycle(void)
{
    /*
     * a0 and a1 are a swap, so the cycle needs somewhere to stage. Every other
     * address is occupied except a63, which is contested — free-looking to a
     * planner that only checks `present`, and the worst possible choice: the
     * staged unit would land on top of two units already fighting over it.
     * Failing closed is the only safe answer.
     */
    record(DALI_SNAPSHOT_SPACE_GEAR, 1u, 100u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 0u, 101u);
    on_bus(0u, 100u);
    on_bus(1u, 101u);

    /* Seeded away from the pair above: two units sharing an identification
     * number are a different conflict and would mask this one. */
    for (uint8_t addr = 2u; addr < 63u; addr++) {
        record(DALI_SNAPSHOT_SPACE_GEAR, addr, addr);
        on_bus(addr, addr);
    }
    contested_on_bus(63u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_TRUE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_NO_STAGING_ADDRESS));
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
}

void test_a_contested_address_is_never_used_to_displace_an_unrecorded_unit(void)
{
    /*
     * The same trap one route over. An unrecorded unit holds an address a
     * recorded one is owed, and the only address that is not `present` is
     * contested. Displacing onto it would put a third unit on a contested
     * address to repair a fault that was merely reportable, so the space counts
     * as full and the move is dropped exactly as it was before displacement
     * existed.
     */
    on_bus(0u, 200u);                            /* unrecorded, holding a0 */
    record(DALI_SNAPSHOT_SPACE_GEAR, 0u, 1u);
    on_bus(1u, 1u);                              /* recorded at a0, sitting on a1 */

    for (uint8_t addr = 2u; addr < 63u; addr++) {
        record(DALI_SNAPSHOT_SPACE_GEAR, addr, addr);
        on_bus(addr, addr);
    }
    contested_on_bus(63u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_FALSE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_OCCUPIED));
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_UNKNOWN_UNIT));
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
}

void test_a_contested_address_blocks_only_the_move_that_wants_it(void)
{
    /* One contested address must not cost the rest of the restore, for the
     * reason the scan keeps going past one: the other sixty-three are fine. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 4u, 1u);    /* blocked by the contest */
    on_bus(1u, 1u);
    contested_on_bus(4u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 9u, 2u);    /* unrelated, must still run */
    on_bus(8u, 2u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_FALSE(s_plan.incomplete);
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT8(8u, s_plan.moves[0].from);
    TEST_ASSERT_EQUAL_UINT8(9u, s_plan.moves[0].to);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_CONTESTED));
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
    TEST_ASSERT_EQUAL_UINT8(1u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 1u));
}

void test_a_contested_address_reserves_only_its_own_space(void)
{
    /*
     * Contested gear at a7 and a contested control device at d5. Neither may
     * reserve the other's numeric address: the spaces are independent, and
     * refusing a device move because gear collided at the same number would
     * turn one fault into two.
     */
    contested_on_bus(7u);
    contested_device_on_bus(5u);

    record(DALI_SNAPSHOT_SPACE_DEVICE, 7u, 1u);  /* device wants d7: free */
    device_on_bus_with_identity(2u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 5u, 2u);    /* gear wants a5: free */
    on_bus(3u, 2u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(2u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(0u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_CONTESTED));
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_DEVICE);
    TEST_ASSERT_EQUAL_UINT8(5u, final_address_of(DALI_SNAPSHOT_SPACE_GEAR, 3u));
    TEST_ASSERT_EQUAL_UINT8(7u, final_address_of(DALI_SNAPSHOT_SPACE_DEVICE, 2u));
}

void test_a_contested_device_address_blocks_a_device_move(void)
{
    /* The device space has no verb that de-addresses, but the planner must
     * still refuse: a contested d4 is as occupied as a contested a4. */
    record(DALI_SNAPSHOT_SPACE_DEVICE, 4u, 1u);
    device_on_bus_with_identity(1u, 1u);
    contested_device_on_bus(4u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_CONTESTED));
    TEST_ASSERT_EQUAL_INT(DALI_SNAPSHOT_SPACE_DEVICE, s_plan.conflicts[0].space);
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_DEVICE);
}

void test_a_hybrid_address_contested_in_one_space_still_plans_the_other(void)
{
    /*
     * The case only enrichment can produce: gear answers at a6 and is readable,
     * while the Part 103 probe at the same number draws undecodable activity.
     * The gear is planned normally; d6 is reserved.
     */
    on_bus(6u, 1u);
    s_inventory.devices[6u].has_undecodable_device_activity = true;
    s_inventory.undecodable_device_count++;

    record(DALI_SNAPSHOT_SPACE_GEAR, 2u, 1u);    /* the gear at a6 belongs at a2 */
    record(DALI_SNAPSHOT_SPACE_DEVICE, 6u, 9u);  /* a device believes it owns d6 */
    device_on_bus_with_identity(1u, 9u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.move_count);
    TEST_ASSERT_EQUAL_INT(DALI_SNAPSHOT_SPACE_GEAR, s_plan.moves[0].space);
    TEST_ASSERT_EQUAL_UINT8(6u, s_plan.moves[0].from);
    TEST_ASSERT_EQUAL_UINT8(2u, s_plan.moves[0].to);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_CONTESTED));
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_DEVICE);
}

/* --------------------------------------------------------------------------
 * Space independence
 * -------------------------------------------------------------------------*/

void test_the_two_address_spaces_are_planned_independently(void)
{
    /* Gear at 3 belongs at 7. A control device also sits at 7 — a different
     * unit in a different space, which must not block the gear move. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 7u, 1u);
    on_bus(3u, 1u);
    device_on_bus(7u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.move_count);
    TEST_ASSERT_EQUAL_INT(DALI_SNAPSHOT_SPACE_GEAR, s_plan.moves[0].space);
    TEST_ASSERT_EQUAL_UINT8(3u, s_plan.moves[0].from);
    TEST_ASSERT_EQUAL_UINT8(7u, s_plan.moves[0].to);
}

void test_a_control_device_is_restored_from_its_own_identity(void)
{
    record(DALI_SNAPSHOT_SPACE_DEVICE, 4u, 1u);
    device_on_bus_with_identity(9u, 1u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.move_count);
    TEST_ASSERT_EQUAL_INT(DALI_SNAPSHOT_SPACE_DEVICE, s_plan.moves[0].space);
    TEST_ASSERT_EQUAL_UINT8(9u, s_plan.moves[0].from);
    TEST_ASSERT_EQUAL_UINT8(4u, s_plan.moves[0].to);
    TEST_ASSERT_EQUAL_UINT16(0u, s_plan.conflict_total);
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_DEVICE);
}

void test_gear_and_device_identities_never_substitute_for_each_other(void)
{
    /*
     * The 2k finding: a0 and d0 report different Bank 0 identities and are
     * different physical units. Gear identity must not anchor a device entry,
     * nor the reverse — otherwise a restore invents a pairing the bus denies.
     */
    record(DALI_SNAPSHOT_SPACE_DEVICE, 7u, 1u);
    on_bus(3u, 1u);            /* gear carrying the device's recorded identity */

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.matched_count);
    /* The gear is unknown to the backup; the device entry has nothing on the bus. */
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_UNKNOWN_UNIT));
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan, DALI_RESTORE_CONFLICT_MISSING));
}

void test_one_number_in_two_spaces_is_two_independent_units(void)
{
    /* Exactly the a0/d0 case. Gear at 0 belongs at 2; a device also sits at 0
     * and at its own recorded address. Neither constrains the other. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 2u, 1u);
    record(DALI_SNAPSHOT_SPACE_DEVICE, 0u, 2u);
    on_bus(0u, 1u);
    device_on_bus_with_identity(0u, 2u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(2u, s_plan.matched_count);
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.already_correct_count);
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.move_count);
    TEST_ASSERT_EQUAL_INT(DALI_SNAPSHOT_SPACE_GEAR, s_plan.moves[0].space);
    TEST_ASSERT_EQUAL_UINT8(2u, s_plan.moves[0].to);
    TEST_ASSERT_EQUAL_UINT16(0u, s_plan.conflict_total);
}

void test_a_hybrid_unit_is_planned_once_per_space(void)
{
    /* The 2k bus's address 0: lamp plus Steinel on one physical unit. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 0u, 1u);
    record_without_identity(DALI_SNAPSHOT_SPACE_DEVICE, 0u);
    on_bus(0u, 1u);
    s_inventory.devices[0].has_input_device = true;

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.matched_count);
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.already_correct_count);
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    /* The device half cannot be anchored yet and says so, twice. */
    TEST_ASSERT_EQUAL_UINT16(2u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_UNIDENTIFIED));
}

/* --------------------------------------------------------------------------
 * Guards
 * -------------------------------------------------------------------------*/

void test_conflict_total_counts_past_the_stored_array(void)
{
    for (uint16_t i = 0u; i < DALI_SHORT_ADDRESS_COUNT; i++) {
        on_bus((uint8_t)i, (uint8_t)i);   /* none of them in the backup */
    }

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(DALI_RESTORE_MAX_CONFLICTS, s_plan.conflict_count);
    TEST_ASSERT_EQUAL_UINT16(DALI_SHORT_ADDRESS_COUNT, s_plan.conflict_total);
}

void test_planning_from_an_invalid_inventory_is_refused(void)
{
    record(DALI_SNAPSHOT_SPACE_GEAR, 0u, 1u);
    on_bus(0u, 1u);
    s_inventory.valid = false;

    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
}

void test_invalid_arguments_are_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_restore_plan(NULL, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_restore_plan(&s_plan, NULL, &s_inventory));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_restore_plan(&s_plan, &s_snapshot, NULL));
    TEST_ASSERT_FALSE(dali_restore_plan_is_clean(NULL));
}

void test_names_are_defined_for_every_conflict_kind_and_space(void)
{
    static const DaliRestoreConflictKind kinds[] = {
        DALI_RESTORE_CONFLICT_UNIDENTIFIED,
        DALI_RESTORE_CONFLICT_UNKNOWN_UNIT,
        DALI_RESTORE_CONFLICT_MISSING,
        DALI_RESTORE_CONFLICT_DUPLICATE_SNAPSHOT,
        DALI_RESTORE_CONFLICT_DUPLICATE_BUS,
        DALI_RESTORE_CONFLICT_TARGET_OCCUPIED,
        DALI_RESTORE_CONFLICT_TARGET_CONTESTED,
        DALI_RESTORE_CONFLICT_NO_STAGING_ADDRESS,
    };
    for (size_t i = 0u; i < (sizeof(kinds) / sizeof(kinds[0])); i++) {
        const char *name = dali_restore_conflict_name(kinds[i]);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(name[0] != '\0');
    }
    TEST_ASSERT_EQUAL_STRING("gear",
                             dali_restore_space_name(DALI_SNAPSHOT_SPACE_GEAR));
    TEST_ASSERT_EQUAL_STRING("device",
                             dali_restore_space_name(DALI_SNAPSHOT_SPACE_DEVICE));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_bus_matching_its_backup_needs_no_moves);
    RUN_TEST(test_one_displaced_unit_produces_exactly_one_move);
    RUN_TEST(test_a_chain_is_ordered_so_each_target_is_free_when_used);
    RUN_TEST(test_a_two_cycle_is_broken_by_staging_through_a_free_address);
    RUN_TEST(test_a_three_cycle_is_resolved_with_one_staging_hop);
    RUN_TEST(test_staging_skips_an_occupied_address_to_reach_a_free_one);
    RUN_TEST(test_a_cycle_with_no_free_address_fails_closed);
    RUN_TEST(test_a_unit_absent_from_the_backup_is_reported_and_left_alone);
    RUN_TEST(test_a_recorded_unit_missing_from_the_bus_is_reported);
    RUN_TEST(test_a_unit_whose_identity_cannot_be_read_is_never_moved);
    RUN_TEST(test_a_backup_entry_without_an_anchor_is_reported_as_unidentified);
    RUN_TEST(test_two_units_sharing_an_identification_number_are_never_moved);
    RUN_TEST(test_two_backup_entries_sharing_an_identification_number_block_the_move);
    RUN_TEST(test_a_target_held_by_an_immovable_unit_is_reported_not_overwritten);
    RUN_TEST(test_blocking_cascades_to_units_waiting_behind_the_blocked_one);
    RUN_TEST(test_a_unit_the_backup_never_saw_is_moved_aside_so_the_restore_converges);
    RUN_TEST(test_moving_one_unit_aside_unblocks_the_chain_waiting_behind_it);
    RUN_TEST(test_a_cycle_is_broken_before_any_unit_is_moved_aside);
    RUN_TEST(test_a_unit_the_backup_never_saw_stays_put_when_the_space_is_full);
    RUN_TEST(test_a_unit_that_cannot_be_told_from_another_is_never_moved_aside);
    RUN_TEST(test_a_move_onto_a_contested_address_is_reported_not_made);
    RUN_TEST(test_a_contested_address_is_never_borrowed_to_stage_a_cycle);
    RUN_TEST(test_a_contested_address_is_never_used_to_displace_an_unrecorded_unit);
    RUN_TEST(test_a_contested_address_blocks_only_the_move_that_wants_it);
    RUN_TEST(test_a_contested_address_reserves_only_its_own_space);
    RUN_TEST(test_a_contested_device_address_blocks_a_device_move);
    RUN_TEST(test_a_hybrid_address_contested_in_one_space_still_plans_the_other);
    RUN_TEST(test_the_two_address_spaces_are_planned_independently);
    RUN_TEST(test_a_control_device_is_restored_from_its_own_identity);
    RUN_TEST(test_gear_and_device_identities_never_substitute_for_each_other);
    RUN_TEST(test_one_number_in_two_spaces_is_two_independent_units);
    RUN_TEST(test_a_hybrid_unit_is_planned_once_per_space);
    RUN_TEST(test_conflict_total_counts_past_the_stored_array);
    RUN_TEST(test_planning_from_an_invalid_inventory_is_refused);
    RUN_TEST(test_invalid_arguments_are_rejected);
    RUN_TEST(test_names_are_defined_for_every_conflict_kind_and_space);
    return UNITY_END();
}
