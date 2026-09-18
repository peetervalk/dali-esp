#pragma once

/*
 * dali_commissioning_audit.h - what a commissioning run actually left on the bus
 *
 * A commissioning walk reports what it believes it did. A scan taken afterwards
 * reports what answers. This module is the difference between the two, and it
 * is deliberately pure: masks in, masks out, no transport, no printing, no
 * knowledge of which address space produced the inventory beyond the one enum
 * that selects the fields to read.
 *
 * It exists as its own module rather than as helpers inside `dali_shell.c`
 * because this is the logic that decides whether an equal-random-address
 * collision is reported or missed, and the shell is not part of the host test
 * build. Everything under it -- the walk, the discovery scan, the reply
 * classification -- has vectors; the comparison that turns their output into a
 * verdict had none.
 *
 * What it can and cannot see, in both spaces:
 *
 *   - It reads a completed inventory. Whatever the scan could not distinguish,
 *     this cannot distinguish either.
 *   - `contested` is undecodable reply-window activity, which is the signature
 *     of two units sharing one short address but is not proof of it. One unit
 *     with a marginal backward waveform presents the same way.
 *   - `silent` is the absence of an answer, which cannot separate gear that
 *     left the bus from gear whose reply landed outside the window.
 *
 * No ESP-IDF, ESPHome, UART or task dependency.
 */

#include "dali_commissioning.h"
#include "dali_discovery.h"

/*
 * Which address space an occupancy view describes.
 *
 * The two are independent: control gear at numeric address 7 and a control
 * device at numeric address 7 are different units, and one physical unit can be
 * both at two unrelated numbers. Mixing them is the fault this enum exists to
 * make impossible to commit silently -- an audit of a Part 103 walk against
 * gear-space occupancy would report every assignment as silent.
 */
typedef enum {
    DALI_COMMISSIONING_SPACE_GEAR = 0,
    DALI_COMMISSIONING_SPACE_DEVICE,
} DaliCommissioningAddressSpace;

/*
 * One address space's occupancy, as one scan saw it.
 *
 * Two masks rather than a copy of the inventory because a caller holding a
 * single 4868-byte inventory buffer overwrites it with the post-scan; sixteen
 * bytes taken before the walk are what let the post-scan tell a contested
 * address the run created from one it inherited and correctly refused to
 * assign.
 *
 * `occupied` and `contested` never share a bit. An address that answered
 * undecodably is contested and nothing else: something is there, and nothing is
 * known about it, so counting it as occupied would claim a reading that does
 * not exist.
 */
typedef struct {
    uint64_t occupied;
    uint64_t contested;
} DaliCommissioningOccupancy;

/*
 * The verdict on one run.
 *
 * The first three masks partition the addresses the run says it assigned; every
 * assigned address lands in exactly one of them. The last two describe
 * addresses the run does not claim, which is where a failed run's damage shows
 * up -- an abort after PROGRAM SHORT ADDRESS went out but before the assignment
 * was recorded leaves an address the walk never mentions.
 */
typedef struct {
    /* Assigned, and one unit answers there. The only good outcome. */
    uint64_t confirmed;
    /*
     * Assigned, and the address answers undecodably. Two gear that generated
     * the same 24-bit random address were selected, programmed and withdrawn as
     * one: the walk counted a single assignment, and both now hold it.
     */
    uint64_t contested;
    /* Assigned, and nothing answers. VERIFY confirmed the write, so the unit
     * took the address and then went quiet. */
    uint64_t silent;
    /*
     * Occupied now, free before, and not claimed as an assignment.
     *
     * A completed run cannot produce these: it only programs addresses the
     * pre-scan proved free, and records every one it programs. A failed run
     * can, and this is the only place it is visible -- the walk aborts between
     * PROGRAM and the assignment record on the VERIFY-silent and
     * QUERY-SHORT-ADDRESS-mismatch paths, leaving a unit addressed and
     * unreported.
     */
    uint64_t unrecorded;
    /*
     * Contested now, not contested before, and not assigned by this run.
     *
     * The run cannot have caused these either. The bus changed underneath it:
     * another master, a unit that was mid-boot during the pre-scan, or one
     * answering undecodably this time and not last time. Worth naming even
     * where the address held healthy gear before -- gear that stops being
     * readable is the same finding whichever direction it came from.
     */
    uint64_t newly_contested;

    uint8_t assigned_count;
    uint8_t confirmed_count;
    uint8_t contested_count;
    uint8_t silent_count;
    uint8_t unrecorded_count;
    uint8_t newly_contested_count;
} DaliCommissioningAudit;

/*
 * Reduce one scan's inventory to the two masks an audit compares.
 *
 * Gear space reads `has_undecodable_activity` and `present && has_control_gear`;
 * device space reads `has_undecodable_device_activity` and
 * `present && has_input_device`. In both, contested is tested first, because an
 * address that answered undecodably has no reading to report.
 */
DaliError dali_commissioning_occupancy_from_inventory(
    const DaliDiscoveryInventory  *inventory,
    DaliCommissioningAddressSpace  space,
    DaliCommissioningOccupancy    *out);

/*
 * Diff a pre-run occupancy against a post-run one, given what the run claims.
 *
 * `assignments` and `assignment_count` come straight out of either walk's
 * result -- both spaces record them in the same DaliCommissioningAssignment
 * array, and only `short_address` is read here, so the caller's space selection
 * is the only thing that has to be right.
 *
 * Duplicate short addresses in the assignment list are counted once;
 * `assigned_count` is the number of distinct addresses claimed, which is what
 * the per-address masks can express.
 */
DaliError dali_commissioning_audit(
    const DaliCommissioningOccupancy  *pre,
    const DaliCommissioningOccupancy  *post,
    const DaliCommissioningAssignment *assignments,
    uint8_t                            assignment_count,
    DaliCommissioningAudit            *out);

/*
 * True when the audit found nothing an operator has to act on: every claimed
 * address confirmed, and no address changed that the run did not claim.
 */
bool dali_commissioning_audit_is_clean(const DaliCommissioningAudit *audit);
