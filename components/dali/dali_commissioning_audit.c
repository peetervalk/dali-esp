#include "dali_commissioning_audit.h"

#include <string.h>

static uint8_t audit_popcount(uint64_t mask)
{
    uint8_t count = 0u;
    while (mask != 0u) {
        mask &= (mask - 1u);
        count++;
    }
    return count;
}

DaliError dali_commissioning_occupancy_from_inventory(
    const DaliDiscoveryInventory  *inventory,
    DaliCommissioningAddressSpace  space,
    DaliCommissioningOccupancy    *out)
{
    if (inventory == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }
    if (space != DALI_COMMISSIONING_SPACE_GEAR &&
        space != DALI_COMMISSIONING_SPACE_DEVICE) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device =
            dali_discovery_inventory_get(inventory, addr);
        if (device == NULL) {
            continue;
        }

        const bool contested =
            (space == DALI_COMMISSIONING_SPACE_GEAR)
                ? device->has_undecodable_activity
                : device->has_undecodable_device_activity;
        const bool occupied =
            device->present &&
            ((space == DALI_COMMISSIONING_SPACE_GEAR) ? device->has_control_gear
                                                      : device->has_input_device);

        const uint64_t bit = ((uint64_t)1u << addr);
        if (contested) {
            /*
             * Contested wins over occupied, and the two can genuinely coexist
             * in device space: a hybrid unit answers QUERY STATUS as gear at
             * this number, so the scan reaches the instance-count probe through
             * the enrichment path, and that probe can meet undecodable activity
             * from a second control device sharing the device address. Reading
             * the stale instance count as "occupied, one unit" would hide
             * exactly the collision this module exists to find.
             */
            out->contested |= bit;
        } else if (occupied) {
            out->occupied |= bit;
        }
    }

    return DALI_OK;
}

DaliError dali_commissioning_audit(
    const DaliCommissioningOccupancy  *pre,
    const DaliCommissioningOccupancy  *post,
    const DaliCommissioningAssignment *assignments,
    uint8_t                            assignment_count,
    DaliCommissioningAudit            *out)
{
    if (pre == NULL || post == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }
    if (assignment_count > 0u && assignments == NULL) {
        return DALI_ERR_INVALID;
    }
    if (assignment_count > DALI_COMMISSIONING_MAX_ASSIGNMENTS) {
        return DALI_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    uint64_t assigned = 0u;
    for (uint8_t i = 0u; i < assignment_count; i++) {
        const uint8_t addr = assignments[i].short_address;
        if (addr >= DALI_SHORT_ADDRESS_COUNT) {
            return DALI_ERR_INVALID;
        }
        assigned |= ((uint64_t)1u << addr);
    }

    out->confirmed = assigned & post->occupied;
    out->contested = assigned & post->contested;
    out->silent    = assigned & ~post->occupied & ~post->contested;

    /*
     * "The run put this here and did not say so."
     *
     * Both exclusions matter. `~pre->occupied` keeps gear that was already
     * addressed out of it — a run never programs an occupied address, so an
     * address that was occupied before and is occupied now says nothing.
     * `~pre->contested` does the same for an address the pre-scan held out of
     * the free pool: one that resolves to a single readable unit this time
     * changed, but not because the walk wrote to it.
     */
    out->unrecorded =
        post->occupied & ~pre->occupied & ~pre->contested & ~assigned;
    out->newly_contested = post->contested & ~pre->contested & ~assigned;

    out->assigned_count        = audit_popcount(assigned);
    out->confirmed_count       = audit_popcount(out->confirmed);
    out->contested_count       = audit_popcount(out->contested);
    out->silent_count          = audit_popcount(out->silent);
    out->unrecorded_count      = audit_popcount(out->unrecorded);
    out->newly_contested_count = audit_popcount(out->newly_contested);

    return DALI_OK;
}

bool dali_commissioning_audit_is_clean(const DaliCommissioningAudit *audit)
{
    if (audit == NULL) {
        return false;
    }
    return audit->contested_count == 0u &&
           audit->silent_count == 0u &&
           audit->unrecorded_count == 0u &&
           audit->newly_contested_count == 0u;
}
