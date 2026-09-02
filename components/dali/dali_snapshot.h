#pragma once

/*
 * dali_snapshot.h - a restorable record of which physical unit holds which
 * short address
 *
 * Commissioning destroys short addresses and nothing else. Groups, scenes and
 * level windows live in each unit's own non-volatile memory and follow the unit
 * through a re-address, so the one thing worth capturing before a destructive
 * run is the mapping from a *physical* unit to the address it held.
 *
 * The only physical anchor a DALI bus offers is the Bank 0 identification
 * number. No addressing operation can change it, which is exactly the property
 * a restore needs: after a commissioning walk has handed out arbitrary
 * addresses, the identification number still says which unit is which, and the
 * recorded layout can be re-established with ordinary addressed SET SHORT
 * ADDRESS commands rather than a second walk.
 *
 * GTIN is deliberately not the anchor. It identifies a product, not a unit:
 * every driver on the 2k bus reports the same GTIN and differs only in
 * identification number. GTIN is carried for reporting and nothing else.
 *
 * This module is the model and its codecs. It performs no bus traffic and has
 * no ESP-IDF, ESPHome, FreeRTOS or task dependency. dali_restore.c turns a
 * snapshot plus a live inventory into a plan; a front end executes it.
 */

#include "dali_discovery.h"
#include "dali_memory.h"

/*
 * Control gear and control devices have independent 0..63 short-address spaces,
 * so one bus can hold 128 addressable units. Sizing for the true worst case
 * costs about 2.4 kB of blob and removes a failure mode from the operator's
 * path: a backup that silently held only part of the bus would be discovered
 * during the restore, which is the worst possible moment.
 */
#define DALI_SNAPSHOT_MAX_ENTRIES     (2u * DALI_SHORT_ADDRESS_COUNT)

#define DALI_SNAPSHOT_FORMAT_VERSION  1u

/* Wire layout, little-endian where it matters. See dali_snapshot_encode(). */
#define DALI_SNAPSHOT_MAGIC_LEN       4u
#define DALI_SNAPSHOT_HEADER_SIZE     8u
#define DALI_SNAPSHOT_ENTRY_WIRE_SIZE 19u
#define DALI_SNAPSHOT_BLOB_MAX \
    (DALI_SNAPSHOT_HEADER_SIZE + \
     (DALI_SNAPSHOT_MAX_ENTRIES * DALI_SNAPSHOT_ENTRY_WIRE_SIZE))

/*
 * Which address space an entry belongs to. The two are independent: control
 * gear at numeric address 7 and a control device at numeric address 7 are
 * different units, and a restore must never let one reserve an address in the
 * other.
 */
typedef enum {
    DALI_SNAPSHOT_SPACE_GEAR   = 0,  /* IEC 62386-102 control gear    */
    DALI_SNAPSHOT_SPACE_DEVICE = 1,  /* IEC 62386-103 control devices */
} DaliSnapshotSpace;

typedef struct {
    DaliSnapshotSpace space;
    uint8_t           short_address;   /* 0..63 */

    /*
     * The anchor. An entry without it cannot be matched to anything on a bus
     * and is retained only so the restore can name what it will not be able to
     * put back, rather than quietly omitting it.
     */
    bool              has_identification;
    uint8_t           identification[DALI_MEMORY_BANK0_IDENTIFICATION_LEN];

    bool              has_gtin;
    uint8_t           gtin[DALI_MEMORY_BANK0_GTIN_LEN];

    /* Gear only; group membership survives re-addressing and is restored only
     * on request, for the case where a RESET wiped it. */
    bool              has_groups;
    uint16_t          groups;          /* bit N set => member of group N */
} DaliSnapshotEntry;

typedef struct {
    uint8_t           version;
    uint8_t           entry_count;
    DaliSnapshotEntry entries[DALI_SNAPSHOT_MAX_ENTRIES];
} DaliSnapshot;

/* ---------------------------------------------------------------------------
 * Model
 * --------------------------------------------------------------------------*/

void dali_snapshot_reset(DaliSnapshot *snapshot);

/*
 * Append one entry. Returns DALI_ERR_FULL when the snapshot is at capacity and
 * DALI_ERR_INVALID on a bad argument or an out-of-range short address.
 */
DaliError dali_snapshot_add(DaliSnapshot *snapshot, const DaliSnapshotEntry *entry);

/*
 * Build a snapshot from a completed discovery inventory. Records every present
 * control gear entry, plus a control-device entry for any address the inventory
 * confirms as an input device.
 *
 * Entries whose identity could not be read are still recorded, without
 * has_identification. Dropping them would make a partial backup look complete.
 */
DaliError dali_snapshot_from_inventory(DaliSnapshot                 *out,
                                       const DaliDiscoveryInventory *inventory);

/* Byte-compare two identification numbers. */
bool dali_snapshot_identification_equal(const uint8_t *a, const uint8_t *b);

/* True if the identification number is all zeroes — some gear answers a memory
 * read with zeroes rather than refusing it, and a bus full of "identical"
 * all-zero units must not be matched to each other. */
bool dali_snapshot_identification_is_null(const uint8_t *identification);

/*
 * Find the entry in `space` whose identification number matches. Returns NULL
 * if absent. When more than one entry matches, the first is returned and
 * `duplicate_out` (optional) is set — the caller must treat that as a conflict
 * rather than a match, because nothing on the bus can separate the two.
 */
const DaliSnapshotEntry *dali_snapshot_find_by_identification(
    const DaliSnapshot *snapshot,
    DaliSnapshotSpace   space,
    const uint8_t      *identification,
    bool               *duplicate_out);

/* Mask of short addresses occupied in `space` according to the snapshot. */
uint64_t dali_snapshot_used_mask(const DaliSnapshot *snapshot,
                                 DaliSnapshotSpace   space);

/* ---------------------------------------------------------------------------
 * Codec
 *
 * Wire format, chosen so a truncated or foreign blob fails to decode rather
 * than producing a plausible-looking snapshot:
 *
 *   0  magic[4]      "DBK1"
 *   4  version       1
 *   5  entry_count   0..DALI_SNAPSHOT_MAX_ENTRIES
 *   6  reserved[2]   zero
 *   8  entries[]     19 bytes each:
 *        +0  flags   bit0 has_identification, bit1 has_gtin, bit2 has_groups
 *        +1  space
 *        +2  short_address
 *        +3  groups, little-endian uint16
 *        +5  identification[8]
 *        +13 gtin[6]
 * --------------------------------------------------------------------------*/

/*
 * Encode into buf. `written` receives the byte count. Returns DALI_ERR_INVALID
 * on a bad argument and DALI_ERR_FULL when buf_len is too small; the required
 * size is never more than DALI_SNAPSHOT_BLOB_MAX.
 */
DaliError dali_snapshot_encode(const DaliSnapshot *snapshot,
                               uint8_t            *buf,
                               uint32_t            buf_len,
                               uint32_t           *written);

/*
 * Decode from buf. Returns DALI_ERR_INVALID on a bad argument, a wrong magic, an
 * unsupported version, an entry count over capacity, or a length that does not
 * match the declared entry count.
 */
DaliError dali_snapshot_decode(DaliSnapshot  *out,
                               const uint8_t *buf,
                               uint32_t       len);
