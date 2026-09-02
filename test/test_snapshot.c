#include "unity.h"
#include "dali_snapshot.h"

#include <string.h>

static DaliSnapshot s_snapshot;
static DaliSnapshot s_decoded;
static uint8_t      s_buf[DALI_SNAPSHOT_BLOB_MAX + 16u];

void setUp(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(&s_decoded, 0, sizeof(s_decoded));
    memset(s_buf, 0, sizeof(s_buf));
    dali_snapshot_reset(&s_snapshot);
}

void tearDown(void) {}

static DaliSnapshotEntry make_entry(DaliSnapshotSpace space,
                                    uint8_t           addr,
                                    uint8_t           ident_seed)
{
    DaliSnapshotEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.space         = space;
    entry.short_address = addr;
    entry.has_identification = true;
    for (uint8_t i = 0u; i < DALI_MEMORY_BANK0_IDENTIFICATION_LEN; i++) {
        entry.identification[i] = (uint8_t)(ident_seed + i);
    }
    entry.has_gtin = true;
    for (uint8_t i = 0u; i < DALI_MEMORY_BANK0_GTIN_LEN; i++) {
        entry.gtin[i] = (uint8_t)(0xA0u + i);
    }
    entry.has_groups = true;
    entry.groups     = (uint16_t)(1u << (addr % 16u));
    return entry;
}

/* --------------------------------------------------------------------------
 * Model
 * -------------------------------------------------------------------------*/

void test_reset_produces_an_empty_versioned_snapshot(void)
{
    DaliSnapshot snapshot;
    memset(&snapshot, 0xAA, sizeof(snapshot));
    dali_snapshot_reset(&snapshot);
    TEST_ASSERT_EQUAL_UINT8(0u, snapshot.entry_count);
    TEST_ASSERT_EQUAL_UINT8(DALI_SNAPSHOT_FORMAT_VERSION, snapshot.version);
}

void test_add_rejects_out_of_range_short_address(void)
{
    DaliSnapshotEntry entry = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 0u, 1u);
    entry.short_address = DALI_SHORT_ADDRESS_COUNT;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, dali_snapshot_add(&s_snapshot, &entry));
    TEST_ASSERT_EQUAL_UINT8(0u, s_snapshot.entry_count);
}

void test_add_rejects_an_unknown_space(void)
{
    DaliSnapshotEntry entry = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 0u, 1u);
    entry.space = (DaliSnapshotSpace)7;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, dali_snapshot_add(&s_snapshot, &entry));
}

void test_add_reports_full_at_capacity(void)
{
    for (uint16_t i = 0u; i < DALI_SNAPSHOT_MAX_ENTRIES; i++) {
        DaliSnapshotEntry entry =
            make_entry(i < DALI_SHORT_ADDRESS_COUNT ? DALI_SNAPSHOT_SPACE_GEAR
                                                    : DALI_SNAPSHOT_SPACE_DEVICE,
                       (uint8_t)(i % DALI_SHORT_ADDRESS_COUNT),
                       (uint8_t)i);
        TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));
    }
    TEST_ASSERT_EQUAL_UINT8(DALI_SNAPSHOT_MAX_ENTRIES, s_snapshot.entry_count);

    DaliSnapshotEntry overflow = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 0u, 9u);
    TEST_ASSERT_EQUAL_INT(DALI_ERR_FULL, dali_snapshot_add(&s_snapshot, &overflow));
    TEST_ASSERT_EQUAL_UINT8(DALI_SNAPSHOT_MAX_ENTRIES, s_snapshot.entry_count);
}

void test_find_matches_within_its_own_address_space_only(void)
{
    DaliSnapshotEntry gear   = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 5u, 0x10u);
    DaliSnapshotEntry device = make_entry(DALI_SNAPSHOT_SPACE_DEVICE, 9u, 0x20u);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &gear));
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &device));

    bool duplicate = true;
    const DaliSnapshotEntry *found =
        dali_snapshot_find_by_identification(&s_snapshot,
                                             DALI_SNAPSHOT_SPACE_GEAR,
                                             gear.identification,
                                             &duplicate);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_UINT8(5u, found->short_address);
    TEST_ASSERT_FALSE(duplicate);

    /* The same identification number looked up in the other space must miss:
     * the two spaces are independent and a device must never resolve to gear. */
    TEST_ASSERT_NULL(dali_snapshot_find_by_identification(&s_snapshot,
                                                          DALI_SNAPSHOT_SPACE_DEVICE,
                                                          gear.identification,
                                                          NULL));
}

void test_find_reports_a_duplicate_rather_than_picking_one(void)
{
    DaliSnapshotEntry first  = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 3u, 0x30u);
    DaliSnapshotEntry second = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 8u, 0x30u);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &first));
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &second));

    bool duplicate = false;
    (void)dali_snapshot_find_by_identification(&s_snapshot,
                                               DALI_SNAPSHOT_SPACE_GEAR,
                                               first.identification,
                                               &duplicate);
    TEST_ASSERT_TRUE(duplicate);
}

void test_an_all_zero_identification_never_matches(void)
{
    /* Gear that answers a memory read with zeroes must not make every such unit
     * look like the same physical device. */
    DaliSnapshotEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.space              = DALI_SNAPSHOT_SPACE_GEAR;
    entry.short_address      = 2u;
    entry.has_identification = true;
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));

    TEST_ASSERT_TRUE(dali_snapshot_identification_is_null(entry.identification));
    TEST_ASSERT_NULL(dali_snapshot_find_by_identification(&s_snapshot,
                                                          DALI_SNAPSHOT_SPACE_GEAR,
                                                          entry.identification,
                                                          NULL));
}

void test_used_mask_is_per_space(void)
{
    DaliSnapshotEntry gear   = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 5u, 0x10u);
    DaliSnapshotEntry device = make_entry(DALI_SNAPSHOT_SPACE_DEVICE, 9u, 0x20u);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &gear));
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &device));

    TEST_ASSERT_EQUAL_UINT64(((uint64_t)1u << 5),
                             dali_snapshot_used_mask(&s_snapshot,
                                                     DALI_SNAPSHOT_SPACE_GEAR));
    TEST_ASSERT_EQUAL_UINT64(((uint64_t)1u << 9),
                             dali_snapshot_used_mask(&s_snapshot,
                                                     DALI_SNAPSHOT_SPACE_DEVICE));
}

/* --------------------------------------------------------------------------
 * Building from an inventory
 * -------------------------------------------------------------------------*/

static void seed_gear(DaliDiscoveryInventory *inv, uint8_t addr, uint8_t seed)
{
    DaliDiscoveryDeviceInfo *device = &inv->devices[addr];
    device->present         = true;
    device->has_control_gear = true;
    device->has_identity    = true;
    device->has_groups      = true;
    device->groups          = (uint16_t)(1u << (addr % 16u));
    for (uint8_t i = 0u; i < DALI_MEMORY_BANK0_IDENTIFICATION_LEN; i++) {
        device->identity.serial[i] = (uint8_t)(seed + i);
    }
}

void test_from_inventory_records_gear_with_identity_and_groups(void)
{
    DaliDiscoveryInventory inv;
    memset(&inv, 0, sizeof(inv));
    inv.valid = true;
    seed_gear(&inv, 2u, 0x40u);
    seed_gear(&inv, 7u, 0x50u);

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_from_inventory(&s_snapshot, &inv));
    TEST_ASSERT_EQUAL_UINT8(2u, s_snapshot.entry_count);
    TEST_ASSERT_EQUAL_UINT8(2u, s_snapshot.entries[0].short_address);
    TEST_ASSERT_TRUE(s_snapshot.entries[0].has_identification);
    TEST_ASSERT_TRUE(s_snapshot.entries[0].has_groups);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(1u << 2), s_snapshot.entries[0].groups);
}

void test_from_inventory_records_a_hybrid_as_one_entry_per_space(void)
{
    DaliDiscoveryInventory inv;
    memset(&inv, 0, sizeof(inv));
    inv.valid = true;
    seed_gear(&inv, 0u, 0x60u);
    inv.devices[0].has_input_device = true;   /* something answers in both spaces */

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_from_inventory(&s_snapshot, &inv));
    TEST_ASSERT_EQUAL_UINT8(2u, s_snapshot.entry_count);
    TEST_ASSERT_EQUAL_INT(DALI_SNAPSHOT_SPACE_GEAR, s_snapshot.entries[0].space);
    TEST_ASSERT_EQUAL_INT(DALI_SNAPSHOT_SPACE_DEVICE, s_snapshot.entries[1].space);
    TEST_ASSERT_EQUAL_UINT8(0u, s_snapshot.entries[1].short_address);
}

void test_a_device_entry_takes_its_identity_from_the_device_bank(void)
{
    DaliDiscoveryInventory inv;
    memset(&inv, 0, sizeof(inv));
    inv.valid = true;
    seed_gear(&inv, 0u, 0x60u);
    inv.devices[0].has_input_device    = true;
    inv.devices[0].has_device_identity = true;
    for (uint8_t i = 0u; i < DALI_MEMORY_BANK0_IDENTIFICATION_LEN; i++) {
        inv.devices[0].device_identity.serial[i] = (uint8_t)(0xB0u + i);
    }

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_from_inventory(&s_snapshot, &inv));
    TEST_ASSERT_EQUAL_UINT8(2u, s_snapshot.entry_count);

    /* The two entries must carry different anchors: on real hardware the gear
     * and device halves of one numeric address reported different identities,
     * and borrowing one for the other invents a pairing the bus denies. */
    TEST_ASSERT_TRUE(s_snapshot.entries[1].has_identification);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(inv.devices[0].device_identity.serial,
                                  s_snapshot.entries[1].identification,
                                  DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
    TEST_ASSERT_FALSE(dali_snapshot_identification_equal(
        s_snapshot.entries[0].identification,
        s_snapshot.entries[1].identification));
}

void test_a_device_without_its_own_bank0_is_recorded_unanchored(void)
{
    DaliDiscoveryInventory inv;
    memset(&inv, 0, sizeof(inv));
    inv.valid = true;
    seed_gear(&inv, 0u, 0x60u);
    inv.devices[0].has_input_device = true;   /* device Bank 0 unreadable */

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_from_inventory(&s_snapshot, &inv));
    TEST_ASSERT_TRUE(s_snapshot.entries[0].has_identification);
    TEST_ASSERT_FALSE(s_snapshot.entries[1].has_identification);
}

void test_from_inventory_keeps_gear_whose_identity_could_not_be_read(void)
{
    /* Dropping it would make a partial backup look complete. */
    DaliDiscoveryInventory inv;
    memset(&inv, 0, sizeof(inv));
    inv.valid = true;
    inv.devices[4].present          = true;
    inv.devices[4].has_control_gear = true;
    inv.devices[4].has_identity     = false;

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_from_inventory(&s_snapshot, &inv));
    TEST_ASSERT_EQUAL_UINT8(1u, s_snapshot.entry_count);
    TEST_ASSERT_FALSE(s_snapshot.entries[0].has_identification);
    TEST_ASSERT_EQUAL_UINT8(4u, s_snapshot.entries[0].short_address);
}

void test_from_inventory_rejects_an_invalid_inventory(void)
{
    DaliDiscoveryInventory inv;
    memset(&inv, 0, sizeof(inv));
    inv.valid = false;
    seed_gear(&inv, 1u, 0x70u);
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_from_inventory(&s_snapshot, &inv));
}

/* --------------------------------------------------------------------------
 * Codec
 * -------------------------------------------------------------------------*/

void test_encode_decode_round_trip_preserves_every_field(void)
{
    DaliSnapshotEntry gear   = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 13u, 0x11u);
    DaliSnapshotEntry device = make_entry(DALI_SNAPSHOT_SPACE_DEVICE, 41u, 0x22u);
    device.has_gtin   = false;
    device.has_groups = false;
    device.groups     = 0u;
    memset(device.gtin, 0, sizeof(device.gtin));
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &gear));
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &device));

    uint32_t written = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_snapshot_encode(&s_snapshot, s_buf, sizeof(s_buf), &written));
    TEST_ASSERT_EQUAL_UINT32(DALI_SNAPSHOT_HEADER_SIZE +
                                 (2u * DALI_SNAPSHOT_ENTRY_WIRE_SIZE),
                             written);

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_decode(&s_decoded, s_buf, written));
    TEST_ASSERT_EQUAL_UINT8(2u, s_decoded.entry_count);

    TEST_ASSERT_EQUAL_INT(DALI_SNAPSHOT_SPACE_GEAR, s_decoded.entries[0].space);
    TEST_ASSERT_EQUAL_UINT8(13u, s_decoded.entries[0].short_address);
    TEST_ASSERT_TRUE(s_decoded.entries[0].has_identification);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(gear.identification,
                                  s_decoded.entries[0].identification,
                                  DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
    TEST_ASSERT_TRUE(s_decoded.entries[0].has_gtin);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(gear.gtin,
                                  s_decoded.entries[0].gtin,
                                  DALI_MEMORY_BANK0_GTIN_LEN);
    TEST_ASSERT_TRUE(s_decoded.entries[0].has_groups);
    TEST_ASSERT_EQUAL_UINT16(gear.groups, s_decoded.entries[0].groups);

    TEST_ASSERT_EQUAL_INT(DALI_SNAPSHOT_SPACE_DEVICE, s_decoded.entries[1].space);
    TEST_ASSERT_EQUAL_UINT8(41u, s_decoded.entries[1].short_address);
    TEST_ASSERT_FALSE(s_decoded.entries[1].has_gtin);
    TEST_ASSERT_FALSE(s_decoded.entries[1].has_groups);
}

void test_encode_of_an_empty_snapshot_is_a_bare_header(void)
{
    uint32_t written = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_snapshot_encode(&s_snapshot, s_buf, sizeof(s_buf), &written));
    TEST_ASSERT_EQUAL_UINT32(DALI_SNAPSHOT_HEADER_SIZE, written);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_decode(&s_decoded, s_buf, written));
    TEST_ASSERT_EQUAL_UINT8(0u, s_decoded.entry_count);
}

void test_a_full_snapshot_fits_the_declared_blob_maximum(void)
{
    for (uint16_t i = 0u; i < DALI_SNAPSHOT_MAX_ENTRIES; i++) {
        DaliSnapshotEntry entry =
            make_entry(i < DALI_SHORT_ADDRESS_COUNT ? DALI_SNAPSHOT_SPACE_GEAR
                                                    : DALI_SNAPSHOT_SPACE_DEVICE,
                       (uint8_t)(i % DALI_SHORT_ADDRESS_COUNT),
                       (uint8_t)i);
        TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));
    }

    uint32_t written = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_snapshot_encode(&s_snapshot, s_buf, sizeof(s_buf), &written));
    TEST_ASSERT_EQUAL_UINT32(DALI_SNAPSHOT_BLOB_MAX, written);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_decode(&s_decoded, s_buf, written));
    TEST_ASSERT_EQUAL_UINT8(DALI_SNAPSHOT_MAX_ENTRIES, s_decoded.entry_count);
}

void test_encode_reports_full_when_the_buffer_is_short(void)
{
    DaliSnapshotEntry entry = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 1u, 0x80u);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));

    uint32_t written = 0xFFFFu;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_FULL,
                          dali_snapshot_encode(&s_snapshot,
                                               s_buf,
                                               DALI_SNAPSHOT_HEADER_SIZE,
                                               &written));
    TEST_ASSERT_EQUAL_UINT32(0u, written);
}

void test_decode_rejects_a_foreign_magic(void)
{
    uint32_t written = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_snapshot_encode(&s_snapshot, s_buf, sizeof(s_buf), &written));
    s_buf[0] = 'X';
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_decode(&s_decoded, s_buf, written));
}

void test_decode_rejects_an_unknown_version(void)
{
    uint32_t written = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_snapshot_encode(&s_snapshot, s_buf, sizeof(s_buf), &written));
    s_buf[4] = DALI_SNAPSHOT_FORMAT_VERSION + 1u;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_decode(&s_decoded, s_buf, written));
}

void test_decode_rejects_a_length_that_disagrees_with_the_entry_count(void)
{
    DaliSnapshotEntry entry = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 1u, 0x90u);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));

    uint32_t written = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_snapshot_encode(&s_snapshot, s_buf, sizeof(s_buf), &written));

    /* Truncated, and one byte long. Both are "not a snapshot", not "a snapshot
     * with slack": guessing which half to trust is how a restore moves a fixture
     * to the wrong address. */
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_decode(&s_decoded, s_buf, written - 1u));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_decode(&s_decoded, s_buf, written + 1u));
}

void test_decode_rejects_an_entry_count_over_capacity(void)
{
    uint32_t written = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_snapshot_encode(&s_snapshot, s_buf, sizeof(s_buf), &written));
    s_buf[5] = (uint8_t)(DALI_SNAPSHOT_MAX_ENTRIES + 1u);
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_decode(&s_decoded, s_buf, written));
}

void test_decode_rejects_a_corrupt_space_or_address(void)
{
    DaliSnapshotEntry entry = make_entry(DALI_SNAPSHOT_SPACE_GEAR, 1u, 0xA0u);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &entry));

    uint32_t written = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_snapshot_encode(&s_snapshot, s_buf, sizeof(s_buf), &written));

    const uint32_t rec = DALI_SNAPSHOT_HEADER_SIZE;
    const uint8_t  saved_space = s_buf[rec + 1u];
    s_buf[rec + 1u] = 9u;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_decode(&s_decoded, s_buf, written));
    s_buf[rec + 1u] = saved_space;

    s_buf[rec + 2u] = DALI_SHORT_ADDRESS_COUNT;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_decode(&s_decoded, s_buf, written));
}

void test_invalid_arguments_are_rejected(void)
{
    uint32_t written = 0u;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_encode(NULL, s_buf, sizeof(s_buf), &written));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_encode(&s_snapshot, NULL, sizeof(s_buf), &written));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_encode(&s_snapshot, s_buf, sizeof(s_buf), NULL));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_decode(NULL, s_buf, sizeof(s_buf)));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_decode(&s_decoded, NULL, sizeof(s_buf)));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_decode(&s_decoded, s_buf, 3u));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, dali_snapshot_add(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_snapshot_from_inventory(NULL, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reset_produces_an_empty_versioned_snapshot);
    RUN_TEST(test_add_rejects_out_of_range_short_address);
    RUN_TEST(test_add_rejects_an_unknown_space);
    RUN_TEST(test_add_reports_full_at_capacity);
    RUN_TEST(test_find_matches_within_its_own_address_space_only);
    RUN_TEST(test_find_reports_a_duplicate_rather_than_picking_one);
    RUN_TEST(test_an_all_zero_identification_never_matches);
    RUN_TEST(test_used_mask_is_per_space);
    RUN_TEST(test_from_inventory_records_gear_with_identity_and_groups);
    RUN_TEST(test_from_inventory_records_a_hybrid_as_one_entry_per_space);
    RUN_TEST(test_a_device_entry_takes_its_identity_from_the_device_bank);
    RUN_TEST(test_a_device_without_its_own_bank0_is_recorded_unanchored);
    RUN_TEST(test_from_inventory_keeps_gear_whose_identity_could_not_be_read);
    RUN_TEST(test_from_inventory_rejects_an_invalid_inventory);
    RUN_TEST(test_encode_decode_round_trip_preserves_every_field);
    RUN_TEST(test_encode_of_an_empty_snapshot_is_a_bare_header);
    RUN_TEST(test_a_full_snapshot_fits_the_declared_blob_maximum);
    RUN_TEST(test_encode_reports_full_when_the_buffer_is_short);
    RUN_TEST(test_decode_rejects_a_foreign_magic);
    RUN_TEST(test_decode_rejects_an_unknown_version);
    RUN_TEST(test_decode_rejects_a_length_that_disagrees_with_the_entry_count);
    RUN_TEST(test_decode_rejects_an_entry_count_over_capacity);
    RUN_TEST(test_decode_rejects_a_corrupt_space_or_address);
    RUN_TEST(test_invalid_arguments_are_rejected);
    return UNITY_END();
}
