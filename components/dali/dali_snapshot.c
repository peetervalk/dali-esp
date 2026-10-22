#include "dali_snapshot.h"
#include "dali_event.h"   /* DaliEventSourceScheme, the range of a recorded scheme */

#include <string.h>

static const uint8_t k_snapshot_magic[DALI_SNAPSHOT_MAGIC_LEN] = { 'D', 'B', 'K', '1' };

#define SNAPSHOT_FLAG_HAS_IDENTIFICATION 0x01u
#define SNAPSHOT_FLAG_HAS_GTIN           0x02u
#define SNAPSHOT_FLAG_HAS_GROUPS         0x04u

#define INSTANCE_FLAG_HAS_TYPE           0x01u
#define INSTANCE_FLAG_HAS_ENABLED        0x02u
#define INSTANCE_FLAG_ENABLED            0x04u
#define INSTANCE_FLAG_HAS_SCHEME         0x08u
#define INSTANCE_FLAG_HAS_PRIORITY       0x10u
#define INSTANCE_FLAG_HAS_FILTER         0x20u
#define INSTANCE_FLAG_HAS_GROUPS         0x40u
#define INSTANCE_FLAGS_KNOWN             0x7Fu

#define INSTANCE_EVENT_FILTER_MAX        0xFFFFFFu

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

/* Every value a recorded field holds is one the instance could have reported
 * and a restore could send back. */
static bool instance_settings_valid(const DaliInstanceSettings *settings)
{
    if (settings->has_type && settings->type >= DALI_INSTANCE_COUNT) {
        return false;
    }
    if (settings->has_event_scheme &&
        settings->event_scheme > (uint8_t)DALI_EVENT_SOURCE_INSTANCE_GROUP) {
        return false;
    }
    if (settings->has_event_filter &&
        settings->event_filter > INSTANCE_EVENT_FILTER_MAX) {
        return false;
    }
    if (settings->has_instance_groups) {
        for (uint8_t slot = 0u; slot < DALI_INPUT_INSTANCE_GROUP_SLOTS; slot++) {
            const uint8_t g = settings->instance_groups[slot];
            if (g >= DALI_INSTANCE_COUNT && g != DALI_INPUT_INSTANCE_GROUP_NONE) {
                return false;
            }
        }
    }
    return true;
}

static bool snapshot_has_instance(const DaliSnapshot *snapshot,
                                  uint8_t             entry_index,
                                  uint8_t             instance)
{
    for (uint8_t i = 0u; i < snapshot->instance_count; i++) {
        if (snapshot->instances[i].entry_index == entry_index &&
            snapshot->instances[i].instance == instance) {
            return true;
        }
    }
    return false;
}

DaliError dali_snapshot_add_instance(DaliSnapshot               *snapshot,
                                     uint8_t                     entry_index,
                                     uint8_t                     instance,
                                     const DaliInstanceSettings *settings)
{
    if (snapshot == NULL || settings == NULL ||
        entry_index >= snapshot->entry_count ||
        snapshot->entries[entry_index].space != DALI_SNAPSHOT_SPACE_DEVICE ||
        instance >= DALI_INSTANCE_COUNT ||
        !instance_settings_valid(settings) ||
        snapshot_has_instance(snapshot, entry_index, instance)) {
        return DALI_ERR_INVALID;
    }
    if (snapshot->instance_count >= DALI_SNAPSHOT_MAX_INSTANCES) {
        return DALI_ERR_FULL;
    }

    DaliSnapshotInstance *rec = &snapshot->instances[snapshot->instance_count];
    rec->entry_index = entry_index;
    rec->instance    = instance;
    rec->settings    = *settings;
    snapshot->instance_count++;
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
         *
         * A gear address listed as present can still be contested: it answered
         * as one unit and its identity read collided. It is left out for the
         * reason an undecodable one never gets this far. It holds more than one
         * unit and no identity, and an entry would record it as one unit that
         * cannot be restored. The device entry at the same number is
         * independent and still recorded.
         */
        if (device->has_control_gear &&
            !dali_discovery_gear_address_contested(device)) {
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

static uint32_t snapshot_encoded_size(uint8_t entry_count, uint8_t instance_count)
{
    return DALI_SNAPSHOT_HEADER_SIZE +
           ((uint32_t)entry_count * DALI_SNAPSHOT_ENTRY_WIRE_SIZE) +
           ((uint32_t)instance_count * DALI_SNAPSHOT_INSTANCE_WIRE_SIZE);
}

static void encode_instance(const DaliSnapshotInstance *inst, uint8_t *rec)
{
    const DaliInstanceSettings *s = &inst->settings;
    uint8_t flags = 0u;
    if (s->has_type)            flags |= INSTANCE_FLAG_HAS_TYPE;
    if (s->has_enabled)         flags |= INSTANCE_FLAG_HAS_ENABLED;
    if (s->enabled)             flags |= INSTANCE_FLAG_ENABLED;
    if (s->has_event_scheme)    flags |= INSTANCE_FLAG_HAS_SCHEME;
    if (s->has_event_priority)  flags |= INSTANCE_FLAG_HAS_PRIORITY;
    if (s->has_event_filter)    flags |= INSTANCE_FLAG_HAS_FILTER;
    if (s->has_instance_groups) flags |= INSTANCE_FLAG_HAS_GROUPS;

    rec[0] = inst->entry_index;
    rec[1] = inst->instance;
    rec[2] = flags;
    rec[3] = s->type;
    rec[4] = s->event_scheme;
    rec[5] = s->event_priority;
    rec[6] = (uint8_t)(s->event_filter & 0xFFu);
    rec[7] = (uint8_t)((s->event_filter >> 8) & 0xFFu);
    rec[8] = (uint8_t)((s->event_filter >> 16) & 0xFFu);
    memcpy(&rec[9], s->instance_groups, DALI_INPUT_INSTANCE_GROUP_SLOTS);
}

/* The settings exactly as the record holds them; validity is checked apart. */
static void decode_instance(const uint8_t *rec, DaliSnapshotInstance *out)
{
    memset(out, 0, sizeof(*out));
    const uint8_t flags = rec[2];
    DaliInstanceSettings *s = &out->settings;

    out->entry_index       = rec[0];
    out->instance          = rec[1];
    s->has_type            = (flags & INSTANCE_FLAG_HAS_TYPE) != 0u;
    s->has_enabled         = (flags & INSTANCE_FLAG_HAS_ENABLED) != 0u;
    s->enabled             = (flags & INSTANCE_FLAG_ENABLED) != 0u;
    s->has_event_scheme    = (flags & INSTANCE_FLAG_HAS_SCHEME) != 0u;
    s->has_event_priority  = (flags & INSTANCE_FLAG_HAS_PRIORITY) != 0u;
    s->has_event_filter    = (flags & INSTANCE_FLAG_HAS_FILTER) != 0u;
    s->has_instance_groups = (flags & INSTANCE_FLAG_HAS_GROUPS) != 0u;
    s->type                = rec[3];
    s->event_scheme        = rec[4];
    s->event_priority      = rec[5];
    s->event_filter        = (uint32_t)rec[6] |
                             ((uint32_t)rec[7] << 8) |
                             ((uint32_t)rec[8] << 16);
    memcpy(s->instance_groups, &rec[9], DALI_INPUT_INSTANCE_GROUP_SLOTS);
}

DaliError dali_snapshot_encode(const DaliSnapshot *snapshot,
                               uint8_t            *buf,
                               uint32_t            buf_len,
                               uint32_t           *written)
{
    if (snapshot == NULL || buf == NULL || written == NULL ||
        snapshot->entry_count > DALI_SNAPSHOT_MAX_ENTRIES ||
        snapshot->instance_count > DALI_SNAPSHOT_MAX_INSTANCES) {
        return DALI_ERR_INVALID;
    }

    const uint32_t need = snapshot_encoded_size(snapshot->entry_count,
                                                snapshot->instance_count);
    if (buf_len < need) {
        *written = 0u;
        return DALI_ERR_FULL;
    }

    memset(buf, 0, need);
    memcpy(buf, k_snapshot_magic, DALI_SNAPSHOT_MAGIC_LEN);
    buf[4] = DALI_SNAPSHOT_FORMAT_VERSION;
    buf[5] = snapshot->entry_count;
    buf[6] = snapshot->instance_count;
    /* buf[7] reserved, already zero. */

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

    for (uint8_t i = 0u; i < snapshot->instance_count; i++) {
        encode_instance(&snapshot->instances[i], &buf[offset]);
        offset += DALI_SNAPSHOT_INSTANCE_WIRE_SIZE;
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

    const uint8_t entry_count    = buf[5];
    const uint8_t instance_count = buf[6];
    if (entry_count > DALI_SNAPSHOT_MAX_ENTRIES ||
        instance_count > DALI_SNAPSHOT_MAX_INSTANCES || buf[7] != 0u) {
        return DALI_ERR_INVALID;
    }
    /*
     * Exact length, not "at least". A blob longer than its declared counts is
     * not a snapshot with slack on the end; it is a blob this decoder does not
     * understand, and guessing which half to trust is how a restore moves a
     * fixture to the wrong address.
     */
    if (len != snapshot_encoded_size(entry_count, instance_count)) {
        return DALI_ERR_INVALID;
    }

    /*
     * Validate every entry before touching `out`.
     *
     * The alternative -- reset, then fail part way through the loop -- leaves
     * the caller holding a truncated snapshot in place of the one it had, which
     * matters because the caller with the most to lose is `backup import`:
     * decoding a pasted blob over the top of the backup an operator took before
     * a commissioning run. A blob this decoder rejects must cost them nothing.
     */
    uint32_t offset = DALI_SNAPSHOT_HEADER_SIZE;
    for (uint8_t i = 0u; i < entry_count; i++) {
        const uint8_t *rec = &buf[offset];
        if (rec[1] != (uint8_t)DALI_SNAPSHOT_SPACE_GEAR &&
            rec[1] != (uint8_t)DALI_SNAPSHOT_SPACE_DEVICE) {
            return DALI_ERR_INVALID;
        }
        if (rec[2] >= DALI_SHORT_ADDRESS_COUNT) {
            return DALI_ERR_INVALID;
        }
        offset += DALI_SNAPSHOT_ENTRY_WIRE_SIZE;
    }

    /* Instance records, against the entries just checked: each must belong to
     * a device entry, appear once, and hold only values that could be sent. */
    const uint32_t instances_at = offset;
    for (uint8_t i = 0u; i < instance_count; i++) {
        const uint8_t *rec = &buf[instances_at + (uint32_t)i * DALI_SNAPSHOT_INSTANCE_WIRE_SIZE];
        DaliSnapshotInstance inst;
        decode_instance(rec, &inst);

        if (inst.entry_index >= entry_count ||
            buf[DALI_SNAPSHOT_HEADER_SIZE +
                (uint32_t)inst.entry_index * DALI_SNAPSHOT_ENTRY_WIRE_SIZE + 1u] !=
                (uint8_t)DALI_SNAPSHOT_SPACE_DEVICE ||
            inst.instance >= DALI_INSTANCE_COUNT ||
            (rec[2] & (uint8_t)~INSTANCE_FLAGS_KNOWN) != 0u ||
            !instance_settings_valid(&inst.settings)) {
            return DALI_ERR_INVALID;
        }
        for (uint8_t j = 0u; j < i; j++) {
            const uint8_t *other =
                &buf[instances_at + (uint32_t)j * DALI_SNAPSHOT_INSTANCE_WIRE_SIZE];
            if (other[0] == rec[0] && other[1] == rec[1]) {
                return DALI_ERR_INVALID;
            }
        }
    }

    dali_snapshot_reset(out);

    offset = DALI_SNAPSHOT_HEADER_SIZE;
    for (uint8_t i = 0u; i < entry_count; i++) {
        const uint8_t *rec = &buf[offset];
        DaliSnapshotEntry entry;
        memset(&entry, 0, sizeof(entry));

        const uint8_t flags = rec[0];
        const uint8_t space = rec[1];

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

    for (uint8_t i = 0u; i < instance_count; i++) {
        DaliSnapshotInstance inst;
        decode_instance(&buf[offset], &inst);
        DaliError err = dali_snapshot_add_instance(out, inst.entry_index,
                                                   inst.instance, &inst.settings);
        if (err != DALI_OK) {
            return err;
        }
        offset += DALI_SNAPSHOT_INSTANCE_WIRE_SIZE;
    }

    return DALI_OK;
}
