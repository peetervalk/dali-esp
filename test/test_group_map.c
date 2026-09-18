#include "unity.h"
#include "dali_group_map.h"
#include "dali_discovery.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static DaliTarget short_target(uint8_t addr)
{
    DaliTarget t = { .type = DALI_ADDR_SHORT, .address = addr };
    return t;
}

static DaliTarget group_target(uint8_t group)
{
    DaliTarget t = { .type = DALI_ADDR_GROUP, .address = group };
    return t;
}

static DaliTarget broadcast_target(void)
{
    DaliTarget t = { .type = DALI_ADDR_BROADCAST, .address = 0u };
    return t;
}

/* ── reset / seed / pick ─────────────────────────────────────────────────── */

static void test_reset_clears_everything(void)
{
    DaliGroupMap map;
    memset(&map, 0xFF, sizeof(map));  /* dirty the struct first */
    dali_group_map_reset(&map);
    for (uint8_t g = 0u; g < DALI_GROUP_COUNT; g++)
        TEST_ASSERT_EQUAL_UINT64(0u, map.members[g]);
    TEST_ASSERT_EQUAL_UINT16(0u, map.verified);
}

static void test_pick_empty_group_returns_ff(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, dali_group_map_pick(&map, 3u));
}

static void test_seed_then_pick_returns_seed(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 5u, 12u);
    TEST_ASSERT_EQUAL_UINT8(12u, dali_group_map_pick(&map, 5u));
}

static void test_pick_returns_lowest_member(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 2u, 20u);
    dali_group_map_seed(&map, 2u, 7u);
    dali_group_map_seed(&map, 2u, 40u);
    TEST_ASSERT_EQUAL_UINT8(7u, dali_group_map_pick(&map, 2u));
}

static void test_seed_out_of_range_is_noop(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 16u, 1u);   /* group too high */
    dali_group_map_seed(&map, 0u, 64u);   /* addr too high  */
    for (uint8_t g = 0u; g < DALI_GROUP_COUNT; g++)
        TEST_ASSERT_EQUAL_UINT64(0u, map.members[g]);
}

static void test_pick_invalid_group_returns_ff(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, dali_group_map_pick(&map, 16u));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, dali_group_map_pick(&map, 200u));
}

static void test_seed_does_not_verify(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 4u, 9u);
    TEST_ASSERT_EQUAL_UINT16(0u, map.verified);
}

static void test_replacement_scan_must_cover_every_known_member(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 2u, 3u);
    dali_group_map_seed(&map, 7u, 12u);

    uint64_t both = ((uint64_t)1u << 3u) | ((uint64_t)1u << 12u);
    TEST_ASSERT_TRUE(dali_group_map_scan_covers_known_members(&map, both));
    TEST_ASSERT_TRUE(dali_group_map_scan_covers_known_members(
        &map, both | ((uint64_t)1u << 40u)));
    TEST_ASSERT_FALSE(dali_group_map_scan_covers_known_members(
        &map, (uint64_t)1u << 3u));
    TEST_ASSERT_FALSE(dali_group_map_scan_covers_known_members(NULL, both));
}

static void test_empty_map_is_covered_by_empty_scan(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    TEST_ASSERT_TRUE(dali_group_map_scan_covers_known_members(&map, 0u));
}

/* ── rebuild from inventory ──────────────────────────────────────────────── */

static void build_gear(DaliDiscoveryInventory *inv, uint8_t addr, uint16_t groups)
{
    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_inventory_store_status(inv, addr, 0u));
    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_inventory_store_groups(inv, addr, groups));
}

static void test_rebuild_maps_gear_to_groups(void)
{
    DaliDiscoveryInventory inv;
    dali_discovery_inventory_reset(&inv);
    build_gear(&inv, 3u, (1u << 0) | (1u << 5));  /* addr 3 in groups 0 and 5 */
    build_gear(&inv, 8u, (1u << 5));              /* addr 8 in group 5        */

    DaliGroupMap map;
    dali_group_map_rebuild_from_inventory(&map, &inv);

    TEST_ASSERT_EQUAL_UINT64((uint64_t)1u << 3, map.members[0]);
    TEST_ASSERT_EQUAL_UINT64(((uint64_t)1u << 3) | ((uint64_t)1u << 8), map.members[5]);
    TEST_ASSERT_EQUAL_UINT8(3u, dali_group_map_pick(&map, 5u));
    /* an untouched group stays empty */
    TEST_ASSERT_EQUAL_UINT64(0u, map.members[1]);
}

static void test_rebuild_marks_all_groups_verified(void)
{
    DaliDiscoveryInventory inv;
    dali_discovery_inventory_reset(&inv);
    build_gear(&inv, 1u, (1u << 2));

    DaliGroupMap map;
    dali_group_map_rebuild_from_inventory(&map, &inv);
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu, map.verified);
}

static void test_rebuild_marks_partial_group_observation_unverified(void)
{
    DaliDiscoveryInventory inv;
    dali_discovery_inventory_reset(&inv);
    build_gear(&inv, 1u, (1u << 2));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_inventory_store_status(&inv, 7u, 0u));
    /* Address 7 is known gear, but its optional group query did not complete. */

    DaliGroupMap map;
    TEST_ASSERT_FALSE(dali_group_map_rebuild_from_inventory(&map, &inv));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)1u << 1, map.members[2]);
    TEST_ASSERT_EQUAL_UINT16(0u, map.verified);
}

static void test_rebuild_skips_pure_input_device(void)
{
    DaliDiscoveryInventory inv;
    dali_discovery_inventory_reset(&inv);
    /* Craft a present, grouped device that is an input device but NOT gear. */
    DaliDiscoveryDeviceInfo *d = &inv.devices[10];
    d->present          = true;
    d->has_groups       = true;
    d->groups           = (1u << 4);
    d->has_input_device = true;
    d->has_control_gear = false;

    DaliGroupMap map;
    TEST_ASSERT_TRUE(dali_group_map_rebuild_from_inventory(&map, &inv));
    TEST_ASSERT_EQUAL_UINT64(0u, map.members[4]);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, dali_group_map_pick(&map, 4u));
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu, map.verified);
}

static void test_rebuild_does_not_authorize_empty_inventory(void)
{
    DaliDiscoveryInventory inv;
    dali_discovery_inventory_reset(&inv);

    DaliGroupMap map;
    TEST_ASSERT_FALSE(dali_group_map_rebuild_from_inventory(&map, &inv));
    TEST_ASSERT_EQUAL_UINT16(0u, map.verified);
}

static void test_rebuild_replaces_prior_state(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 9u, 60u);  /* stale seed in a group the scan won't touch */

    DaliDiscoveryInventory inv;
    dali_discovery_inventory_reset(&inv);
    build_gear(&inv, 2u, (1u << 1));

    dali_group_map_rebuild_from_inventory(&map, &inv);
    TEST_ASSERT_EQUAL_UINT64(0u, map.members[9]);        /* stale seed gone */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)1u << 2, map.members[1]);
}

/* ── apply_config: short target ──────────────────────────────────────────── */

static void test_apply_short_add_sets_bit(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, short_target(6u), DALI_CMD_ADD_TO_GROUP, 3u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_UPDATED, r);
    TEST_ASSERT_EQUAL_UINT8(6u, dali_group_map_pick(&map, 3u));
}

static void test_apply_short_remove_clears_bit(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 3u, 6u);
    dali_group_map_seed(&map, 3u, 9u);
    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, short_target(6u), DALI_CMD_REMOVE_FROM_GROUP, 3u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_UPDATED, r);
    TEST_ASSERT_EQUAL_UINT8(9u, dali_group_map_pick(&map, 3u));  /* 6 gone, 9 remains */
}

static void test_apply_short_remove_last_member_empties_group(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 7u, 2u);  /* representative == the one being removed */
    dali_group_map_apply_config(&map, short_target(2u), DALI_CMD_REMOVE_FROM_GROUP, 7u);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, dali_group_map_pick(&map, 7u));
}

static void test_apply_short_add_already_member_no_change(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 1u, 4u);
    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, short_target(4u), DALI_CMD_ADD_TO_GROUP, 1u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_NO_CHANGE, r);
}

static void test_apply_short_remove_absent_no_change(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, short_target(4u), DALI_CMD_REMOVE_FROM_GROUP, 1u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_NO_CHANGE, r);
}

/* ── apply_config: guards ────────────────────────────────────────────────── */

static void test_apply_non_group_command_ignored(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, short_target(4u), DALI_CMD_RESET, 1u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_NO_CHANGE, r);
    TEST_ASSERT_EQUAL_UINT64(0u, map.members[1]);
}

static void test_apply_bad_group_ignored(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, short_target(4u), DALI_CMD_ADD_TO_GROUP, 16u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_NO_CHANGE, r);
}

static void test_apply_broadcast_target_ignored(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, broadcast_target(), DALI_CMD_ADD_TO_GROUP, 1u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_NO_CHANGE, r);
    TEST_ASSERT_EQUAL_UINT64(0u, map.members[1]);
}

/* ── apply_config: group target ──────────────────────────────────────────── */

static void test_apply_group_add_verified_source_unions_members(void)
{
    DaliDiscoveryInventory inv;
    dali_discovery_inventory_reset(&inv);
    build_gear(&inv, 4u, (1u << 3));   /* group 3 = {4, 11} */
    build_gear(&inv, 11u, (1u << 3));
    DaliGroupMap map;
    dali_group_map_rebuild_from_inventory(&map, &inv);  /* all groups verified */

    /* "every member of group 3, also join group 6" */
    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, group_target(3u), DALI_CMD_ADD_TO_GROUP, 6u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_UPDATED, r);
    TEST_ASSERT_EQUAL_UINT64(((uint64_t)1u << 4) | ((uint64_t)1u << 11), map.members[6]);
}

static void test_apply_group_remove_verified_source_subtracts_members(void)
{
    DaliDiscoveryInventory inv;
    dali_discovery_inventory_reset(&inv);
    build_gear(&inv, 4u, (1u << 3) | (1u << 6));   /* 4 in groups 3 and 6 */
    build_gear(&inv, 11u, (1u << 3) | (1u << 6));  /* 11 in groups 3 and 6 */
    build_gear(&inv, 20u, (1u << 6));              /* 20 in group 6 only */
    DaliGroupMap map;
    dali_group_map_rebuild_from_inventory(&map, &inv);

    /* "every member of group 3, leave group 6" → only 20 remains in group 6 */
    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, group_target(3u), DALI_CMD_REMOVE_FROM_GROUP, 6u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_UPDATED, r);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)1u << 20, map.members[6]);
}

static void test_apply_group_add_unverified_source_keeps_dest(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 6u, 30u);  /* dest group 6 seeded (unverified) */
    /* source group 3 is unverified (never scanned) */

    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, group_target(3u), DALI_CMD_ADD_TO_GROUP, 6u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_UNVERIFIED_ADD, r);
    /* dest kept its existing poll target */
    TEST_ASSERT_EQUAL_UINT8(30u, dali_group_map_pick(&map, 6u));
    /* dest marked unverified (was already, stays so) */
    TEST_ASSERT_EQUAL_UINT16(0u, (map.verified >> 6) & 1u);
}

static void test_apply_group_remove_unverified_source_clears_dest(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 6u, 30u);  /* dest group 6 seeded */

    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, group_target(3u), DALI_CMD_REMOVE_FROM_GROUP, 6u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_UNVERIFIED_REMOVE, r);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, dali_group_map_pick(&map, 6u));  /* cleared */
}

static void test_apply_group_unverified_clears_dest_verified_bit(void)
{
    DaliDiscoveryInventory inv;
    dali_discovery_inventory_reset(&inv);
    build_gear(&inv, 5u, (1u << 6));  /* verifies group 6 (and all others) */
    DaliGroupMap map;
    dali_group_map_rebuild_from_inventory(&map, &inv);
    /* Now force the SOURCE group 3 to look unverified while dest 6 stays verified. */
    map.verified &= (uint16_t)~(1u << 3);

    DaliGroupMapResult r =
        dali_group_map_apply_config(&map, group_target(3u), DALI_CMD_ADD_TO_GROUP, 6u);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_UNVERIFIED_ADD, r);
    TEST_ASSERT_EQUAL_UINT16(0u, (map.verified >> 6) & 1u);  /* dest now unverified */
}

/* ── null-safety ─────────────────────────────────────────────────────────── */

static void test_forget_single_group_clears_only_that_group(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 3u, 7u);
    dali_group_map_seed(&map, 5u, 7u);
    dali_group_map_seed(&map, 3u, 9u);

    TEST_ASSERT_TRUE(dali_group_map_forget(&map, 7u, 3u));
    /* 9 still holds group 3; 7 still holds group 5. */
    TEST_ASSERT_EQUAL_UINT8(9u, dali_group_map_pick(&map, 3u));
    TEST_ASSERT_EQUAL_UINT8(7u, dali_group_map_pick(&map, 5u));
}

static void test_forget_all_groups_removes_every_membership(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 0u, 4u);
    dali_group_map_seed(&map, 7u, 4u);
    dali_group_map_seed(&map, 7u, 5u);

    TEST_ASSERT_TRUE(dali_group_map_forget(&map, 4u, DALI_GROUP_MAP_ALL_GROUPS));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, dali_group_map_pick(&map, 0u));
    TEST_ASSERT_EQUAL_UINT8(5u, dali_group_map_pick(&map, 7u));
}

static void test_forget_preserves_verified_bits(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 2u, 1u);
    dali_group_map_seed(&map, 2u, 6u);
    map.verified = 0xFFFFu;

    TEST_ASSERT_TRUE(dali_group_map_forget(&map, 1u, 2u));
    /* Retiring a departed member says nothing about the rest of the group, so
     * the scan-verified status of every group is left alone. */
    TEST_ASSERT_EQUAL_HEX16(0xFFFFu, map.verified);
    TEST_ASSERT_EQUAL_UINT8(6u, dali_group_map_pick(&map, 2u));
}

/*
 * A re-addressed member keeps every group it was in. This is the whole point of
 * having a move at all: `forget` plus a rescan is the alternative, and the
 * rescan is what the `address` verb exists to avoid.
 */
static void test_move_follows_member_into_every_group(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 1u, 5u);
    dali_group_map_seed(&map, 3u, 5u);
    dali_group_map_seed(&map, 3u, 9u);

    TEST_ASSERT_TRUE(dali_group_map_move(&map, 5u, 13u));

    /* a5 is gone from both its groups, a13 stands in its place. */
    TEST_ASSERT_EQUAL_UINT8(13u, dali_group_map_pick(&map, 1u));
    /* Group 3 still has a9, which is lower, so pick() proves membership
     * rather than ordering: check the bit directly. */
    TEST_ASSERT_EQUAL_HEX64((uint64_t) 1u << 9u | (uint64_t) 1u << 13u,
                            map.members[3]);
    TEST_ASSERT_EQUAL_HEX64((uint64_t) 1u << 13u, map.members[1]);
}

static void test_move_leaves_other_groups_and_verified_alone(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 2u, 4u);
    dali_group_map_seed(&map, 6u, 8u);
    map.verified = 0xFFFFu;

    TEST_ASSERT_TRUE(dali_group_map_move(&map, 4u, 20u));

    /* A member that changed address is still a member: the group's membership
     * is no less known than it was, so verified is untouched. */
    TEST_ASSERT_EQUAL_HEX16(0xFFFFu, map.verified);
    TEST_ASSERT_EQUAL_UINT8(20u, dali_group_map_pick(&map, 2u));
    /* The gear that did not move is where it was. */
    TEST_ASSERT_EQUAL_UINT8(8u, dali_group_map_pick(&map, 6u));
}

/*
 * Moving onto an address that is already a member must not double-count or
 * lose the group. The `address` verb refuses an occupied destination, so this
 * should be unreachable from the shell -- which is exactly why the map should
 * not depend on that being true.
 */
static void test_move_onto_an_existing_member_is_idempotent(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 1u, 5u);
    dali_group_map_seed(&map, 1u, 7u);

    TEST_ASSERT_TRUE(dali_group_map_move(&map, 5u, 7u));
    TEST_ASSERT_EQUAL_HEX64((uint64_t) 1u << 7u, map.members[1]);
}

static void test_move_reports_no_change_and_rejects_bad_args(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 1u, 2u);

    /* The mover belonged to nothing. */
    TEST_ASSERT_FALSE(dali_group_map_move(&map, 3u, 4u));
    /* Same address both ends: a no-op, not a self-move. */
    TEST_ASSERT_FALSE(dali_group_map_move(&map, 2u, 2u));
    /* Out of range at either end, and a NULL map. */
    TEST_ASSERT_FALSE(dali_group_map_move(&map, 2u, 64u));
    TEST_ASSERT_FALSE(dali_group_map_move(&map, 64u, 2u));
    TEST_ASSERT_FALSE(dali_group_map_move(NULL, 2u, 3u));

    /* None of the refusals touched the map. */
    TEST_ASSERT_EQUAL_HEX64((uint64_t) 1u << 2u, map.members[1]);
}

static void test_forget_reports_no_change_and_rejects_bad_args(void)
{
    DaliGroupMap map;
    dali_group_map_reset(&map);
    dali_group_map_seed(&map, 1u, 2u);

    /* Not a member of that group. */
    TEST_ASSERT_FALSE(dali_group_map_forget(&map, 3u, 1u));
    /* Member, but of a different group. */
    TEST_ASSERT_FALSE(dali_group_map_forget(&map, 2u, 4u));
    /* Out-of-range address and group. */
    TEST_ASSERT_FALSE(dali_group_map_forget(&map, 64u, 1u));
    TEST_ASSERT_FALSE(dali_group_map_forget(&map, 2u, 16u));
    /* Nothing above may have disturbed the real membership. */
    TEST_ASSERT_EQUAL_UINT8(2u, dali_group_map_pick(&map, 1u));
}

static void test_null_map_is_safe(void)
{
    dali_group_map_reset(NULL);
    dali_group_map_seed(NULL, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, dali_group_map_pick(NULL, 0u));
    dali_group_map_rebuild_from_inventory(NULL, NULL);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_NO_CHANGE,
                      dali_group_map_apply_config(NULL, short_target(0u),
                                                  DALI_CMD_ADD_TO_GROUP, 0u));
    TEST_ASSERT_FALSE(dali_group_map_forget(NULL, 0u, 0u));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_reset_clears_everything);
    RUN_TEST(test_pick_empty_group_returns_ff);
    RUN_TEST(test_seed_then_pick_returns_seed);
    RUN_TEST(test_pick_returns_lowest_member);
    RUN_TEST(test_seed_out_of_range_is_noop);
    RUN_TEST(test_pick_invalid_group_returns_ff);
    RUN_TEST(test_seed_does_not_verify);
    RUN_TEST(test_replacement_scan_must_cover_every_known_member);
    RUN_TEST(test_empty_map_is_covered_by_empty_scan);

    RUN_TEST(test_rebuild_maps_gear_to_groups);
    RUN_TEST(test_rebuild_marks_all_groups_verified);
    RUN_TEST(test_rebuild_marks_partial_group_observation_unverified);
    RUN_TEST(test_rebuild_skips_pure_input_device);
    RUN_TEST(test_rebuild_does_not_authorize_empty_inventory);
    RUN_TEST(test_rebuild_replaces_prior_state);

    RUN_TEST(test_apply_short_add_sets_bit);
    RUN_TEST(test_apply_short_remove_clears_bit);
    RUN_TEST(test_apply_short_remove_last_member_empties_group);
    RUN_TEST(test_apply_short_add_already_member_no_change);
    RUN_TEST(test_apply_short_remove_absent_no_change);

    RUN_TEST(test_apply_non_group_command_ignored);
    RUN_TEST(test_apply_bad_group_ignored);
    RUN_TEST(test_apply_broadcast_target_ignored);

    RUN_TEST(test_apply_group_add_verified_source_unions_members);
    RUN_TEST(test_apply_group_remove_verified_source_subtracts_members);
    RUN_TEST(test_apply_group_add_unverified_source_keeps_dest);
    RUN_TEST(test_apply_group_remove_unverified_source_clears_dest);
    RUN_TEST(test_apply_group_unverified_clears_dest_verified_bit);

    RUN_TEST(test_forget_single_group_clears_only_that_group);
    RUN_TEST(test_forget_all_groups_removes_every_membership);
    RUN_TEST(test_forget_preserves_verified_bits);
    RUN_TEST(test_forget_reports_no_change_and_rejects_bad_args);

    RUN_TEST(test_move_follows_member_into_every_group);
    RUN_TEST(test_move_leaves_other_groups_and_verified_alone);
    RUN_TEST(test_move_onto_an_existing_member_is_idempotent);
    RUN_TEST(test_move_reports_no_change_and_rejects_bad_args);

    RUN_TEST(test_null_map_is_safe);

    return UNITY_END();
}
