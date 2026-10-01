#include "unity.h"

#include "dali_event_source.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/*
 * The 2k Steinel's two heartbeats in both schemes it has been seen in. Device 0
 * instance 1 is occupancy (type 3), instance 0 is lux (type 4). The scheme-2
 * forms are from the 2026-09-04 capture; the scheme-0 forms are what the
 * integration decoded on 2026-10-01 as "type=3 inst=1" and "type=4 inst=0".
 */
#define STEINEL_OCC_DEVICE_INSTANCE 0x00840Cu
#define STEINEL_LUX_DEVICE_INSTANCE 0x008001u
#define STEINEL_OCC_INSTANCE        0x86840Cu
#define STEINEL_LUX_INSTANCE        0x888001u

static DaliInputEvent parse_24(uint32_t raw)
{
    DaliFrame frame = {
        .data = raw,
        .bit_length = DALI_EXTENDED_FRAME_BITS,
    };
    DaliInputEvent event;

    TEST_ASSERT_EQUAL(DALI_OK, dali_event_parse_frame(&frame, &event));
    return event;
}

static DaliEventSourceProfile profile_of_type(uint8_t type)
{
    DaliEventSourceProfile p;
    dali_event_source_profile_clear(&p);
    p.has_type = true;
    p.type = type;
    return p;
}

static DaliEventSourceProfile profile_with_groups(uint8_t g0, uint8_t g1, uint8_t g2)
{
    DaliEventSourceProfile p;
    dali_event_source_profile_clear(&p);
    p.has_instance_groups = true;
    p.instance_groups[0] = g0;
    p.instance_groups[1] = g1;
    p.instance_groups[2] = g2;
    return p;
}

/* A sequence result whose first `answered` steps replied with `values`, ending
 * with `error` at the next step when `error` is not DALI_OK. */
static DaliSequenceResult result_with(const uint8_t *values, uint8_t answered,
                                      DaliError error)
{
    DaliSequenceResult r;
    memset(&r, 0, sizeof(r));
    for (uint8_t i = 0u; i < answered; i++) {
        r.replies[i] = (DaliFrame){ .data = values[i],
                                    .bit_length = DALI_BACKWARD_FRAME_BITS };
        r.reply_mask |= (uint8_t)(1u << i);
    }
    r.result = error;
    if (error == DALI_OK) {
        r.failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
        r.steps_run = answered;
    } else {
        r.failed_step = answered;
        r.steps_run = (uint8_t)(answered + 1u);
    }
    return r;
}

/* ── Device/Instance: exact, and the profile is never consulted ─────────── */

void test_device_instance_matches_its_own_pair_exactly(void)
{
    DaliInputEvent ev = parse_24(STEINEL_OCC_DEVICE_INSTANCE);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_EXACT,
                      dali_event_source_match(&ev, 0u, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&ev, 0u, 0u, NULL));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&ev, 1u, 1u, NULL));
}

void test_device_instance_ignores_a_contradicting_profile(void)
{
    /* The wire names the instance; a stale profile must not veto it. */
    DaliInputEvent ev = parse_24(STEINEL_OCC_DEVICE_INSTANCE);
    DaliEventSourceProfile wrong = profile_of_type(4u);
    wrong.has_scheme = true;
    wrong.scheme = DALI_EVENT_SOURCE_INSTANCE;

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_EXACT,
                      dali_event_source_match(&ev, 0u, 1u, &wrong));
}

/* ── Instance scheme: the 2026-10-01 case ─────────────────────────────── */

void test_instance_scheme_matches_by_instance_number_when_nothing_is_known(void)
{
    DaliInputEvent ev = parse_24(STEINEL_OCC_INSTANCE);
    DaliEventSourceProfile unknown;
    dali_event_source_profile_clear(&unknown);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_INSTANCE, ev.source.scheme);
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&ev, 0u, 1u, &unknown));
    /* No address on the wire: instance 1 at any address could have sent it. */
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&ev, 17u, 1u, &unknown));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&ev, 0u, 0u, &unknown));
}

void test_instance_scheme_narrows_by_known_type(void)
{
    DaliEventSourceProfile occ = profile_of_type(3u);
    DaliEventSourceProfile lux = profile_of_type(4u);
    DaliInputEvent occ_ev = parse_24(STEINEL_OCC_INSTANCE);
    DaliInputEvent lux_ev = parse_24(STEINEL_LUX_INSTANCE);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&occ_ev, 0u, 1u, &occ));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&occ_ev, 0u, 1u, &lux));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&lux_ev, 0u, 0u, &lux));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&lux_ev, 0u, 1u, &occ));
}

/* ── Device scheme: address and type ──────────────────────────────────── */

void test_device_scheme_matches_by_address_and_type(void)
{
    /* Device 0, type 3, event information 0x00C. */
    DaliInputEvent ev = parse_24(0x000C0Cu);
    DaliEventSourceProfile occ = profile_of_type(3u);
    DaliEventSourceProfile lux = profile_of_type(4u);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_DEVICE, ev.source.scheme);
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&ev, 0u, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&ev, 0u, 1u, &occ));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&ev, 0u, 0u, &lux));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&ev, 1u, 1u, NULL));
}

/* ── Group schemes ────────────────────────────────────────────────────── */

void test_device_group_scheme_uses_known_membership(void)
{
    /* Device group 15, type 1, long press start. */
    DaliInputEvent ev = parse_24(0x9E0409u);
    DaliEventSourceProfile in_15 = profile_of_type(1u);
    in_15.has_device_groups = true;
    in_15.device_groups = (uint32_t)1u << 15u;
    DaliEventSourceProfile in_16 = in_15;
    in_16.device_groups = (uint32_t)1u << 16u;

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_DEVICE_GROUP, ev.source.scheme);
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&ev, 9u, 4u, NULL));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&ev, 9u, 4u, &in_15));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&ev, 9u, 4u, &in_16));
}

void test_instance_group_scheme_checks_all_three_slots(void)
{
    /* Instance group 3, type 3, event information 0x00B. */
    DaliInputEvent ev = parse_24(0xC60C0Bu);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_INSTANCE_GROUP, ev.source.scheme);
    TEST_ASSERT_EQUAL_UINT8(3u, ev.source.instance_group);
    TEST_ASSERT_EQUAL_UINT8(3u, ev.source.instance_type);

    DaliEventSourceProfile primary = profile_with_groups(3u, 0xFFu, 0xFFu);
    DaliEventSourceProfile second  = profile_with_groups(0xFFu, 7u, 3u);
    DaliEventSourceProfile others  = profile_with_groups(1u, 2u, 0xFFu);
    DaliEventSourceProfile none    = profile_with_groups(0xFFu, 0xFFu, 0xFFu);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&ev, 2u, 5u, NULL));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&ev, 2u, 5u, &primary));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&ev, 2u, 5u, &second));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&ev, 2u, 5u, &others));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&ev, 2u, 5u, &none));
}

void test_instance_group_scheme_also_narrows_by_type(void)
{
    DaliInputEvent ev = parse_24(0xC60C0Bu);
    DaliEventSourceProfile lux_in_3 = profile_with_groups(3u, 0xFFu, 0xFFu);
    lux_in_3.has_type = true;
    lux_in_3.type = 4u;

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&ev, 2u, 5u, &lux_in_3));
}

/* ── What never matches ───────────────────────────────────────────────── */

void test_non_input_frames_and_bad_targets_never_match(void)
{
    /* Group 0, RECALL MAX LEVEL: a DALI-1 coupler's frame. */
    DaliFrame legacy_frame = { .data = 0x8105u, .bit_length = DALI_FORWARD_FRAME_BITS };
    DaliInputEvent legacy;
    TEST_ASSERT_EQUAL(DALI_OK, dali_event_parse_frame(&legacy_frame, &legacy));
    DaliInputEvent power = parse_24(0xFEE000u);
    DaliInputEvent occ = parse_24(STEINEL_OCC_INSTANCE);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&legacy, 0u, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&power, 0u, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(NULL, 0u, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&occ, 64u, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&occ, 0u, 32u, NULL));
}

/* ── Scheme change detection ─────────────────────────────────────────── */

void test_scheme_differs_only_against_a_known_scheme(void)
{
    DaliInputEvent scheme0 = parse_24(STEINEL_OCC_INSTANCE);
    DaliInputEvent scheme2 = parse_24(STEINEL_OCC_DEVICE_INSTANCE);
    DaliEventSourceProfile p;
    dali_event_source_profile_clear(&p);

    TEST_ASSERT_FALSE(dali_event_source_scheme_differs(&scheme0, &p));

    p.has_scheme = true;
    p.scheme = DALI_EVENT_SOURCE_DEVICE_INSTANCE;
    TEST_ASSERT_TRUE(dali_event_source_scheme_differs(&scheme0, &p));
    TEST_ASSERT_FALSE(dali_event_source_scheme_differs(&scheme2, &p));

    /* And the way back: the operator restored scheme 2. */
    p.scheme = DALI_EVENT_SOURCE_INSTANCE;
    TEST_ASSERT_TRUE(dali_event_source_scheme_differs(&scheme2, &p));

    DaliInputEvent power = parse_24(0xFEE000u);
    TEST_ASSERT_FALSE(dali_event_source_scheme_differs(&power, &p));
    TEST_ASSERT_FALSE(dali_event_source_scheme_differs(NULL, &p));
    TEST_ASSERT_FALSE(dali_event_source_scheme_differs(&scheme2, NULL));
}

/* ── Profile read ────────────────────────────────────────────────────── */

void test_profile_sequence_reads_type_scheme_and_groups(void)
{
    DaliSequence seq;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_event_source_build_profile_sequence(0u, 1u, &seq));

    /* Independent vectors: address byte 0x01, instance 0x01, Part 103 opcode. */
    static const uint32_t expected[DALI_EVENT_SOURCE_PROFILE_STEPS] = {
        0x010180u, 0x01018Bu, 0x010188u, 0x010189u, 0x01018Au,
    };
    TEST_ASSERT_EQUAL_UINT8(DALI_EVENT_SOURCE_PROFILE_STEPS, seq.step_count);
    for (uint8_t i = 0u; i < DALI_EVENT_SOURCE_PROFILE_STEPS; i++) {
        TEST_ASSERT_EQUAL_HEX32(expected[i], seq.steps[i].frame.data);
        TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, seq.steps[i].frame.bit_length);
        TEST_ASSERT_TRUE(seq.steps[i].needs_reply);
        TEST_ASSERT_FALSE(seq.steps[i].send_twice);
        TEST_ASSERT_EQUAL_UINT8(1u, seq.steps[i].retries_left);
    }
    TEST_ASSERT_NULL(seq.on_complete);
}

void test_profile_sequence_rejects_bad_targets(void)
{
    DaliSequence seq;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_event_source_build_profile_sequence(64u, 0u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_event_source_build_profile_sequence(0u, 32u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_event_source_build_profile_sequence(0u, 0u, NULL));
}

void test_profile_from_complete_read(void)
{
    /* The Steinel's occupancy instance as found on 2026-10-01. */
    const uint8_t values[] = { 3u, 0u, 0xFFu, 0xFFu, 0xFFu };
    DaliSequenceResult r = result_with(values, 5u, DALI_OK);
    DaliEventSourceProfile p;

    TEST_ASSERT_EQUAL(DALI_OK, dali_event_source_profile_from_sequence(&r, &p));
    TEST_ASSERT_TRUE(p.has_type);
    TEST_ASSERT_EQUAL_UINT8(3u, p.type);
    TEST_ASSERT_TRUE(p.has_scheme);
    TEST_ASSERT_EQUAL_UINT8(DALI_EVENT_SOURCE_INSTANCE, p.scheme);
    TEST_ASSERT_TRUE(p.has_instance_groups);
    for (uint8_t i = 0u; i < DALI_EVENT_SOURCE_INSTANCE_GROUPS; i++) {
        TEST_ASSERT_EQUAL_HEX8(DALI_EVENT_SOURCE_GROUP_NONE, p.instance_groups[i]);
    }
    TEST_ASSERT_FALSE(p.has_device_groups);
}

void test_profile_keeps_what_a_cut_short_read_got(void)
{
    const uint8_t values[] = { 3u, 2u };
    DaliSequenceResult r = result_with(values, 2u, DALI_ERR_TIMEOUT);
    DaliEventSourceProfile p;

    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, dali_event_source_profile_from_sequence(&r, &p));
    TEST_ASSERT_TRUE(p.has_type);
    TEST_ASSERT_TRUE(p.has_scheme);
    TEST_ASSERT_EQUAL_UINT8(DALI_EVENT_SOURCE_DEVICE_INSTANCE, p.scheme);
    TEST_ASSERT_FALSE(p.has_instance_groups);
}

void test_profile_of_a_silent_instance_is_all_unknown(void)
{
    DaliSequenceResult r = result_with(NULL, 0u, DALI_ERR_TIMEOUT);
    DaliEventSourceProfile p;
    p.has_type = true;   /* proves the parser clears what it was handed */

    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, dali_event_source_profile_from_sequence(&r, &p));
    TEST_ASSERT_FALSE(p.has_type);
    TEST_ASSERT_FALSE(p.has_scheme);
    TEST_ASSERT_FALSE(p.has_instance_groups);
}

void test_profile_drops_out_of_range_replies(void)
{
    const uint8_t values[] = { 40u, 5u, 3u, 32u, 0xFFu };
    DaliSequenceResult r = result_with(values, 5u, DALI_OK);
    DaliEventSourceProfile p;

    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED, dali_event_source_profile_from_sequence(&r, &p));
    TEST_ASSERT_FALSE(p.has_type);
    TEST_ASSERT_FALSE(p.has_scheme);
    /* One bad slot voids the set: a partial set would exclude wrongly. */
    TEST_ASSERT_FALSE(p.has_instance_groups);
}

void test_profile_ignores_a_reply_of_the_wrong_width(void)
{
    const uint8_t values[] = { 3u, 2u, 0xFFu, 0xFFu, 0xFFu };
    DaliSequenceResult r = result_with(values, 5u, DALI_OK);
    r.replies[0].bit_length = DALI_FORWARD_FRAME_BITS;
    DaliEventSourceProfile p;

    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED, dali_event_source_profile_from_sequence(&r, &p));
    TEST_ASSERT_FALSE(p.has_type);
    TEST_ASSERT_TRUE(p.has_scheme);
}

void test_profile_rejects_null_arguments(void)
{
    DaliEventSourceProfile p;
    p.has_type = true;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_event_source_profile_from_sequence(NULL, &p));
    TEST_ASSERT_FALSE(p.has_type);

    DaliSequenceResult r = result_with(NULL, 0u, DALI_OK);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_event_source_profile_from_sequence(&r, NULL));
}

/* ── The incident, end to end ─────────────────────────────────────────── */

void test_scheme_zero_steinel_reaches_only_the_right_sensor(void)
{
    /* Both instances read as found on 2026-10-01: scheme 0, no groups. */
    const uint8_t occ_values[] = { 3u, 0u, 0xFFu, 0xFFu, 0xFFu };
    const uint8_t lux_values[] = { 4u, 0u, 0xFFu, 0xFFu, 0xFFu };
    DaliSequenceResult occ_r = result_with(occ_values, 5u, DALI_OK);
    DaliSequenceResult lux_r = result_with(lux_values, 5u, DALI_OK);
    DaliEventSourceProfile occ, lux;
    TEST_ASSERT_EQUAL(DALI_OK, dali_event_source_profile_from_sequence(&occ_r, &occ));
    TEST_ASSERT_EQUAL(DALI_OK, dali_event_source_profile_from_sequence(&lux_r, &lux));

    DaliInputEvent occ_ev = parse_24(STEINEL_OCC_INSTANCE);
    DaliInputEvent lux_ev = parse_24(STEINEL_LUX_INSTANCE);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&occ_ev, 0u, 1u, &occ));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&occ_ev, 0u, 0u, &lux));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_POSSIBLE,
                      dali_event_source_match(&lux_ev, 0u, 0u, &lux));
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_MATCH_NONE,
                      dali_event_source_match(&lux_ev, 0u, 1u, &occ));

    /* The profile agrees with the events, so nothing is flagged. */
    TEST_ASSERT_FALSE(dali_event_source_scheme_differs(&occ_ev, &occ));
    /* After the fix, the first scheme-2 heartbeat flags the stale profile. */
    DaliInputEvent fixed = parse_24(STEINEL_OCC_DEVICE_INSTANCE);
    TEST_ASSERT_TRUE(dali_event_source_scheme_differs(&fixed, &occ));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_device_instance_matches_its_own_pair_exactly);
    RUN_TEST(test_device_instance_ignores_a_contradicting_profile);
    RUN_TEST(test_instance_scheme_matches_by_instance_number_when_nothing_is_known);
    RUN_TEST(test_instance_scheme_narrows_by_known_type);
    RUN_TEST(test_device_scheme_matches_by_address_and_type);
    RUN_TEST(test_device_group_scheme_uses_known_membership);
    RUN_TEST(test_instance_group_scheme_checks_all_three_slots);
    RUN_TEST(test_instance_group_scheme_also_narrows_by_type);
    RUN_TEST(test_non_input_frames_and_bad_targets_never_match);
    RUN_TEST(test_scheme_differs_only_against_a_known_scheme);
    RUN_TEST(test_profile_sequence_reads_type_scheme_and_groups);
    RUN_TEST(test_profile_sequence_rejects_bad_targets);
    RUN_TEST(test_profile_from_complete_read);
    RUN_TEST(test_profile_keeps_what_a_cut_short_read_got);
    RUN_TEST(test_profile_of_a_silent_instance_is_all_unknown);
    RUN_TEST(test_profile_drops_out_of_range_replies);
    RUN_TEST(test_profile_ignores_a_reply_of_the_wrong_width);
    RUN_TEST(test_profile_rejects_null_arguments);
    RUN_TEST(test_scheme_zero_steinel_reaches_only_the_right_sensor);
    return UNITY_END();
}
