#include "unity.h"
#include "dali_restore.h"

#include <string.h>

static DaliSnapshot           s_snapshot;
static DaliDiscoveryInventory s_inventory;
static DaliRestoreGroupPlan   s_plan;

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

/* Backup: gear `seed` was at `addr` and belonged to `groups`. */
static void record(uint8_t addr, uint8_t seed, uint16_t groups)
{
    DaliSnapshotEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.space              = DALI_SNAPSHOT_SPACE_GEAR;
    entry.short_address      = addr;
    entry.has_identification = true;
    entry.has_groups         = true;
    entry.groups             = groups;
    fill_identification(entry.identification, seed);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));
}

/* Backup taken by a scan whose QUERY GROUPS never answered for this gear. */
static void record_without_groups(uint8_t addr, uint8_t seed)
{
    DaliSnapshotEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.space              = DALI_SNAPSHOT_SPACE_GEAR;
    entry.short_address      = addr;
    entry.has_identification = true;
    fill_identification(entry.identification, seed);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));
}

/* Bus: gear `seed` answers at `addr` and currently belongs to `groups`. */
static void on_bus(uint8_t addr, uint8_t seed, uint16_t groups)
{
    DaliDiscoveryDeviceInfo *device = &s_inventory.devices[addr];
    device->present          = true;
    device->has_control_gear = true;
    device->has_identity     = true;
    device->has_groups       = true;
    device->groups           = groups;
    fill_identification(device->identity.serial, seed);
}

static void on_bus_without_groups(uint8_t addr, uint8_t seed)
{
    DaliDiscoveryDeviceInfo *device = &s_inventory.devices[addr];
    device->present          = true;
    device->has_control_gear = true;
    device->has_identity     = true;
    device->has_groups       = false;
    fill_identification(device->identity.serial, seed);
}

static const DaliRestoreGroupChange *change_for(uint8_t addr)
{
    for (uint8_t i = 0u; i < s_plan.change_count; i++) {
        if (s_plan.changes[i].address == addr) {
            return &s_plan.changes[i];
        }
    }
    return NULL;
}

static uint16_t count_conflicts(DaliRestoreConflictKind kind)
{
    uint16_t count = 0u;
    for (uint8_t i = 0u; i < s_plan.conflict_count; i++) {
        if (s_plan.conflicts[i].kind == kind) {
            count++;
        }
    }
    return count;
}

static void plan_ok(void)
{
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_restore_plan_groups(&s_plan, &s_snapshot, &s_inventory));
}

/* --------------------------------------------------------------------------
 * The ordinary cases
 * -------------------------------------------------------------------------*/

void test_membership_matching_the_backup_needs_no_edits(void)
{
    record(0u, 1u, 0x0006u);   /* groups 1 and 2 */
    record(1u, 2u, 0x0000u);
    on_bus(0u, 1u, 0x0006u);
    on_bus(1u, 2u, 0x0000u);

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT8(2u, s_plan.matched_count);
    TEST_ASSERT_EQUAL_UINT8(2u, s_plan.already_correct_count);
    TEST_ASSERT_EQUAL_UINT16(0u, s_plan.conflict_total);
    TEST_ASSERT_TRUE(dali_restore_group_plan_is_clean(&s_plan));
}

void test_a_wiped_gear_is_planned_back_into_every_recorded_group(void)
{
    /* The RESET case this exists for: membership gone, backup intact. */
    record(0u, 1u, 0x0006u);
    on_bus(0u, 1u, 0x0000u);

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.change_count);
    const DaliRestoreGroupChange *change = change_for(0u);
    TEST_ASSERT_NOT_NULL(change);
    TEST_ASSERT_EQUAL_UINT16(0x0006u, change->add_mask);
    TEST_ASSERT_EQUAL_UINT16(0x0000u, change->remove_mask);
    TEST_ASSERT_EQUAL_UINT16(0x0000u, change->current);
    TEST_ASSERT_EQUAL_UINT16(0x0006u, change->recorded);
    TEST_ASSERT_EQUAL_UINT16(0u, s_plan.conflict_total);
    TEST_ASSERT_FALSE(dali_restore_group_plan_is_clean(&s_plan));
}

void test_the_two_masks_are_the_exact_difference_in_both_directions(void)
{
    /* In 0 and 1, recorded in 1 and 4: join 4, leave 0, and 1 is untouched.
     * A restore that re-sent every recorded bit would land in the same place
     * but would put unnecessary unacknowledged writes on the bus. */
    record(3u, 7u, 0x0012u);   /* groups 1 and 4 */
    on_bus(3u, 7u, 0x0003u);   /* groups 0 and 1 */

    plan_ok();

    const DaliRestoreGroupChange *change = change_for(3u);
    TEST_ASSERT_NOT_NULL(change);
    TEST_ASSERT_EQUAL_UINT16(0x0010u, change->add_mask);
    TEST_ASSERT_EQUAL_UINT16(0x0001u, change->remove_mask);
}

void test_group_15_is_not_lost_off_the_top_of_the_mask(void)
{
    record(0u, 1u, 0x8000u);
    on_bus(0u, 1u, 0x0000u);

    plan_ok();

    const DaliRestoreGroupChange *change = change_for(0u);
    TEST_ASSERT_NOT_NULL(change);
    TEST_ASSERT_EQUAL_UINT16(0x8000u, change->add_mask);
    TEST_ASSERT_EQUAL_UINT16(0x0000u, change->remove_mask);
}

/* --------------------------------------------------------------------------
 * Independence from the address restore
 * -------------------------------------------------------------------------*/

void test_edits_are_addressed_to_where_the_gear_answers_now(void)
{
    /*
     * The whole reason this is a separate plan: it is correct on a bus whose
     * addresses have not been restored. The gear recorded at a0 now answers at
     * a9, and the edits must go to a9 -- sending them to a0 would rewrite a
     * different fixture's membership.
     */
    record(0u, 1u, 0x0004u);
    on_bus(9u, 1u, 0x0000u);

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT8(9u, s_plan.changes[0].address);
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.changes[0].recorded_address);
    TEST_ASSERT_EQUAL_UINT16(0x0004u, s_plan.changes[0].add_mask);
}

void test_a_displaced_gear_whose_groups_are_intact_needs_no_edits(void)
{
    /* Commissioning moved it but did not touch its memory, which is the normal
     * outcome and must produce no group traffic at all. */
    record(0u, 1u, 0x0006u);
    on_bus(9u, 1u, 0x0006u);

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.already_correct_count);
    TEST_ASSERT_TRUE(dali_restore_group_plan_is_clean(&s_plan));
}

/* --------------------------------------------------------------------------
 * Refusing to guess
 * -------------------------------------------------------------------------*/

void test_a_backup_without_group_data_never_empties_the_gear(void)
{
    /*
     * The dangerous case. The gear is matched and is in groups 1 and 2, but the
     * backup never read its membership. Diffing against an absent mask would
     * emit REMOVE FROM GROUP for both.
     */
    record_without_groups(0u, 1u);
    on_bus(0u, 1u, 0x0006u);

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.matched_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
        count_conflicts(DALI_RESTORE_CONFLICT_NO_RECORDED_GROUPS));
}

void test_unreadable_current_membership_is_reported_not_written_blind(void)
{
    record(0u, 1u, 0x0006u);
    on_bus_without_groups(0u, 1u);

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
        count_conflicts(DALI_RESTORE_CONFLICT_GROUPS_UNREADABLE));
}

void test_a_gear_absent_from_the_backup_is_left_alone(void)
{
    record(0u, 1u, 0x0002u);
    on_bus(0u, 1u, 0x0002u);
    on_bus(5u, 9u, 0x0008u);   /* added since the backup */

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
        count_conflicts(DALI_RESTORE_CONFLICT_UNKNOWN_UNIT));
}

void test_a_recorded_gear_missing_from_the_bus_is_reported(void)
{
    record(0u, 1u, 0x0002u);
    record(1u, 2u, 0x0004u);
    on_bus(0u, 1u, 0x0002u);

    plan_ok();

    TEST_ASSERT_EQUAL_UINT16(1u, count_conflicts(DALI_RESTORE_CONFLICT_MISSING));
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.conflicts[0].address);
}

void test_two_gear_sharing_an_identification_number_are_never_edited(void)
{
    record(0u, 1u, 0x0002u);
    on_bus(0u, 1u, 0x0000u);
    on_bus(4u, 1u, 0x0000u);   /* same identity, so neither can be told apart */

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT16(2u,
        count_conflicts(DALI_RESTORE_CONFLICT_DUPLICATE_BUS));
}

void test_gear_whose_identity_cannot_be_read_is_never_edited(void)
{
    record(0u, 1u, 0x0002u);

    DaliDiscoveryDeviceInfo *device = &s_inventory.devices[0];
    device->present          = true;
    device->has_control_gear = true;
    device->has_identity     = false;
    device->has_groups       = true;
    device->groups           = 0x0000u;

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.matched_count);
    TEST_ASSERT_EQUAL_UINT16(1u,
        count_conflicts(DALI_RESTORE_CONFLICT_UNIDENTIFIED));
}

/* --------------------------------------------------------------------------
 * Scope
 * -------------------------------------------------------------------------*/

void test_control_devices_are_not_considered_at_all(void)
{
    /* A Part 103 device shares the numeric address space but has its own group
     * scheme that nothing here reads. It must not be matched to a gear entry,
     * and must not appear as a conflict either. */
    DaliSnapshotEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.space              = DALI_SNAPSHOT_SPACE_DEVICE;
    entry.short_address      = 2u;
    entry.has_identification = true;
    fill_identification(entry.identification, 3u);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));

    DaliDiscoveryDeviceInfo *device = &s_inventory.devices[2];
    device->present             = true;
    device->has_input_device    = true;
    device->has_device_identity = true;
    fill_identification(device->device_identity.serial, 3u);

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT8(0u, s_plan.matched_count);
    TEST_ASSERT_EQUAL_UINT16(0u, s_plan.conflict_total);
}

void test_a_hybrid_unit_is_planned_from_its_gear_half_only(void)
{
    record(0u, 1u, 0x0006u);

    DaliSnapshotEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.space              = DALI_SNAPSHOT_SPACE_DEVICE;
    entry.short_address      = 0u;
    entry.has_identification = true;
    fill_identification(entry.identification, 1u);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));

    on_bus(0u, 1u, 0x0000u);
    s_inventory.devices[0].has_input_device    = true;
    s_inventory.devices[0].has_device_identity = true;
    fill_identification(s_inventory.devices[0].device_identity.serial, 1u);

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT8(1u, s_plan.matched_count);
    TEST_ASSERT_EQUAL_UINT16(0x0006u, s_plan.changes[0].add_mask);
    TEST_ASSERT_EQUAL_UINT16(0u, s_plan.conflict_total);
}

/* --------------------------------------------------------------------------
 * Bounds and arguments
 * -------------------------------------------------------------------------*/

void test_a_full_bus_of_wiped_gear_fits_the_change_list(void)
{
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        record(addr, (uint8_t)(addr + 1u), 0x0001u);
        on_bus(addr, (uint8_t)(addr + 1u), 0x0000u);
    }

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(DALI_SHORT_ADDRESS_COUNT, s_plan.change_count);
    TEST_ASSERT_EQUAL_UINT8(DALI_SHORT_ADDRESS_COUNT, s_plan.matched_count);
    TEST_ASSERT_EQUAL_UINT16(0u, s_plan.conflict_total);
}

void test_conflict_total_counts_past_the_stored_array(void)
{
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        on_bus(addr, (uint8_t)(addr + 1u), 0x0000u);   /* none in the backup */
    }

    plan_ok();

    TEST_ASSERT_EQUAL_UINT8(DALI_RESTORE_MAX_CONFLICTS, s_plan.conflict_count);
    TEST_ASSERT_EQUAL_UINT16(DALI_SHORT_ADDRESS_COUNT, s_plan.conflict_total);
    TEST_ASSERT_FALSE(dali_restore_group_plan_is_clean(&s_plan));
}

void test_planning_from_an_invalid_inventory_is_refused(void)
{
    record(0u, 1u, 0x0002u);
    on_bus(0u, 1u, 0x0000u);
    s_inventory.valid = false;

    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_restore_plan_groups(&s_plan, &s_snapshot, &s_inventory));
}

void test_invalid_arguments_are_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_restore_plan_groups(NULL, &s_snapshot, &s_inventory));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_restore_plan_groups(&s_plan, NULL, &s_inventory));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_restore_plan_groups(&s_plan, &s_snapshot, NULL));
    TEST_ASSERT_FALSE(dali_restore_group_plan_is_clean(NULL));
}

void test_names_are_defined_for_the_group_conflict_kinds(void)
{
    static const DaliRestoreConflictKind kinds[] = {
        DALI_RESTORE_CONFLICT_NO_RECORDED_GROUPS,
        DALI_RESTORE_CONFLICT_GROUPS_UNREADABLE,
    };
    for (size_t i = 0u; i < (sizeof(kinds) / sizeof(kinds[0])); i++) {
        const char *name = dali_restore_conflict_name(kinds[i]);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(name[0] != '\0');
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_membership_matching_the_backup_needs_no_edits);
    RUN_TEST(test_a_wiped_gear_is_planned_back_into_every_recorded_group);
    RUN_TEST(test_the_two_masks_are_the_exact_difference_in_both_directions);
    RUN_TEST(test_group_15_is_not_lost_off_the_top_of_the_mask);
    RUN_TEST(test_edits_are_addressed_to_where_the_gear_answers_now);
    RUN_TEST(test_a_displaced_gear_whose_groups_are_intact_needs_no_edits);
    RUN_TEST(test_a_backup_without_group_data_never_empties_the_gear);
    RUN_TEST(test_unreadable_current_membership_is_reported_not_written_blind);
    RUN_TEST(test_a_gear_absent_from_the_backup_is_left_alone);
    RUN_TEST(test_a_recorded_gear_missing_from_the_bus_is_reported);
    RUN_TEST(test_two_gear_sharing_an_identification_number_are_never_edited);
    RUN_TEST(test_gear_whose_identity_cannot_be_read_is_never_edited);
    RUN_TEST(test_control_devices_are_not_considered_at_all);
    RUN_TEST(test_a_hybrid_unit_is_planned_from_its_gear_half_only);
    RUN_TEST(test_a_full_bus_of_wiped_gear_fits_the_change_list);
    RUN_TEST(test_conflict_total_counts_past_the_stored_array);
    RUN_TEST(test_planning_from_an_invalid_inventory_is_refused);
    RUN_TEST(test_invalid_arguments_are_rejected);
    RUN_TEST(test_names_are_defined_for_the_group_conflict_kinds);
    return UNITY_END();
}
