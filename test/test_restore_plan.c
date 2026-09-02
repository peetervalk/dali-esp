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
    TEST_ASSERT_FALSE(s_plan.moves[0].is_staging);
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
    TEST_ASSERT_TRUE(s_plan.moves[0].is_staging);
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
     * a unit that will never move and is not the destination of anything, so a
     * planner that only checked "is anyone aiming at this?" would park the
     * staged unit on top of the squatter. The lowest legal choice is a3.
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
    TEST_ASSERT_TRUE(s_plan.moves[0].is_staging);
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
    record(DALI_SNAPSHOT_SPACE_GEAR, 9u, 1u);
    on_bus(1u, 1u);           /* wants 9 */
    on_bus(9u, 55u);          /* squatter, absent from the backup */

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_OCCUPIED));
    TEST_ASSERT_EQUAL_UINT16(1u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_UNKNOWN_UNIT));
}

void test_blocking_cascades_to_units_waiting_behind_the_blocked_one(void)
{
    /* 1 wants 9 (held by a squatter, so 1 stays); 2 wants 1, which 1 still
     * holds. Both must be reported rather than one being moved onto the other. */
    record(DALI_SNAPSHOT_SPACE_GEAR, 9u, 1u);
    record(DALI_SNAPSHOT_SPACE_GEAR, 1u, 2u);
    on_bus(1u, 1u);
    on_bus(2u, 2u);
    on_bus(9u, 55u);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_plan(&s_plan, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.move_count);
    TEST_ASSERT_EQUAL_UINT16(2u,
                             count_conflicts(&s_plan,
                                             DALI_RESTORE_CONFLICT_TARGET_OCCUPIED));
    replay_and_assert_safe(DALI_SNAPSHOT_SPACE_GEAR);
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
