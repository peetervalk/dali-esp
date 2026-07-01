#pragma once

/*
 * dali_group_map.h - group→member bookkeeping for group-addressed lights
 *
 * DALI groups cannot be queried directly (multiple gear would reply and collide
 * on the bus), so a group light polls the ACTUAL LEVEL of one representative
 * member instead. This module tracks which short addresses belong to each group
 * so that representative can be chosen at runtime rather than hardcoded.
 *
 * Pure data logic only: no atomics, no logging, no persistence, no ESP deps.
 * The integration layer (dali_component.cpp) owns one instance, guards it for
 * cross-core access, and handles logging / flash persistence.
 */

#include <stdint.h>
#include <stdbool.h>

#include "dali_control.h"    /* DaliTarget, DaliCommandId (via dali_protocol.h) */
#include "dali_discovery.h"  /* DaliDiscoveryInventory */

#define DALI_GROUP_COUNT 16u

typedef struct {
    uint64_t members[DALI_GROUP_COUNT]; /* bit N set => short addr N is in group */
    uint16_t verified;                  /* bit g set => group g came from a bus scan */
} DaliGroupMap;

/* Outcome of applying a group config command, so the caller can log/persist. */
typedef enum {
    DALI_GROUP_MAP_NO_CHANGE = 0,     /* not a group command, or nothing changed */
    DALI_GROUP_MAP_UPDATED,           /* one or more membership bits changed */
    DALI_GROUP_MAP_UNVERIFIED_ADD,    /* group-target add, source group not scan-verified:
                                       * destination kept as-is (partial), left unverified */
    DALI_GROUP_MAP_UNVERIFIED_REMOVE  /* group-target remove, source group not scan-verified:
                                       * destination cache cleared, left unverified */
} DaliGroupMapResult;

/* Clear all membership and verified state. */
void dali_group_map_reset(DaliGroupMap *map);

/* Cold-start seed: mark short address `addr` a (unverified) member of `group`.
 * No-op if group >= 16 or addr >= 64. */
void dali_group_map_seed(DaliGroupMap *map, uint8_t group, uint8_t addr);

/* Lowest short address currently known in `group`, or 0xFF if none / invalid. */
uint8_t dali_group_map_pick(const DaliGroupMap *map, uint8_t group);

/* Replace the whole map from a completed bus scan; marks every group verified.
 * Only present, group-capable control gear counts (pure input devices skipped). */
void dali_group_map_rebuild_from_inventory(DaliGroupMap *map,
                                           const DaliDiscoveryInventory *inv);

/* Apply an ADD TO GROUP / REMOVE FROM GROUP command that was issued to `target`,
 * naming destination `group`. Short targets flip exactly that address's bit.
 * Group targets affect all members of the source group, but only if that source
 * group is scan-verified (otherwise its full membership is unknown). */
DaliGroupMapResult dali_group_map_apply_config(DaliGroupMap *map,
                                               DaliTarget target,
                                               DaliCommandId id,
                                               uint8_t group);
