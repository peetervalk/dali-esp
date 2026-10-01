/*
 * test_restore_instances.c — recording and restoring a control device's
 * instance settings: the read (dali_restore_read_instance_settings()), the
 * decision what to write (dali_restore_instance_write_mask()), finding the
 * device by identification number (dali_restore_locate_device()), and the
 * writes (dali_restore_write_instance_settings()).
 *
 * The mock is a control device with real registers. It answers the Part 103
 * instance queries from them, keeps its own DTR0-2, and applies the
 * configuration commands a restore sends, so a read after a write says what a
 * device would now report rather than which frames went out.
 */

#include "unity.h"
#include "dali_restore.h"

#include <string.h>

typedef struct {
    bool    present;
    uint8_t type;
    bool    enabled;
    uint8_t status;          /* QUERY INSTANCE STATUS reply */
    uint8_t scheme;
    uint8_t priority;
    uint8_t filter[3];
    uint8_t groups[3];
    bool    filter_silent;   /* answers no QUERY EVENT FILTER */
    bool    enabled_lost;    /* the YES to QUERY INSTANCE ENABLED goes missing */
} MockInstance;

#define MOCK_ADDR 0u
static MockInstance s_inst[4];
static uint8_t      s_dtr[3];
static DaliError    s_hard_error_opcode_err;
static uint8_t      s_hard_error_opcode;      /* 0 = none */

#define MOCK_LOG_MAX 32u
static DaliSequenceStep s_steps[MOCK_LOG_MAX];
static uint8_t          s_step_count;
static uint8_t          s_query_count;

static DaliDiscoveryInventory s_inventory;
static DaliSnapshot           s_snapshot;

void setUp(void)
{
    memset(s_inst, 0, sizeof(s_inst));
    memset(s_dtr, 0, sizeof(s_dtr));
    memset(s_steps, 0, sizeof(s_steps));
    s_step_count = 0u;
    s_query_count = 0u;
    s_hard_error_opcode = 0u;
    s_hard_error_opcode_err = DALI_OK;
    memset(&s_inventory, 0, sizeof(s_inventory));
    s_inventory.valid = true;
    dali_snapshot_reset(&s_snapshot);
}

void tearDown(void) {}

/* The 2k Steinel's occupancy instance as found on 2026-10-01: scheme 0. */
static void steinel_occupancy(MockInstance *m)
{
    m->present   = true;
    m->type      = 3u;
    m->enabled   = true;
    m->status    = 0x02u;
    m->scheme    = 0u;
    m->priority  = 4u;
    m->filter[0] = 0x07u;
    m->groups[0] = m->groups[1] = m->groups[2] = 0xFFu;
}

static DaliError reply_byte(uint8_t value, DaliFrame *reply_out)
{
    *reply_out = (DaliFrame){ .data = value, .bit_length = DALI_BACKWARD_FRAME_BITS };
    return DALI_OK;
}

static DaliError mock_transact(const DaliFrame *frame, bool needs_reply,
                               uint8_t retries_left, bool send_twice,
                               DaliFrame *reply_out, void *ctx)
{
    (void)retries_left;
    (void)send_twice;
    (void)ctx;
    TEST_ASSERT_TRUE(needs_reply);
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, frame->bit_length);
    s_query_count++;

    const uint8_t addr_byte = (uint8_t)(frame->data >> 16);
    const uint8_t instance  = (uint8_t)(frame->data >> 8);
    const uint8_t opcode    = (uint8_t)frame->data;
    TEST_ASSERT_EQUAL_HEX8((uint8_t)((MOCK_ADDR << 1) | 1u), addr_byte);

    if (s_hard_error_opcode != 0u && opcode == s_hard_error_opcode) {
        return s_hard_error_opcode_err;
    }
    if (instance >= 4u || !s_inst[instance].present) {
        return DALI_ERR_TIMEOUT;
    }
    const MockInstance *m = &s_inst[instance];
    switch (opcode) {
        case 0x80u: return reply_byte(m->type, reply_out);
        case 0x83u: return reply_byte(m->status, reply_out);
        case 0x84u: return reply_byte(m->priority, reply_out);
        case 0x86u:
            /* YES or nothing: a disabled instance and a lost YES look alike. */
            return (m->enabled && !m->enabled_lost) ? reply_byte(0xFFu, reply_out)
                                                    : DALI_ERR_TIMEOUT;
        case 0x88u: return reply_byte(m->groups[0], reply_out);
        case 0x89u: return reply_byte(m->groups[1], reply_out);
        case 0x8Au: return reply_byte(m->groups[2], reply_out);
        case 0x8Bu: return reply_byte(m->scheme, reply_out);
        case 0x90u:
        case 0x91u:
        case 0x92u:
            return m->filter_silent ? DALI_ERR_TIMEOUT
                                    : reply_byte(m->filter[opcode - 0x90u], reply_out);
        default:
            return DALI_ERR_TIMEOUT;
    }
}

/* Applies DTR loads and the configuration commands a restore sends. */
static DaliError mock_transact_sequence(const DaliSequence *seq,
                                        DaliSequenceResult *result_out, void *ctx)
{
    (void)ctx;
    if (result_out != NULL) {
        memset(result_out, 0, sizeof(*result_out));
        result_out->failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
    }
    for (uint8_t i = 0u; i < seq->step_count; i++) {
        const DaliSequenceStep *step = &seq->steps[i];
        TEST_ASSERT_TRUE(s_step_count < MOCK_LOG_MAX);
        s_steps[s_step_count++] = *step;

        const uint32_t d = step->frame.data;
        if ((d >> 16) == 0xC1u && ((d >> 8) & 0xFFu) >= 0x30u &&
            ((d >> 8) & 0xFFu) <= 0x32u) {
            s_dtr[((d >> 8) & 0xFFu) - 0x30u] = (uint8_t)d;
            continue;
        }
        TEST_ASSERT_TRUE(step->send_twice);
        const uint8_t instance = (uint8_t)(d >> 8);
        MockInstance *m = &s_inst[instance];
        switch ((uint8_t)d) {
            case 0x61u: m->priority = s_dtr[0]; break;
            case 0x62u: m->enabled = true;  m->status |= 0x02u; break;
            case 0x63u: m->enabled = false; m->status &= (uint8_t)~0x02u; break;
            case 0x64u: m->groups[0] = s_dtr[0]; break;
            case 0x65u: m->groups[1] = s_dtr[0]; break;
            case 0x66u: m->groups[2] = s_dtr[0]; break;
            case 0x67u: m->scheme = s_dtr[0]; break;
            case 0x68u: memcpy(m->filter, s_dtr, 3u); break;
            default: TEST_FAIL_MESSAGE("unexpected configuration opcode");
        }
    }
    if (result_out != NULL) {
        result_out->steps_run = seq->step_count;
    }
    return DALI_OK;
}

static DaliTransport transport(void)
{
    DaliTransport t = {
        .transact          = mock_transact,
        .transact_sequence = mock_transact_sequence,
    };
    return t;
}

/* ── Read ─────────────────────────────────────────────────────────────── */

void test_read_gets_every_field(void)
{
    steinel_occupancy(&s_inst[1]);
    DaliTransport t = transport();
    DaliInstanceSettings s;

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_restore_read_instance_settings(&t, 0u, 1u, &s));
    TEST_ASSERT_TRUE(s.has_type);
    TEST_ASSERT_EQUAL_UINT8(3u, s.type);
    TEST_ASSERT_TRUE(s.has_enabled);
    TEST_ASSERT_TRUE(s.enabled);
    TEST_ASSERT_TRUE(s.has_event_scheme);
    TEST_ASSERT_EQUAL_UINT8(0u, s.event_scheme);
    TEST_ASSERT_TRUE(s.has_event_priority);
    TEST_ASSERT_EQUAL_UINT8(4u, s.event_priority);
    TEST_ASSERT_TRUE(s.has_event_filter);
    TEST_ASSERT_EQUAL_HEX32(0x000007u, s.event_filter);
    TEST_ASSERT_TRUE(s.has_instance_groups);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, s.instance_groups[0]);
}

void test_read_of_an_absent_instance_asks_nothing_more(void)
{
    DaliTransport t = transport();
    DaliInstanceSettings s;

    TEST_ASSERT_EQUAL_INT(DALI_ERR_TIMEOUT,
                          dali_restore_read_instance_settings(&t, 0u, 2u, &s));
    TEST_ASSERT_EQUAL_UINT8(1u, s_query_count);
    TEST_ASSERT_FALSE(s.has_type);
}

/*
 * The 2k Steinel's instances 2 and 3: no answer to QUERY INSTANCE ENABLED and
 * status 0x00. Both signals agree, so it is recorded as disabled.
 */
void test_silence_with_an_inactive_status_reads_as_disabled(void)
{
    steinel_occupancy(&s_inst[2]);
    s_inst[2].enabled = false;
    s_inst[2].status  = 0x00u;
    DaliTransport t = transport();
    DaliInstanceSettings s;

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_restore_read_instance_settings(&t, 0u, 2u, &s));
    TEST_ASSERT_TRUE(s.has_enabled);
    TEST_ASSERT_FALSE(s.enabled);
}

/* A lost YES with an active status is not recorded as disabled: a restore
 * would switch the instance off. */
void test_silence_with_an_active_status_leaves_enabled_unknown(void)
{
    steinel_occupancy(&s_inst[1]);
    s_inst[1].enabled_lost = true;
    DaliTransport t = transport();
    DaliInstanceSettings s;

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_restore_read_instance_settings(&t, 0u, 1u, &s));
    TEST_ASSERT_FALSE(s.has_enabled);
    TEST_ASSERT_TRUE(s.has_event_scheme);
}

void test_a_field_that_does_not_answer_is_left_unknown(void)
{
    steinel_occupancy(&s_inst[1]);
    s_inst[1].filter_silent = true;
    s_inst[1].groups[1] = 40u;    /* out of range: the set is dropped */
    DaliTransport t = transport();
    DaliInstanceSettings s;

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_restore_read_instance_settings(&t, 0u, 1u, &s));
    TEST_ASSERT_FALSE(s.has_event_filter);
    TEST_ASSERT_FALSE(s.has_instance_groups);
    TEST_ASSERT_TRUE(s.has_event_priority);
}

void test_a_failing_transport_ends_the_read(void)
{
    steinel_occupancy(&s_inst[1]);
    s_hard_error_opcode     = 0x8Bu;
    s_hard_error_opcode_err = DALI_ERR_WAIT_EXPIRED;
    DaliTransport t = transport();
    DaliInstanceSettings s;

    TEST_ASSERT_EQUAL_INT(DALI_ERR_WAIT_EXPIRED,
                          dali_restore_read_instance_settings(&t, 0u, 1u, &s));
}

/* ── What to write ────────────────────────────────────────────────────── */

static DaliInstanceSettings recorded_steinel(void)
{
    DaliInstanceSettings s;
    memset(&s, 0, sizeof(s));
    s.has_type = true;            s.type = 3u;
    s.has_enabled = true;         s.enabled = true;
    s.has_event_scheme = true;    s.event_scheme = 2u;
    s.has_event_priority = true;  s.event_priority = 4u;
    s.has_event_filter = true;    s.event_filter = 0x07u;
    s.has_instance_groups = true;
    s.instance_groups[0] = s.instance_groups[1] = s.instance_groups[2] = 0xFFu;
    return s;
}

void test_write_mask_names_only_what_differs(void)
{
    DaliInstanceSettings rec = recorded_steinel();
    DaliInstanceSettings cur = recorded_steinel();
    TEST_ASSERT_EQUAL_HEX8(0u, dali_restore_instance_write_mask(&rec, &cur));

    cur.event_scheme = 0u;           /* the 2026-10-01 state */
    TEST_ASSERT_EQUAL_HEX8(DALI_RESTORE_INSTANCE_SCHEME,
                           dali_restore_instance_write_mask(&rec, &cur));

    cur.instance_groups[0] = 5u;
    cur.enabled = false;
    TEST_ASSERT_EQUAL_HEX8(DALI_RESTORE_INSTANCE_SCHEME | DALI_RESTORE_INSTANCE_GROUPS |
                               DALI_RESTORE_INSTANCE_ENABLED,
                           dali_restore_instance_write_mask(&rec, &cur));
}

void test_write_mask_writes_what_the_device_does_not_report(void)
{
    DaliInstanceSettings rec = recorded_steinel();
    DaliInstanceSettings cur = recorded_steinel();
    cur.has_event_filter = false;
    TEST_ASSERT_EQUAL_HEX8(DALI_RESTORE_INSTANCE_FILTER,
                           dali_restore_instance_write_mask(&rec, &cur));
    TEST_ASSERT_EQUAL_HEX8(DALI_RESTORE_INSTANCE_ALL,
                           dali_restore_instance_write_mask(&rec, NULL));
}

void test_write_mask_never_writes_what_the_backup_lacks_or_cannot_send(void)
{
    DaliInstanceSettings rec = recorded_steinel();
    DaliInstanceSettings cur = recorded_steinel();
    cur.event_scheme = 0u;
    rec.has_event_scheme = false;
    TEST_ASSERT_EQUAL_HEX8(0u, dali_restore_instance_write_mask(&rec, &cur));

    rec = recorded_steinel();
    rec.event_priority = 0u;    /* SET EVENT PRIORITY takes 2-5 */
    cur.event_scheme = 2u;
    cur.event_priority = 4u;
    TEST_ASSERT_EQUAL_HEX8(0u, dali_restore_instance_write_mask(&rec, &cur));
    TEST_ASSERT_EQUAL_HEX8(0u, dali_restore_instance_write_mask(NULL, &cur));
}

/* ── Where the device is now ──────────────────────────────────────────── */

static void fill_ident(uint8_t *out, uint8_t seed)
{
    for (uint8_t i = 0u; i < DALI_MEMORY_BANK0_IDENTIFICATION_LEN; i++) {
        out[i] = (uint8_t)(0x40u + seed + i);
    }
}

static uint8_t record_device(uint8_t addr, uint8_t seed, bool with_identity)
{
    DaliSnapshotEntry e;
    memset(&e, 0, sizeof(e));
    e.space = DALI_SNAPSHOT_SPACE_DEVICE;
    e.short_address = addr;
    e.has_identification = with_identity;
    if (with_identity) {
        fill_ident(e.identification, seed);
    }
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &e));
    return (uint8_t)(s_snapshot.entry_count - 1u);
}

static void device_on_bus(uint8_t addr, uint8_t seed)
{
    DaliDiscoveryDeviceInfo *d = &s_inventory.devices[addr];
    d->present = true;
    d->has_input_device = true;
    d->has_device_identity = true;
    fill_ident(d->device_identity.serial, seed);
}

void test_locate_follows_the_identification_number(void)
{
    const uint8_t e = record_device(0u, 7u, true);
    device_on_bus(1u, 9u);
    device_on_bus(5u, 7u);    /* moved from d0 to d5 */
    uint8_t addr = 0xFFu;
    DaliRestoreConflictKind why;

    TEST_ASSERT_TRUE(dali_restore_locate_device(&s_snapshot, e, &s_inventory, &addr, &why));
    TEST_ASSERT_EQUAL_UINT8(5u, addr);
}

void test_locate_reports_why_it_cannot(void)
{
    const uint8_t gone   = record_device(0u, 7u, true);
    const uint8_t anon   = record_device(1u, 8u, false);
    const uint8_t twice  = record_device(2u, 9u, true);
    device_on_bus(10u, 9u);
    device_on_bus(11u, 9u);
    uint8_t addr = 0xFFu;
    DaliRestoreConflictKind why;

    TEST_ASSERT_FALSE(dali_restore_locate_device(&s_snapshot, gone, &s_inventory, &addr, &why));
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_CONFLICT_MISSING, why);
    TEST_ASSERT_FALSE(dali_restore_locate_device(&s_snapshot, anon, &s_inventory, &addr, &why));
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_CONFLICT_UNIDENTIFIED, why);
    TEST_ASSERT_FALSE(dali_restore_locate_device(&s_snapshot, twice, &s_inventory, &addr, &why));
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_CONFLICT_DUPLICATE_BUS, why);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, addr);
}

void test_locate_never_matches_a_contested_address_or_gear(void)
{
    const uint8_t e = record_device(0u, 7u, true);
    device_on_bus(3u, 7u);
    s_inventory.devices[3u].has_undecodable_device_activity = true;
    uint8_t addr = 0xFFu;
    DaliRestoreConflictKind why;
    TEST_ASSERT_FALSE(dali_restore_locate_device(&s_snapshot, e, &s_inventory, &addr, &why));
    TEST_ASSERT_EQUAL_INT(DALI_RESTORE_CONFLICT_MISSING, why);

    DaliSnapshotEntry gear;
    memset(&gear, 0, sizeof(gear));
    gear.space = DALI_SNAPSHOT_SPACE_GEAR;
    gear.has_identification = true;
    fill_ident(gear.identification, 7u);
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_snapshot_add(&s_snapshot, &gear));
    s_inventory.devices[3u].has_undecodable_device_activity = false;
    TEST_ASSERT_FALSE(dali_restore_locate_device(&s_snapshot,
                                                 (uint8_t)(s_snapshot.entry_count - 1u),
                                                 &s_inventory, &addr, &why));
}

/* ── Write ────────────────────────────────────────────────────────────── */

/* The whole repair on the incident's device: read, write what differs, read
 * back as recorded. */
void test_write_restores_the_steinel_and_reads_back_clean(void)
{
    steinel_occupancy(&s_inst[1]);
    s_inst[1].groups[0] = 7u;
    DaliTransport t = transport();
    DaliInstanceSettings rec = recorded_steinel();
    DaliInstanceSettings cur;

    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_restore_read_instance_settings(&t, 0u, 1u, &cur));
    const uint8_t mask = dali_restore_instance_write_mask(&rec, &cur);
    TEST_ASSERT_EQUAL_HEX8(DALI_RESTORE_INSTANCE_SCHEME | DALI_RESTORE_INSTANCE_GROUPS, mask);

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_write_instance_settings(&t, 0u, 1u, &rec, mask));
    TEST_ASSERT_EQUAL_INT(DALI_OK, dali_restore_read_instance_settings(&t, 0u, 1u, &cur));
    TEST_ASSERT_EQUAL_HEX8(0u, dali_restore_instance_write_mask(&rec, &cur));
    TEST_ASSERT_EQUAL_UINT8(2u, s_inst[1].scheme);

    /* DTR0 = 2 then SET EVENT SCHEME at d0 instance 1, sent twice. */
    TEST_ASSERT_EQUAL_HEX32(0xC13002u, s_steps[0].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0x010167u, s_steps[1].frame.data);
    TEST_ASSERT_TRUE(s_steps[1].send_twice);
}

void test_write_sends_the_filter_in_three_registers_and_enable_last(void)
{
    steinel_occupancy(&s_inst[1]);
    s_inst[1].enabled  = false;
    s_inst[1].status   = 0x00u;
    s_inst[1].filter[0] = 0u;
    DaliTransport t = transport();
    DaliInstanceSettings rec = recorded_steinel();
    rec.event_filter = 0x030201u;

    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_restore_write_instance_settings(
                              &t, 0u, 1u, &rec,
                              DALI_RESTORE_INSTANCE_FILTER | DALI_RESTORE_INSTANCE_ENABLED));
    TEST_ASSERT_EQUAL_UINT8(5u, s_step_count);
    TEST_ASSERT_EQUAL_HEX32(0xC13001u, s_steps[0].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC13102u, s_steps[1].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC13203u, s_steps[2].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0x010168u, s_steps[3].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0x010162u, s_steps[4].frame.data);
    TEST_ASSERT_TRUE(s_inst[1].enabled);
}

void test_write_refuses_a_mask_the_recording_cannot_supply(void)
{
    DaliTransport t = transport();
    DaliInstanceSettings rec = recorded_steinel();
    rec.has_event_filter = false;

    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_restore_write_instance_settings(
                              &t, 0u, 1u, &rec, DALI_RESTORE_INSTANCE_FILTER));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_restore_write_instance_settings(&t, 0u, 1u, &rec, 0x20u));
    TEST_ASSERT_EQUAL_UINT8(0u, s_step_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_read_gets_every_field);
    RUN_TEST(test_read_of_an_absent_instance_asks_nothing_more);
    RUN_TEST(test_silence_with_an_inactive_status_reads_as_disabled);
    RUN_TEST(test_silence_with_an_active_status_leaves_enabled_unknown);
    RUN_TEST(test_a_field_that_does_not_answer_is_left_unknown);
    RUN_TEST(test_a_failing_transport_ends_the_read);
    RUN_TEST(test_write_mask_names_only_what_differs);
    RUN_TEST(test_write_mask_writes_what_the_device_does_not_report);
    RUN_TEST(test_write_mask_never_writes_what_the_backup_lacks_or_cannot_send);
    RUN_TEST(test_locate_follows_the_identification_number);
    RUN_TEST(test_locate_reports_why_it_cannot);
    RUN_TEST(test_locate_never_matches_a_contested_address_or_gear);
    RUN_TEST(test_write_restores_the_steinel_and_reads_back_clean);
    RUN_TEST(test_write_sends_the_filter_in_three_registers_and_enable_last);
    RUN_TEST(test_write_refuses_a_mask_the_recording_cannot_supply);
    return UNITY_END();
}
