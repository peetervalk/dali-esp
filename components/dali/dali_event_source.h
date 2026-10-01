#pragma once

/*
 * dali_event_source.h - which configured instance an input event could have
 * come from
 *
 * A sensor is configured, and polled, by (short address, instance number).
 * Only a Device/Instance event (eventScheme 2) carries exactly that pair. The
 * other four schemes each carry something else — an instance type, a device
 * group, an instance group — and leave out part of the pair, so matching them
 * to a sensor needs facts about the sensor that only the bus can supply.
 *
 * A profile holds those facts, as read from the instance. Matching follows one
 * rule: a fact the profile does not have never rules a sensor out. An event is
 * only a request to poll, and the poll supplies the value, so matching too many
 * sensors costs a few queries while matching too few leaves a sensor waiting
 * for its poll interval. The profile narrows; it is never needed to match.
 *
 * The profile's scheme is reported, never used to exclude: it is the field
 * that can change under a running installation (a reset, a cleared address,
 * another controller), and an event in a scheme the profile did not expect is
 * evidence that it has. dali_event_source_scheme_differs() flags that case so
 * the caller can read the profile again.
 */

#include "dali_event.h"
#include "dali_protocol.h"
#include "dali_scheduler.h"

/* Instance-group slot holding no group (MASK). */
#define DALI_EVENT_SOURCE_GROUP_NONE      0xFFu
/* Primary, group 1 and group 2. */
#define DALI_EVENT_SOURCE_INSTANCE_GROUPS 3u

/* Steps of the profile read: type, scheme, then the three instance groups. */
#define DALI_EVENT_SOURCE_PROFILE_STEPS   5u

typedef struct {
    bool     has_type;
    uint8_t  type;              /* instance type, 0..31 */
    bool     has_scheme;
    uint8_t  scheme;            /* DaliEventSourceScheme value, 0..4 */
    bool     has_instance_groups;
    uint8_t  instance_groups[DALI_EVENT_SOURCE_INSTANCE_GROUPS]; /* 0..31 or GROUP_NONE */
    bool     has_device_groups;
    uint32_t device_groups;     /* bit N set => the device is in device group N */
} DaliEventSourceProfile;

typedef enum {
    DALI_EVENT_SOURCE_MATCH_NONE = 0,
    /* Device/Instance event naming this address and instance. */
    DALI_EVENT_SOURCE_MATCH_EXACT,
    /* Another scheme, and nothing known about the sensor rules it out. */
    DALI_EVENT_SOURCE_MATCH_POSSIBLE,
} DaliEventSourceMatch;

/* Every field unknown: matches as widely as the event allows. */
void dali_event_source_profile_clear(DaliEventSourceProfile *profile);

/*
 * Could `event` have come from the instance at (address, instance)?
 *
 * Only Part 103 input events match; legacy frames, power notifications and
 * NULL arguments are MATCH_NONE. `profile` may be NULL, which is the same as a
 * cleared profile.
 */
DaliEventSourceMatch dali_event_source_match(const DaliInputEvent *event,
                                             uint8_t address,
                                             uint8_t instance,
                                             const DaliEventSourceProfile *profile);

/*
 * True when the profile knows the instance's scheme and `event` is a Part 103
 * input event in a different one. For an event that matched the instance, that
 * means either the instance's scheme has changed since it was read or the
 * event came from another device; reading the profile again tells which.
 */
bool dali_event_source_scheme_differs(const DaliInputEvent *event,
                                      const DaliEventSourceProfile *profile);

/*
 * Build the profile read for one instance as a scheduler sequence: QUERY
 * INSTANCE TYPE, QUERY EVENT SCHEME, QUERY PRIMARY INSTANCE GROUP, QUERY
 * INSTANCE GROUP 1, QUERY INSTANCE GROUP 2. None depends on the others, so the
 * sequence is for admission and bus economy, not atomicity. Each step may
 * retry once. Device groups are not read.
 */
DaliError dali_event_source_build_profile_sequence(uint8_t address,
                                                   uint8_t instance,
                                                   DaliSequence *out);

/*
 * Fill `out` from a sequence built by dali_event_source_build_profile_sequence().
 *
 * `out` is cleared first and then holds every field whose reply arrived and is
 * in range, so a sequence cut short still yields what it read. The instance
 * groups are taken only as a set of three. Returns DALI_OK when every step
 * answered in range, the sequence's own error when it failed, and
 * DALI_ERR_MALFORMED when it ran but a reply was out of range.
 */
DaliError dali_event_source_profile_from_sequence(const DaliSequenceResult *result,
                                                  DaliEventSourceProfile *out);
