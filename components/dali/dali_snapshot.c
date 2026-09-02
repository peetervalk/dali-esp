#include "dali_snapshot.h"

#include <string.h>

static const uint8_t k_snapshot_magic[DALI_SNAPSHOT_MAGIC_LEN] = { 'D', 'B', 'K', '1' };

#define SNAPSHOT_FLAG_HAS_IDENTIFICATION 0x01u
#define SNAPSHOT_FLAG_HAS_GTIN           0x02u
#define SNAPSHOT_FLAG_HAS_GROUPS         0x04u

void dali_snapshot_reset(DaliSnapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = DALI_SNAPSHOT_FORMAT_VERSION;
}

static bool snapshot_space_valid(DaliSnapshotSpace space)
{
    return space == DALI_SNAPSHOT_SPACE_GEAR || space == DALI_SNAPSHOT_SPACE_DEVICE;
}

DaliError dali_snapshot_add(DaliSnapshot *snapshot, const DaliSnapshotEntry *entry)
{
    if (snapshot == NULL || entry == NULL ||
        entry->short_address >= DALI_SHORT_ADDRESS_COUNT ||
        !snapshot_space_valid(entry->space)) {
        return DALI_ERR_INVALID;
    }

    if (snapshot->entry_count >= DALI_SNAPSHOT_MAX_ENTRIES) {
        return DALI_ERR_FULL;
    }

    snapshot->entries[snapshot->entry_count] = *entry;
    snapshot->entry_count++;
    snapshot->version = DALI_SNAPSHOT_FORMAT_VERSION;
    return DALI_OK;
}

bool dali_snapshot_identification_equal(const uint8_t *a, const uint8_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return memcmp(a, b, DALI_MEMORY_BANK0_IDENTIFICATION_LEN) == 0;
}

bool dali_snapshot_identification_is_null(const uint8_t *identification)
{
    if (identification == NULL) {
        return true;
    }
    for (uint8_t i = 0u; i < DALI_MEMORY_BANK0_IDENTIFICATION_LEN; i++) {
        if (identification[i] != 0u) {
            return false;
        }
    }
    return true;
}

const DaliSnapshotEntry *dali_snapshot_find_by_identification(
    const DaliSnapshot *snapshot,
    DaliSnapshotSpace   space,
    const uint8_t      *identification,
    bool               *duplicate_out)
{
    if (duplicate_out != NULL) {
        *duplicate_out = false;
    }

    if (snapshot == NULL || identification == NULL ||
        !snapshot_space_valid(space) ||
        dali_snapshot_identification_is_null(identification)) {
        return NULL;
    }

    const DaliSnapshotEntry *found = NULL;
    for (uint8_t i = 0u; i < snapshot->entry_count; i++) {
        const DaliSnapshotEntry *entry = &snapshot->entries[i];
        if (entry->space != space || !entry->has_identification) {
            continue;
        }
        if (!dali_snapshot_identification_equal(entry->identification, identification)) {
            continue;
        }
        if (found != NULL) {
            /*
             * Two recorded units claiming one identification number. Nothing on
             * the bus can separate them, so the caller is told rather than
             * handed an arbitrary one of the two.
             */
            if (duplicate_out != NULL) {
                *duplicate_out = true;
            }
            return found;
        }
        found = entry;
    }

    return found;
}

uint64_t dali_snapshot_used_mask(const DaliSnapshot *snapshot,
                                 DaliSnapshotSpace   space)
{
    if (snapshot == NULL || !snapshot_space_valid(space)) {
        return 0u;
    }

    uint64_t mask = 0u;
    for (uint8_t i = 0u; i < snapshot->entry_count; i++) {
        const DaliSnapshotEntry *entry = &snapshot->entries[i];
        if (entry->space == space && entry->short_address < DALI_SHORT_ADDRESS_COUNT) {
            mask |= ((uint64_t)1u << entry->short_address);
        }
    }
    return mask;
}

DaliError dali_snapshot_from_inventory(DaliSnapshot                 *out,
                                       const DaliDiscoveryInventory *inventory)
{
    if (out == NULL || inventory == NULL || !inventory->valid) {
        return DALI_ERR_INVALID;
    }

    dali_snapshot_reset(out);

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device =
            dali_discovery_inventory_get(inventory, addr);
        if (device == NULL || !device->present) {
            continue;
        }

        /*
         * A hybrid unit occupies one address in each space and produces two
         * entries. They are recorded independently because the two addresses
         * move independently; pairing them is dali_restore's job, not the
         * snapshot's.
         */
        if (device->has_control_gear) {
            DaliSnapshotEntry entry;
            memset(&entry, 0, sizeof(entry));
            entry.space         = DALI_SNAPSHOT_SPACE_GEAR;
            entry.short_address = addr;

            if (device->has_identity) {
                entry.has_identification = true;
                memcpy(entry.identification,
                       device->identity.serial,
                       DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
                entry.has_gtin = true;
                memcpy(entry.gtin,
                       device->identity.gtin,
                       DALI_MEMORY_BANK0_GTIN_LEN);
            }

            if (device->has_groups) {
                entry.has_groups = true;
                entry.groups     = device->groups;
            }

            DaliError err = dali_snapshot_add(out, &entry);
            if (err != DALI_OK) {
                return err;
            }
        }

        if (device->has_input_device) {
            DaliSnapshotEntry entry;
            memset(&entry, 0, sizeof(entry));
            entry.space         = DALI_SNAPSHOT_SPACE_DEVICE;
            entry.short_address = addr;

            /*
             * The device's own Bank 0, never the gear entry's. A unit answering
             * in both spaces is not thereby one physical device, and borrowing
             * the gear identity here would fabricate exactly the pairing the
             * two-space design exists to avoid asserting.
             */
            if (device->has_device_identity) {
                entry.has_identification = true;
                memcpy(entry.identification,
                       device->device_identity.serial,
                       DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
                entry.has_gtin = true;
                memcpy(entry.gtin,
                       device->device_identity.gtin,
                       DALI_MEMORY_BANK0_GTIN_LEN);
            }

            DaliError err = dali_snapshot_add(out, &entry);
            if (err != DALI_OK) {
                return err;
            }
        }
    }

    return DALI_OK;
}

/* ---------------------------------------------------------------------------
 * Codec
 * --------------------------------------------------------------------------*/

static uint32_t snapshot_encoded_size(uint8_t entry_count)
{
    return DALI_SNAPSHOT_HEADER_SIZE +
           ((uint32_t)entry_count * DALI_SNAPSHOT_ENTRY_WIRE_SIZE);
}

DaliError dali_snapshot_encode(const DaliSnapshot *snapshot,
                               uint8_t            *buf,
                               uint32_t            buf_len,
                               uint32_t           *written)
{
    if (snapshot == NULL || buf == NULL || written == NULL ||
        snapshot->entry_count > DALI_SNAPSHOT_MAX_ENTRIES) {
        return DALI_ERR_INVALID;
    }

    const uint32_t need = snapshot_encoded_size(snapshot->entry_count);
    if (buf_len < need) {
        *written = 0u;
        return DALI_ERR_FULL;
    }

    memset(buf, 0, need);
    memcpy(buf, k_snapshot_magic, DALI_SNAPSHOT_MAGIC_LEN);
    buf[4] = DALI_SNAPSHOT_FORMAT_VERSION;
    buf[5] = snapshot->entry_count;
    /* buf[6..7] reserved, already zero. */

    uint32_t offset = DALI_SNAPSHOT_HEADER_SIZE;
    for (uint8_t i = 0u; i < snapshot->entry_count; i++) {
        const DaliSnapshotEntry *entry = &snapshot->entries[i];
        uint8_t *rec = &buf[offset];

        uint8_t flags = 0u;
        if (entry->has_identification) {
            flags |= SNAPSHOT_FLAG_HAS_IDENTIFICATION;
        }
        if (entry->has_gtin) {
            flags |= SNAPSHOT_FLAG_HAS_GTIN;
        }
        if (entry->has_groups) {
            flags |= SNAPSHOT_FLAG_HAS_GROUPS;
        }

        rec[0] = flags;
        rec[1] = (uint8_t)entry->space;
        rec[2] = entry->short_address;
        rec[3] = (uint8_t)(entry->groups & 0xFFu);
        rec[4] = (uint8_t)((entry->groups >> 8) & 0xFFu);
        memcpy(&rec[5], entry->identification, DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
        memcpy(&rec[13], entry->gtin, DALI_MEMORY_BANK0_GTIN_LEN);

        offset += DALI_SNAPSHOT_ENTRY_WIRE_SIZE;
    }

    *written = need;
    return DALI_OK;
}

DaliError dali_snapshot_decode(DaliSnapshot  *out,
                               const uint8_t *buf,
                               uint32_t       len)
{
    if (out == NULL || buf == NULL || len < DALI_SNAPSHOT_HEADER_SIZE) {
        return DALI_ERR_INVALID;
    }

    if (memcmp(buf, k_snapshot_magic, DALI_SNAPSHOT_MAGIC_LEN) != 0) {
        return DALI_ERR_INVALID;
    }
    if (buf[4] != DALI_SNAPSHOT_FORMAT_VERSION) {
        return DALI_ERR_INVALID;
    }

    const uint8_t entry_count = buf[5];
    if (entry_count > DALI_SNAPSHOT_MAX_ENTRIES) {
        return DALI_ERR_INVALID;
    }
    /*
     * Exact length, not "at least". A blob longer than its declared entry count
     * is not a snapshot with slack on the end; it is a blob this decoder does
     * not understand, and guessing which half to trust is how a restore moves a
     * fixture to the wrong address.
     */
    if (len != snapshot_encoded_size(entry_count)) {
        return DALI_ERR_INVALID;
    }

    dali_snapshot_reset(out);

    uint32_t offset = DALI_SNAPSHOT_HEADER_SIZE;
    for (uint8_t i = 0u; i < entry_count; i++) {
        const uint8_t *rec = &buf[offset];
        DaliSnapshotEntry entry;
        memset(&entry, 0, sizeof(entry));

        const uint8_t flags = rec[0];
        const uint8_t space = rec[1];
        if (space != (uint8_t)DALI_SNAPSHOT_SPACE_GEAR &&
            space != (uint8_t)DALI_SNAPSHOT_SPACE_DEVICE) {
            return DALI_ERR_INVALID;
        }
        if (rec[2] >= DALI_SHORT_ADDRESS_COUNT) {
            return DALI_ERR_INVALID;
        }

        entry.space              = (DaliSnapshotSpace)space;
        entry.short_address      = rec[2];
        entry.groups             = (uint16_t)((uint16_t)rec[3] |
                                              ((uint16_t)rec[4] << 8));
        entry.has_identification = (flags & SNAPSHOT_FLAG_HAS_IDENTIFICATION) != 0u;
        entry.has_gtin           = (flags & SNAPSHOT_FLAG_HAS_GTIN) != 0u;
        entry.has_groups         = (flags & SNAPSHOT_FLAG_HAS_GROUPS) != 0u;
        memcpy(entry.identification, &rec[5], DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
        memcpy(entry.gtin, &rec[13], DALI_MEMORY_BANK0_GTIN_LEN);

        DaliError err = dali_snapshot_add(out, &entry);
        if (err != DALI_OK) {
            return err;
        }

        offset += DALI_SNAPSHOT_ENTRY_WIRE_SIZE;
    }

    return DALI_OK;
}
