#include "unity.h"
#include "dali_commissioning_audit.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Inventory builders
 *
 * The audit reads a completed DaliDiscoveryInventory, so the vectors build one
 * by hand rather than driving a scan: what is under test is the comparison, and
 * the scan that produces these records has its own suite.
 * --------------------------------------------------------------------------*/

static DaliDiscoveryInventory s_inventory;

static void inventory_begin(void)
{
    memset(&s_inventory, 0, sizeof(s_inventory));
    s_inventory.valid = true;
}

static void put_gear(uint8_t addr)
{
    s_inventory.devices[addr].present = true;
    s_inventory.devices[addr].has_control_gear = true;
    s_inventory.found_count++;
}

static void put_device(uint8_t addr)
{
    s_inventory.devices[addr].present = true;
    s_inventory.devices[addr].has_input_device = true;
    s_inventory.found_count++;
}

static void put_gear_contested(uint8_t addr)
{
    s_inventory.devices[addr].has_undecodable_activity = true;
    s_inventory.undecodable_count++;
}

static void put_device_contested(uint8_t addr)
{
    s_inventory.devices[addr].has_undecodable_device_activity = true;
    s_inventory.undecodable_device_count++;
}

static DaliCommissioningOccupancy occupancy_of(DaliCommissioningAddressSpace space)
{
    DaliCommissioningOccupancy occ;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_occupancy_from_inventory(&s_inventory, space, &occ));
    return occ;
}

/* An assignment list from a plain address list; only short_address is read. */
static uint8_t build_assignments(DaliCommissioningAssignment *out,
                                 const uint8_t *addresses,
                                 uint8_t count)
{
    memset(out, 0, sizeof(*out) * count);
    for (uint8_t i = 0u; i < count; i++) {
        out[i].short_address = addresses[i];
        out[i].random_address = 0x100000u + i;
    }
    return count;
}

#define MASK(addr) ((uint64_t)1u << (addr))

/* ---------------------------------------------------------------------------
 * Occupancy extraction
 * --------------------------------------------------------------------------*/

static void test_gear_occupancy_reads_only_the_gear_fields(void)
{
    inventory_begin();
    put_gear(3u);
    put_device(7u);          /* device space: invisible to a gear audit */
    put_gear_contested(11u);
    put_device_contested(13u);

    DaliCommissioningOccupancy occ = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);
    TEST_ASSERT_EQUAL_HEX64(MASK(3u), occ.occupied);
    TEST_ASSERT_EQUAL_HEX64(MASK(11u), occ.contested);
}

static void test_device_occupancy_reads_only_the_device_fields(void)
{
    inventory_begin();
    put_gear(3u);
    put_device(7u);
    put_gear_contested(11u);
    put_device_contested(13u);

    DaliCommissioningOccupancy occ =
        occupancy_of(DALI_COMMISSIONING_SPACE_DEVICE);
    TEST_ASSERT_EQUAL_HEX64(MASK(7u), occ.occupied);
    TEST_ASSERT_EQUAL_HEX64(MASK(13u), occ.contested);
}

/*
 * The hybrid case, and the reason contested wins over occupied.
 *
 * One physical unit answers as control gear at a numeric address and as a
 * control device at the same number. A second control device collides with it
 * in device space. The gear reading stays good; the device reading must not be
 * reported as one readable device.
 */
static void test_contested_wins_over_occupied_in_the_same_record(void)
{
    inventory_begin();
    put_gear(9u);
    put_device(9u);
    put_device_contested(9u);

    DaliCommissioningOccupancy gear = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);
    TEST_ASSERT_EQUAL_HEX64(MASK(9u), gear.occupied);
    TEST_ASSERT_EQUAL_HEX64(0u, gear.contested);

    DaliCommissioningOccupancy dev =
        occupancy_of(DALI_COMMISSIONING_SPACE_DEVICE);
    TEST_ASSERT_EQUAL_HEX64(0u, dev.occupied);
    TEST_ASSERT_EQUAL_HEX64(MASK(9u), dev.contested);
}

static void test_occupancy_rejects_bad_arguments(void)
{
    DaliCommissioningOccupancy occ;
    inventory_begin();

    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_commissioning_occupancy_from_inventory(NULL,
                                                    DALI_COMMISSIONING_SPACE_GEAR,
                                                    &occ));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_commissioning_occupancy_from_inventory(&s_inventory,
                                                    DALI_COMMISSIONING_SPACE_GEAR,
                                                    NULL));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_commissioning_occupancy_from_inventory(
            &s_inventory, (DaliCommissioningAddressSpace)7, &occ));
}

/* ---------------------------------------------------------------------------
 * The diff
 * --------------------------------------------------------------------------*/

/* The good run: three assignments, three readable addresses, nothing else. */
static void test_a_clean_run_confirms_every_assignment(void)
{
    inventory_begin();
    put_gear(0u);   /* already commissioned before the run */
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear(0u);
    put_gear(1u);
    put_gear(2u);
    put_gear(3u);
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    const uint8_t addrs[] = { 1u, 2u, 3u };
    DaliCommissioningAssignment assignments[3];
    uint8_t count = build_assignments(assignments, addrs, 3u);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, assignments, count, &audit));

    TEST_ASSERT_EQUAL_UINT8(3u, audit.assigned_count);
    TEST_ASSERT_EQUAL_UINT8(3u, audit.confirmed_count);
    TEST_ASSERT_EQUAL_HEX64(MASK(1u) | MASK(2u) | MASK(3u), audit.confirmed);
    TEST_ASSERT_EQUAL_UINT8(0u, audit.contested_count);
    TEST_ASSERT_EQUAL_UINT8(0u, audit.silent_count);
    TEST_ASSERT_EQUAL_UINT8(0u, audit.unrecorded_count);
    TEST_ASSERT_EQUAL_UINT8(0u, audit.newly_contested_count);
    TEST_ASSERT_TRUE(dali_commissioning_audit_is_clean(&audit));
}

/*
 * The collision the whole module exists for.
 *
 * Two gear generate the same 24-bit random address, are selected, programmed
 * and withdrawn as one, and the walk records a single assignment. The post-scan
 * draws two overlapping replies at that address.
 */
static void test_equal_random_address_collision_reads_as_contested(void)
{
    inventory_begin();
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear(0u);
    put_gear_contested(1u);
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    const uint8_t addrs[] = { 0u, 1u };
    DaliCommissioningAssignment assignments[2];
    uint8_t count = build_assignments(assignments, addrs, 2u);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, assignments, count, &audit));

    TEST_ASSERT_EQUAL_HEX64(MASK(0u), audit.confirmed);
    TEST_ASSERT_EQUAL_HEX64(MASK(1u), audit.contested);
    TEST_ASSERT_EQUAL_UINT8(0u, audit.silent_count);
    TEST_ASSERT_FALSE(dali_commissioning_audit_is_clean(&audit));
}

/* An address the pre-scan already found contested is not damage this run did:
 * it was held out of the free pool and never assigned. */
static void test_a_pre_existing_contested_address_is_not_reported(void)
{
    inventory_begin();
    put_gear_contested(5u);
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear_contested(5u);
    put_gear(0u);
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    const uint8_t addrs[] = { 0u };
    DaliCommissioningAssignment assignments[1];
    uint8_t count = build_assignments(assignments, addrs, 1u);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, assignments, count, &audit));

    TEST_ASSERT_EQUAL_UINT8(1u, audit.confirmed_count);
    TEST_ASSERT_EQUAL_UINT8(0u, audit.newly_contested_count);
    TEST_ASSERT_TRUE(dali_commissioning_audit_is_clean(&audit));
}

/* An address that became contested during the run and that the run never
 * assigned: the bus changed underneath the walk. */
static void test_a_newly_contested_unassigned_address_is_reported(void)
{
    inventory_begin();
    put_gear(5u);
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear_contested(5u);
    put_gear(0u);
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    const uint8_t addrs[] = { 0u };
    DaliCommissioningAssignment assignments[1];
    uint8_t count = build_assignments(assignments, addrs, 1u);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, assignments, count, &audit));

    TEST_ASSERT_EQUAL_HEX64(MASK(5u), audit.newly_contested);
    TEST_ASSERT_EQUAL_UINT8(0u, audit.unrecorded_count);
    TEST_ASSERT_FALSE(dali_commissioning_audit_is_clean(&audit));
}

/*
 * The failed-run finding.
 *
 * The walk aborted between PROGRAM SHORT ADDRESS and the assignment record --
 * VERIFY answered silent, or QUERY SHORT ADDRESS came back as a different
 * address -- so a2 is addressed and the result does not mention it. Nothing but
 * this diff can see it, and until the failure path ran a post-scan, nothing did.
 */
static void test_an_address_written_but_not_recorded_is_reported(void)
{
    inventory_begin();
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear(0u);
    put_gear(1u);
    put_gear(2u);
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    const uint8_t addrs[] = { 0u, 1u };
    DaliCommissioningAssignment assignments[2];
    uint8_t count = build_assignments(assignments, addrs, 2u);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, assignments, count, &audit));

    TEST_ASSERT_EQUAL_UINT8(2u, audit.confirmed_count);
    TEST_ASSERT_EQUAL_HEX64(MASK(2u), audit.unrecorded);
    TEST_ASSERT_FALSE(dali_commissioning_audit_is_clean(&audit));
}

/* Gear that was already addressed before the run is not "unrecorded": the walk
 * never assigns an occupied address, so its presence says nothing. */
static void test_pre_existing_gear_is_not_mistaken_for_an_unrecorded_write(void)
{
    inventory_begin();
    put_gear(40u);
    put_gear(41u);
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear(40u);
    put_gear(41u);
    put_gear(0u);
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    const uint8_t addrs[] = { 0u };
    DaliCommissioningAssignment assignments[1];
    uint8_t count = build_assignments(assignments, addrs, 1u);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, assignments, count, &audit));

    TEST_ASSERT_EQUAL_UINT8(0u, audit.unrecorded_count);
    TEST_ASSERT_TRUE(dali_commissioning_audit_is_clean(&audit));
}

/* An address that was contested before and reads as one unit now changed, but
 * not because the walk wrote to it -- it was never offered as free. */
static void test_a_contested_address_that_resolves_is_not_an_unrecorded_write(void)
{
    inventory_begin();
    put_gear_contested(6u);
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear(6u);
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, NULL, 0u, &audit));

    TEST_ASSERT_EQUAL_UINT8(0u, audit.unrecorded_count);
    TEST_ASSERT_TRUE(dali_commissioning_audit_is_clean(&audit));
}

/* VERIFY confirmed the write and the post-scan hears nothing back. */
static void test_an_assignment_that_stops_answering_reads_as_silent(void)
{
    inventory_begin();
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear(0u);
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    const uint8_t addrs[] = { 0u, 1u };
    DaliCommissioningAssignment assignments[2];
    uint8_t count = build_assignments(assignments, addrs, 2u);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, assignments, count, &audit));

    TEST_ASSERT_EQUAL_HEX64(MASK(1u), audit.silent);
    TEST_ASSERT_EQUAL_UINT8(1u, audit.confirmed_count);
    TEST_ASSERT_FALSE(dali_commissioning_audit_is_clean(&audit));
}

/* The three assignment masks partition the claimed addresses: every assigned
 * address lands in exactly one, and their counts add up. */
static void test_the_assignment_masks_partition_the_claimed_addresses(void)
{
    inventory_begin();
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear(0u);
    put_gear_contested(1u);
    /* a2 answers nothing at all */
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    const uint8_t addrs[] = { 0u, 1u, 2u };
    DaliCommissioningAssignment assignments[3];
    uint8_t count = build_assignments(assignments, addrs, 3u);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, assignments, count, &audit));

    const uint64_t claimed = MASK(0u) | MASK(1u) | MASK(2u);
    TEST_ASSERT_EQUAL_HEX64(claimed,
        audit.confirmed | audit.contested | audit.silent);
    TEST_ASSERT_EQUAL_HEX64(0u, audit.confirmed & audit.contested);
    TEST_ASSERT_EQUAL_HEX64(0u, audit.confirmed & audit.silent);
    TEST_ASSERT_EQUAL_HEX64(0u, audit.contested & audit.silent);
    TEST_ASSERT_EQUAL_UINT8(3u, audit.assigned_count);
    TEST_ASSERT_EQUAL_UINT8(audit.assigned_count,
        (uint8_t)(audit.confirmed_count + audit.contested_count +
                  audit.silent_count));
}

/* A device-space audit reads the device fields end to end, so a Part 103 walk
 * cannot be graded against gear occupancy by accident. */
static void test_a_device_walk_is_audited_in_device_space(void)
{
    inventory_begin();
    DaliCommissioningOccupancy pre =
        occupancy_of(DALI_COMMISSIONING_SPACE_DEVICE);

    inventory_begin();
    put_device(0u);
    put_device_contested(1u);
    put_gear(2u);   /* gear at d2's number: not a control device */
    DaliCommissioningOccupancy post =
        occupancy_of(DALI_COMMISSIONING_SPACE_DEVICE);

    const uint8_t addrs[] = { 0u, 1u, 2u };
    DaliCommissioningAssignment assignments[3];
    uint8_t count = build_assignments(assignments, addrs, 3u);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, assignments, count, &audit));

    TEST_ASSERT_EQUAL_HEX64(MASK(0u), audit.confirmed);
    TEST_ASSERT_EQUAL_HEX64(MASK(1u), audit.contested);
    TEST_ASSERT_EQUAL_HEX64(MASK(2u), audit.silent);
    TEST_ASSERT_EQUAL_UINT8(0u, audit.unrecorded_count);
}

/* A run that assigned nothing still audits: this is the shape the failure path
 * produces when it aborts before the first assignment is recorded. */
static void test_an_empty_assignment_list_still_finds_an_unrecorded_write(void)
{
    inventory_begin();
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear(0u);
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, NULL, 0u, &audit));

    TEST_ASSERT_EQUAL_UINT8(0u, audit.assigned_count);
    TEST_ASSERT_EQUAL_HEX64(MASK(0u), audit.unrecorded);
    TEST_ASSERT_FALSE(dali_commissioning_audit_is_clean(&audit));
}

/* Duplicate addresses in the assignment list are one address, not two: the
 * masks cannot say otherwise, and the count must agree with them. */
static void test_a_repeated_assignment_address_is_counted_once(void)
{
    inventory_begin();
    DaliCommissioningOccupancy pre = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    inventory_begin();
    put_gear(4u);
    DaliCommissioningOccupancy post = occupancy_of(DALI_COMMISSIONING_SPACE_GEAR);

    const uint8_t addrs[] = { 4u, 4u };
    DaliCommissioningAssignment assignments[2];
    uint8_t count = build_assignments(assignments, addrs, 2u);

    DaliCommissioningAudit audit;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
        dali_commissioning_audit(&pre, &post, assignments, count, &audit));

    TEST_ASSERT_EQUAL_UINT8(1u, audit.assigned_count);
    TEST_ASSERT_EQUAL_UINT8(1u, audit.confirmed_count);
    TEST_ASSERT_TRUE(dali_commissioning_audit_is_clean(&audit));
}

static void test_audit_rejects_bad_arguments(void)
{
    DaliCommissioningOccupancy occ;
    DaliCommissioningAudit audit;
    DaliCommissioningAssignment assignments[1];

    memset(&occ, 0, sizeof(occ));
    memset(assignments, 0, sizeof(assignments));

    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_commissioning_audit(NULL, &occ, NULL, 0u, &audit));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_commissioning_audit(&occ, NULL, NULL, 0u, &audit));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_commissioning_audit(&occ, &occ, NULL, 0u, NULL));
    /* A count with no array. */
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_commissioning_audit(&occ, &occ, NULL, 1u, &audit));
    /* More assignments than a run can hold. */
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_commissioning_audit(&occ, &occ, assignments,
                                 DALI_COMMISSIONING_MAX_ASSIGNMENTS + 1u,
                                 &audit));
    /* An address outside the short-address space. */
    assignments[0].short_address = DALI_SHORT_ADDRESS_COUNT;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
        dali_commissioning_audit(&occ, &occ, assignments, 1u, &audit));

    TEST_ASSERT_FALSE(dali_commissioning_audit_is_clean(NULL));
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gear_occupancy_reads_only_the_gear_fields);
    RUN_TEST(test_device_occupancy_reads_only_the_device_fields);
    RUN_TEST(test_contested_wins_over_occupied_in_the_same_record);
    RUN_TEST(test_occupancy_rejects_bad_arguments);
    RUN_TEST(test_a_clean_run_confirms_every_assignment);
    RUN_TEST(test_equal_random_address_collision_reads_as_contested);
    RUN_TEST(test_a_pre_existing_contested_address_is_not_reported);
    RUN_TEST(test_a_newly_contested_unassigned_address_is_reported);
    RUN_TEST(test_an_address_written_but_not_recorded_is_reported);
    RUN_TEST(test_pre_existing_gear_is_not_mistaken_for_an_unrecorded_write);
    RUN_TEST(test_a_contested_address_that_resolves_is_not_an_unrecorded_write);
    RUN_TEST(test_an_assignment_that_stops_answering_reads_as_silent);
    RUN_TEST(test_the_assignment_masks_partition_the_claimed_addresses);
    RUN_TEST(test_a_device_walk_is_audited_in_device_space);
    RUN_TEST(test_an_empty_assignment_list_still_finds_an_unrecorded_write);
    RUN_TEST(test_a_repeated_assignment_address_is_counted_once);
    RUN_TEST(test_audit_rejects_bad_arguments);
    return UNITY_END();
}
