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

/*
 * Retire `addr` from `group`, or from every group when group is
 * DALI_GROUP_MAP_ALL_GROUPS. This is the operator's counterpart to the
 * conservative retention in dali_group_map_scan_covers_known_members(): gear
 * that has been physically removed never answers a scan again, so no scan can
 * ever drop it, and without an explicit retirement it stays a candidate query
 * target forever.
 *
 * Unlike dali_group_map_apply_config() this describes no DALI command and
 * should send no traffic — the gear is gone. It only edits local bookkeeping.
 * The affected groups stay marked verified or unverified as they were: removing
 * a member does not make the rest of the group's membership any less known.
 *
 * Returns true when a membership bit actually changed.
 */
#define DALI_GROUP_MAP_ALL_GROUPS 0xFFu
bool dali_group_map_forget(DaliGroupMap *map, uint8_t addr, uint8_t group);

/* True when every address previously known as a group member was positively
 * observed as control gear by a replacement scan. This lets integrations
 * reject a nominally complete scan that transiently missed a known device,
 * while still accepting a real removal from all groups when the gear itself
 * answered the scan. */
bool dali_group_map_scan_covers_known_members(const DaliGroupMap *map,
                                              uint64_t observed_gear);

/*
 * Build a candidate map from a completed bus scan. Only present control gear
 * counts; pure input devices are skipped. Returns true and marks every group
 * verified only when every discovered control gear has complete group data and
 * the scan found at least one gear or input device (so an empty/disconnected
 * bus is not authoritative).
 * On false, observed memberships are still copied into `map`, but verified is
 * zero: the candidate is partial and must not replace a known-good snapshot.
 */
bool dali_group_map_rebuild_from_inventory(DaliGroupMap *map,
                                           const DaliDiscoveryInventory *inv);

/* Apply an ADD TO GROUP / REMOVE FROM GROUP command that was issued to `target`,
 * naming destination `group`. Short targets flip exactly that address's bit.
 * Group targets affect all members of the source group, but only if that source
 * group is scan-verified (otherwise its full membership is unknown). */
DaliGroupMapResult dali_group_map_apply_config(DaliGroupMap *map,
                                               DaliTarget target,
                                               DaliCommandId id,
                                               uint8_t group);
