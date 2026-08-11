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

static void test_null_map_is_safe(void)
{
    dali_group_map_reset(NULL);
    dali_group_map_seed(NULL, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, dali_group_map_pick(NULL, 0u));
    dali_group_map_rebuild_from_inventory(NULL, NULL);
    TEST_ASSERT_EQUAL(DALI_GROUP_MAP_NO_CHANGE,
                      dali_group_map_apply_config(NULL, short_target(0u),
                                                  DALI_CMD_ADD_TO_GROUP, 0u));
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

    RUN_TEST(test_null_map_is_safe);

    return UNITY_END();
}
