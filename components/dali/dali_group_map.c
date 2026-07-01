#include "dali_group_map.h"

#include <string.h>

#include "dali_frame.h"  /* DALI_SHORT_ADDRESS_COUNT, DaliAddressType */

void dali_group_map_reset(DaliGroupMap *map)
{
    if (map == NULL) {
        return;
    }
    memset(map, 0, sizeof(*map));
}

void dali_group_map_seed(DaliGroupMap *map, uint8_t group, uint8_t addr)
{
    if (map == NULL || group >= DALI_GROUP_COUNT || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return;
    }
    map->members[group] |= (uint64_t)1u << addr;
}

uint8_t dali_group_map_pick(const DaliGroupMap *map, uint8_t group)
{
    if (map == NULL || group >= DALI_GROUP_COUNT) {
        return 0xFFu;
    }
    uint64_t mask = map->members[group];
    for (uint8_t a = 0u; a < DALI_SHORT_ADDRESS_COUNT; a++) {
        if ((mask >> a) & 1u) {
            return a;
        }
    }
    return 0xFFu;
}

void dali_group_map_rebuild_from_inventory(DaliGroupMap *map,
                                           const DaliDiscoveryInventory *inv)
{
    if (map == NULL || inv == NULL) {
        return;
    }
    dali_group_map_reset(map);
    for (uint8_t a = 0u; a < DALI_SHORT_ADDRESS_COUNT; a++) {
        const DaliDiscoveryDeviceInfo *d = dali_discovery_inventory_get(inv, a);
        if (d == NULL || !d->present || !d->has_groups || !d->has_control_gear) {
            continue;
        }
        for (uint8_t g = 0u; g < DALI_GROUP_COUNT; g++) {
            if (d->groups & (1u << g)) {
                map->members[g] |= (uint64_t)1u << a;
            }
        }
    }
    map->verified = 0xFFFFu;
}

DaliGroupMapResult dali_group_map_apply_config(DaliGroupMap *map,
                                               DaliTarget target,
                                               DaliCommandId id,
                                               uint8_t group)
{
    if (map == NULL || group >= DALI_GROUP_COUNT ||
        (id != DALI_CMD_ADD_TO_GROUP && id != DALI_CMD_REMOVE_FROM_GROUP)) {
        return DALI_GROUP_MAP_NO_CHANGE;
    }

    uint64_t affected;
    if (target.type == DALI_ADDR_SHORT) {
        if (target.address >= DALI_SHORT_ADDRESS_COUNT) {
            return DALI_GROUP_MAP_NO_CHANGE;
        }
        affected = (uint64_t)1u << target.address;
    } else if (target.type == DALI_ADDR_GROUP && target.address < DALI_GROUP_COUNT) {
        if (((map->verified >> target.address) & 1u) == 0u) {
            /* Source group's membership isn't fully known, so we can't compute
             * the exact set of devices this command affects. The destination
             * group's membership is now incomplete → mark it unverified. */
            map->verified &= (uint16_t)~(1u << group);
            if (id == DALI_CMD_REMOVE_FROM_GROUP) {
                /* Some current members of `group` may have just left, so cached
                 * poll targets could be stale — drop them until the next scan. */
                map->members[group] = 0u;
                return DALI_GROUP_MAP_UNVERIFIED_REMOVE;
            }
            /* For add-group nobody leaves `group`; existing poll targets stay
             * valid, we just can't add the unknown newcomers — so keep them. */
            return DALI_GROUP_MAP_UNVERIFIED_ADD;
        }
        affected = map->members[target.address];
    } else {
        return DALI_GROUP_MAP_NO_CHANGE;
    }

    uint64_t before = map->members[group];
    if (id == DALI_CMD_ADD_TO_GROUP) {
        map->members[group] |= affected;
    } else {
        map->members[group] &= ~affected;
    }
    return (map->members[group] == before) ? DALI_GROUP_MAP_NO_CHANGE
                                           : DALI_GROUP_MAP_UPDATED;
}
