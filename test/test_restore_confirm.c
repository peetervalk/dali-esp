/*
 * test_restore_confirm.c — dali_restore_confirm_move(), the frames of the move
 * it confirms (dali_restore_build_move_sequence()), and the write that sends
 * them (dali_restore_write_short_address())
 *
 * `restore apply` used to report a move as done once its frames were sent.
 * SET SHORT ADDRESS is unacknowledged, so a unit that silently discarded the
 * pair still read as moved, and the plan's next move could land a second unit
 * on the address the first never vacated. These vectors pin the check the
 * executor now runs after every move, against a mock bus that answers the
 * presence probes and the Bank 0 reads in both address spaces.
 *
 * The write came later, from a 1k move that lost a frame: a unit that misses
 * the DTR0 load takes whatever DTR0 held. The mock moves units the way gear
 * and devices do, and can make the unit miss a load or a pair, so these
 * vectors can say where a unit ends up and not only which frames went out.
 */

#include "unity.h"
#include "dali_restore.h"
#include "dali_commissioning.h"

#include <string.h>

typedef struct {
    bool    present;
    bool    contested;     /* answers undecodably: two units on one address */
    bool    memory_silent; /* answers queries, but not memory reads */
    uint8_t ident[DALI_MEMORY_BANK0_IDENTIFICATION_LEN];
} MockUnit;

static MockUnit  s_gear[DALI_SHORT_ADDRESS_COUNT];
static MockUnit  s_device[DALI_SHORT_ADDRESS_COUNT];
static uint8_t   s_gear_dtr0;
static uint8_t   s_gear_dtr1;
static uint8_t   s_device_dtr0;
static uint8_t   s_device_dtr1;
/* When set, every transaction fails with it: a transport whose wait expired,
 * or a bus fault, before anything was learned. */
static DaliError s_forced_error;
/* DTR0 loads, in either space, that the unit being written does not hear. The
 * mock keeps one DTR0 per space, so a missed load leaves the old value. */
static uint8_t   s_dtr0_loads_to_miss;
/* SET SHORT ADDRESS pairs the unit does not hear whole. */
static uint8_t   s_pairs_to_miss;
/* When set, a SET SHORT ADDRESS frame fails with it: a pair the transport
 * could not finish sending. */
static DaliError s_pair_error;

/* Every frame the mock was handed, in order. */
#define MOCK_LOG_MAX 24u
typedef struct {
    DaliFrame frame;
    bool      send_twice;
} MockSent;
static MockSent  s_sent[MOCK_LOG_MAX];
static uint8_t   s_sent_count;

static void fill_identification(uint8_t *out, uint8_t seed)
{
    for (uint8_t i = 0u; i < DALI_MEMORY_BANK0_IDENTIFICATION_LEN; i++) {
        out[i] = (uint8_t)(0x40u + seed + i);
    }
}

static void unit_at(MockUnit *space, uint8_t addr, uint8_t seed)
{
    space[addr].present = true;
    fill_identification(space[addr].ident, seed);
}

static uint8_t bank0_byte(const MockUnit *unit, uint8_t offset)
{
    const uint8_t first = DALI_MEMORY_BANK0_OFFSET_IDENTIFICATION;
    if (offset >= first &&
        offset < (uint8_t)(first + DALI_MEMORY_BANK0_IDENTIFICATION_LEN)) {
        return unit->ident[offset - first];
    }
    return 0x00u;
}

static DaliError unit_reply(const MockUnit *unit, uint8_t value, DaliFrame *reply_out)
{
    if (unit->contested) {
        return DALI_ERR_RX_ACTIVITY;
    }
    if (!unit->present) {
        return DALI_ERR_TIMEOUT;
    }
    if (reply_out != NULL) {
        *reply_out = (DaliFrame){ .data = value, .bit_length = 8u };
    }
    return DALI_OK;
}

/* READ MEMORY LOCATION in either space: Bank 0 only, DTR0 auto-increments. */
static DaliError memory_reply(const MockUnit *unit, uint8_t bank, uint8_t *offset,
                              DaliFrame *reply_out)
{
    if (bank != DALI_MEMORY_BANK0 || unit->memory_silent) {
        return unit->contested ? DALI_ERR_RX_ACTIVITY : DALI_ERR_TIMEOUT;
    }
    DaliError err = unit_reply(unit, bank0_byte(unit, *offset), reply_out);
    if (err == DALI_OK) {
        (*offset)++;
    }
    return err;
}

/* A DTR0 load, unless the unit being written is due to miss it. */
static void load_dtr0(uint8_t *dtr0, uint8_t value)
{
    if (s_dtr0_loads_to_miss > 0u) {
        s_dtr0_loads_to_miss--;
        return;
    }
    *dtr0 = value;
}

/*
 * SET SHORT ADDRESS (DTR0), heard by the unit at `addr`. Gear takes
 * (a << 1) | 1 and a device the raw value; both take 0xFF as "none", and any
 * other value leaves the address alone. It is send-twice, so a lone frame does
 * nothing. A unit moved onto an occupied address leaves it contested, which
 * is what a unit that missed its DTR0 load risks.
 */
static void set_short_address(MockUnit *space, uint8_t addr, uint8_t dtr0,
                              bool gear, bool send_twice)
{
    MockUnit *unit = &space[addr];
    if (!send_twice || (!unit->present && !unit->contested)) {
        return;
    }
    if (s_pairs_to_miss > 0u) {
        s_pairs_to_miss--;
        return;
    }
    if (dtr0 == 0xFFu) {
        memset(unit, 0, sizeof(*unit));
        return;
    }

    uint8_t to;
    if (gear) {
        if ((dtr0 & 0x81u) != 0x01u) {
            return;
        }
        to = (uint8_t)(dtr0 >> 1u);
    } else {
        if (dtr0 >= DALI_SHORT_ADDRESS_COUNT) {
            return;
        }
        to = dtr0;
    }
    if (to == addr) {
        return;
    }

    if (space[to].present || space[to].contested) {
        space[to].present   = false;
        space[to].contested = true;
    } else {
        space[to] = *unit;
    }
    memset(unit, 0, sizeof(*unit));
}

static DaliError mock_transact(const DaliFrame *frame,
                               bool             needs_reply,
                               uint8_t          retries_left,
                               bool             send_twice,
                               DaliFrame       *reply_out,
                               void            *ctx)
{
    (void)needs_reply;
    (void)retries_left;
    (void)ctx;

    if (s_forced_error != DALI_OK) {
        return s_forced_error;
    }
    if (s_sent_count < MOCK_LOG_MAX) {
        s_sent[s_sent_count].frame      = *frame;
        s_sent[s_sent_count].send_twice = send_twice;
        s_sent_count++;
    }

    if (frame->bit_length == DALI_FORWARD_FRAME_BITS) {
        const uint8_t addr_byte = (uint8_t)(frame->data >> 8u);
        const uint8_t opcode    = (uint8_t)(frame->data & 0xFFu);

        if (addr_byte == 0xA3u) {          /* DTR0 DATA */
            load_dtr0(&s_gear_dtr0, opcode);
            return DALI_OK;
        }
        if (addr_byte == 0xC3u) {          /* DTR1 DATA */
            s_gear_dtr1 = opcode;
            return DALI_OK;
        }
        if ((addr_byte & 0x01u) != 0u && addr_byte <= 0x7Fu) {
            const uint8_t   addr = (uint8_t)(addr_byte >> 1u);
            const MockUnit *unit = &s_gear[addr];
            if (opcode == 0x90u) {         /* QUERY STATUS */
                return unit_reply(unit, 0x04u, reply_out);
            }
            if (opcode == 0x98u) {         /* QUERY CONTENT DTR0 */
                return unit_reply(unit, s_gear_dtr0, reply_out);
            }
            if (opcode == 0xC5u) {         /* READ MEMORY LOCATION */
                return memory_reply(unit, s_gear_dtr1, &s_gear_dtr0, reply_out);
            }
            if (opcode == 0x80u) {         /* SET SHORT ADDRESS (DTR0) */
                if (s_pair_error != DALI_OK) {
                    return s_pair_error;
                }
                set_short_address(s_gear, addr, s_gear_dtr0, true, send_twice);
                return DALI_OK;
            }
        }
        return DALI_ERR_INVALID;
    }

    if (frame->bit_length == DALI_EXTENDED_FRAME_BITS) {
        const uint8_t addr_byte = (uint8_t)(frame->data >> 16u);
        const uint8_t middle    = (uint8_t)((frame->data >> 8u) & 0xFFu);
        const uint8_t opcode    = (uint8_t)(frame->data & 0xFFu);

        if (addr_byte == 0xC1u && middle == 0x30u) {   /* device DTR0 */
            load_dtr0(&s_device_dtr0, opcode);
            return DALI_OK;
        }
        if (addr_byte == 0xC1u && middle == 0x31u) {   /* device DTR1 */
            s_device_dtr1 = opcode;
            return DALI_OK;
        }
        if ((addr_byte & 0x01u) != 0u && addr_byte <= 0x7Fu &&
            middle == DALI_DEVICE_INSTANCE) {
            const uint8_t   addr = (uint8_t)(addr_byte >> 1u);
            const MockUnit *unit = &s_device[addr];
            if (opcode == 0x35u) {         /* QUERY NUMBER OF INSTANCES */
                return unit_reply(unit, 4u, reply_out);
            }
            if (opcode == 0x36u) {         /* QUERY CONTENT DTR0 (device) */
                return unit_reply(unit, s_device_dtr0, reply_out);
            }
            if (opcode == 0x3Cu) {         /* READ MEMORY LOCATION (device) */
                return memory_reply(unit, s_device_dtr1, &s_device_dtr0, reply_out);
            }
            if (opcode == 0x14u) {         /* SET SHORT ADDRESS (device) */
                set_short_address(s_device, addr, s_device_dtr0, false, send_twice);
                return DALI_OK;
            }
        }
        return DALI_ERR_INVALID;
    }

    return DALI_ERR_INVALID;
}

/* Runs the steps the way the scheduler does: in order, stopping at the first
 * failure, keeping one reply per reply-bearing step. */
static DaliError mock_transact_sequence(const DaliSequence *seq,
                                        DaliSequenceResult *result_out,
                                        void               *ctx)
{
    DaliSequenceResult result;
    memset(&result, 0, sizeof(result));
    result.result      = DALI_OK;
    result.failed_step = DALI_SEQUENCE_NO_FAILED_STEP;

    for (uint8_t i = 0u; i < seq->step_count; i++) {
        const DaliSequenceStep *step = &seq->steps[i];
        DaliFrame reply = {0u, 0u};
        DaliError err = mock_transact(&step->frame, step->needs_reply,
                                      step->retries_left, step->send_twice,
                                      step->needs_reply ? &reply : NULL, ctx);
        result.steps_run = (uint8_t)(i + 1u);
        if (err != DALI_OK) {
            result.result      = err;
            result.failed_step = i;
            break;
        }
        if (step->needs_reply) {
            result.replies[i] = reply;
            result.reply_mask |= (uint8_t)(1u << i);
        }
    }

    if (result_out != NULL) {
        *result_out = result;
    }
    return result.result;
}

static const DaliTransport s_transport = {
    .transact          = mock_transact,
    .transact_sequence = mock_transact_sequence,
};

static DaliRestoreMove move_of(DaliSnapshotSpace space, uint8_t from, uint8_t to,
                               uint8_t seed)
{
    DaliRestoreMove move;
    memset(&move, 0, sizeof(move));
    move.space = space;
    move.from  = from;
    move.to    = to;
    move.kind  = DALI_RESTORE_MOVE_PLACE;
    move.has_identification = true;
    fill_identification(move.identification, seed);
    return move;
}

static DaliRestoreMoveCheck check_move(const DaliRestoreMove *move,
                                       DaliError             *probe_err)
{
    DaliRestoreMoveCheck check = DALI_RESTORE_MOVE_CONFIRMED;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_restore_confirm_move(&s_transport, move, &check,
                                                probe_err));
    return check;
}

void setUp(void)
{
    memset(s_gear, 0, sizeof(s_gear));
    memset(s_device, 0, sizeof(s_device));
    s_gear_dtr0 = 0u;
    s_gear_dtr1 = 0u;
    s_device_dtr0 = 0u;
    s_device_dtr1 = 0u;
    s_forced_error = DALI_OK;
    s_dtr0_loads_to_miss = 0u;
    s_pairs_to_miss = 0u;
    s_pair_error = DALI_OK;
    memset(s_sent, 0, sizeof(s_sent));
    s_sent_count = 0u;
}

void tearDown(void) {}

/* --------------------------------------------------------------------------
 * Control gear
 * -------------------------------------------------------------------------*/

void test_a_move_that_landed_is_confirmed(void)
{
    unit_at(s_gear, 8u, 5u);     /* the unit, now at its destination */
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);

    DaliError probe_err = DALI_ERR_INVALID;
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_CONFIRMED, check_move(&move, &probe_err));
    TEST_ASSERT_EQUAL(DALI_OK, probe_err);
}

/* The pair was discarded: nothing at the destination, the unit still at its
 * source. */
void test_silence_at_the_target_is_reported(void)
{
    unit_at(s_gear, 5u, 5u);
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_TARGET_SILENT, check_move(&move, NULL));
}

/*
 * Silent at both ends: the unit took an address nobody chose, or lost power.
 * Calling that "did not move" would send the operator back to planning with a
 * unit loose on the bus.
 */
void test_silence_at_both_ends_is_a_missing_unit(void)
{
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_UNIT_MISSING, check_move(&move, NULL));
}

/* A silent target and an undecodable source prove neither reading. */
void test_a_silent_target_with_an_unreadable_source_is_unreadable(void)
{
    s_gear[5].contested = true;
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);

    DaliError probe_err = DALI_OK;
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_UNREADABLE, check_move(&move, &probe_err));
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY, probe_err);
}

/*
 * The case that made this necessary: the unit never left, so whatever answers
 * at the destination is something else, and the next move in the plan would
 * have been written on top of the unit that stayed.
 */
void test_a_unit_still_at_the_source_is_reported(void)
{
    unit_at(s_gear, 5u, 5u);     /* did not move */
    unit_at(s_gear, 8u, 5u);
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_SOURCE_ANSWERS, check_move(&move, NULL));
}

/* Something answers at the target, but not the unit that was sent there. */
void test_a_different_unit_at_the_target_is_reported(void)
{
    unit_at(s_gear, 8u, 99u);
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_WRONG_UNIT, check_move(&move, NULL));
}

/* Two units now answer at the target together. Unreadable, never "absent". */
void test_a_collision_at_the_target_is_unreadable(void)
{
    s_gear[8].contested = true;
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);

    DaliError probe_err = DALI_OK;
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_UNREADABLE, check_move(&move, &probe_err));
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY, probe_err);
}

/*
 * A transport that stopped waiting has learned nothing. Reading that as a
 * silent target would be the failure the distinct error exists to prevent.
 */
void test_an_expired_wait_is_unreadable_not_silence(void)
{
    s_forced_error = DALI_ERR_WAIT_EXPIRED;
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);

    DaliError probe_err = DALI_OK;
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_UNREADABLE, check_move(&move, &probe_err));
    TEST_ASSERT_EQUAL(DALI_ERR_WAIT_EXPIRED, probe_err);
}

/* It answers, but its identity cannot be read: not confirmed either way. */
void test_an_unreadable_identity_is_not_a_confirmation(void)
{
    unit_at(s_gear, 8u, 5u);
    s_gear[8].memory_silent = true;
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);

    DaliError probe_err = DALI_OK;
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_UNREADABLE, check_move(&move, &probe_err));
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, probe_err);
}

/* A move with no identity to compare is confirmed on presence alone. The
 * planner never emits one, but the check must not invent a comparison. */
void test_a_move_without_an_identity_is_confirmed_on_presence(void)
{
    unit_at(s_gear, 8u, 77u);
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);
    move.has_identification = false;
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_CONFIRMED, check_move(&move, NULL));
}

/* --------------------------------------------------------------------------
 * Control devices, and the independence of the two spaces
 * -------------------------------------------------------------------------*/

void test_a_device_move_is_confirmed_in_the_device_space(void)
{
    unit_at(s_device, 4u, 30u);
    /* Gear at the device's old number is a different unit in another space. */
    unit_at(s_gear, 2u, 31u);
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_DEVICE, 2u, 4u, 30u);

    DaliError probe_err = DALI_ERR_INVALID;
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_CONFIRMED, check_move(&move, &probe_err));
    TEST_ASSERT_EQUAL(DALI_OK, probe_err);
}

void test_a_device_still_at_its_source_is_reported(void)
{
    unit_at(s_device, 2u, 30u);
    unit_at(s_device, 4u, 30u);
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_DEVICE, 2u, 4u, 30u);
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_SOURCE_ANSWERS, check_move(&move, NULL));
}

/* The device identity is read from the device's own Bank 0, never the gear's. */
void test_a_device_is_identified_from_its_own_bank0(void)
{
    unit_at(s_device, 4u, 30u);
    unit_at(s_gear, 4u, 31u);    /* a hybrid: its gear half reports another number */
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_DEVICE, 2u, 4u, 31u);
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_WRONG_UNIT, check_move(&move, NULL));
}

/* --------------------------------------------------------------------------
 * Sending a move: dali_restore_build_move_sequence()
 *
 * The frames `restore apply` and `address d<N>` send. Both once loaded a
 * device's DTR0 with the gear encoding, which the host suites could not see
 * because nothing pinned the frames; the 2k bus saw it, as a device sent to
 * d2 that landed on d5.
 * -------------------------------------------------------------------------*/

static void assert_move_frames(DaliSnapshotSpace space, uint8_t from, uint8_t to,
                               uint32_t dtr0_frame, uint32_t command_frame,
                               uint8_t bits)
{
    DaliSequence seq;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_restore_build_move_sequence(space, from, to, &seq));
    TEST_ASSERT_EQUAL_UINT8(2u, seq.step_count);

    TEST_ASSERT_EQUAL_HEX32(dtr0_frame, seq.steps[0].frame.data);
    TEST_ASSERT_EQUAL_UINT8(bits, seq.steps[0].frame.bit_length);
    TEST_ASSERT_FALSE(seq.steps[0].send_twice);

    TEST_ASSERT_EQUAL_HEX32(command_frame, seq.steps[1].frame.data);
    TEST_ASSERT_EQUAL_UINT8(bits, seq.steps[1].frame.bit_length);
    TEST_ASSERT_TRUE(seq.steps[1].send_twice);
}

/* DTR0 = 2, raw, then SET SHORT ADDRESS to d5. The old frames loaded 5. */
void test_a_device_move_loads_the_raw_destination(void)
{
    assert_move_frames(DALI_SNAPSHOT_SPACE_DEVICE, 5u, 2u,
                       0xC13002u, 0x0BFE14u, DALI_EXTENDED_FRAME_BITS);
    assert_move_frames(DALI_SNAPSHOT_SPACE_DEVICE, 62u, 0u,
                       0xC13000u, 0x7DFE14u, DALI_EXTENDED_FRAME_BITS);
    assert_move_frames(DALI_SNAPSHOT_SPACE_DEVICE, 0u, 63u,
                       0xC1303Fu, 0x01FE14u, DALI_EXTENDED_FRAME_BITS);
}

/* Gear keeps (a << 1) | 1: a4 -> a1 loads 3, and a2 -> a13 loads 27. */
void test_a_gear_move_loads_the_encoded_destination(void)
{
    assert_move_frames(DALI_SNAPSHOT_SPACE_GEAR, 4u, 1u,
                       0xA303u, 0x0980u, DALI_FORWARD_FRAME_BITS);
    assert_move_frames(DALI_SNAPSHOT_SPACE_GEAR, 2u, 13u,
                       0xA31Bu, 0x0580u, DALI_FORWARD_FRAME_BITS);
}

/* "No address" is 0xFF in both spaces and is never encoded. */
void test_a_clear_loads_ff_in_both_spaces(void)
{
    assert_move_frames(DALI_SNAPSHOT_SPACE_DEVICE, 0u,
                       DALI_COMMISSIONING_NO_SHORT_ADDRESS,
                       0xC130FFu, 0x01FE14u, DALI_EXTENDED_FRAME_BITS);
    assert_move_frames(DALI_SNAPSHOT_SPACE_GEAR, 3u,
                       DALI_COMMISSIONING_NO_SHORT_ADDRESS,
                       0xA3FFu, 0x0780u, DALI_FORWARD_FRAME_BITS);
}

void test_a_move_sequence_rejects_what_no_address_can_be(void)
{
    DaliSequence seq;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_build_move_sequence(DALI_SNAPSHOT_SPACE_DEVICE,
                                                       0u, 2u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_build_move_sequence(DALI_SNAPSHOT_SPACE_DEVICE,
                                                       DALI_SHORT_ADDRESS_COUNT,
                                                       2u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_build_move_sequence(DALI_SNAPSHOT_SPACE_GEAR,
                                                       0u, DALI_SHORT_ADDRESS_COUNT,
                                                       &seq));
    /* 0x7F is the INITIALISE selector for "unaddressed", not an address. */
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_build_move_sequence(DALI_SNAPSHOT_SPACE_DEVICE,
                                                       0u, 0x7Fu, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_build_move_sequence((DaliSnapshotSpace)7,
                                                       0u, 2u, &seq));
}

/* --------------------------------------------------------------------------
 * Writing a move: dali_restore_write_short_address()
 *
 * The 1k bus sent DTR0 and the pair for a5 -> a2, and the unit stayed at a5.
 * Every unit but the one the previous confirmation read held 0x0B, a5 encoded,
 * so a lost load there was harmless only because the unit was leaving a5.
 * From anywhere else the same loss lands it on a5.
 * -------------------------------------------------------------------------*/

static DaliRestoreWriteResult write_move(DaliSnapshotSpace space, uint8_t from,
                                         uint8_t to, bool allow_collided)
{
    DaliRestoreWriteResult result;
    memset(&result, 0xA5, sizeof(result));   /* every field must be written */
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_restore_write_short_address(&s_transport, space, from,
                                                       to, allow_collided,
                                                       &result));
    return result;
}

static void assert_sent(uint8_t index, uint32_t data, uint8_t bits, bool twice)
{
    TEST_ASSERT_TRUE(index < s_sent_count);
    TEST_ASSERT_EQUAL_HEX32(data, s_sent[index].frame.data);
    TEST_ASSERT_EQUAL_UINT8(bits, s_sent[index].frame.bit_length);
    TEST_ASSERT_EQUAL(twice, s_sent[index].send_twice);
}

/* Whether any SET SHORT ADDRESS went out, in either space. */
static bool pair_was_sent(void)
{
    for (uint8_t i = 0u; i < s_sent_count; i++) {
        const uint32_t d = s_sent[i].frame.data;
        if (s_sent[i].frame.bit_length == DALI_FORWARD_FRAME_BITS &&
            (d >> 8u) <= 0x7Fu && (d & 0x100u) != 0u && (d & 0xFFu) == 0x80u) {
            return true;
        }
        if (s_sent[i].frame.bit_length == DALI_EXTENDED_FRAME_BITS &&
            (d & 0xFFFFu) == 0xFE14u) {
            return true;
        }
    }
    return false;
}

/* DTR0, the read-back at the unit, then the pair; and the move confirms. */
void test_a_write_reads_dtr0_back_before_the_pair(void)
{
    unit_at(s_gear, 5u, 5u);
    DaliRestoreWriteResult result =
        write_move(DALI_SNAPSHOT_SPACE_GEAR, 5u, 2u, false);

    TEST_ASSERT_EQUAL(DALI_RESTORE_WRITE_SENT, result.outcome);
    TEST_ASSERT_EQUAL_HEX8(0x05u, result.dtr0);
    TEST_ASSERT_EQUAL_UINT8(1u, result.loads);
    TEST_ASSERT_EQUAL_UINT8(3u, s_sent_count);
    assert_sent(0u, 0xA305u, DALI_FORWARD_FRAME_BITS, false);  /* DTR0 = a2    */
    assert_sent(1u, 0x0B98u, DALI_FORWARD_FRAME_BITS, false);  /* read it, a5  */
    assert_sent(2u, 0x0B80u, DALI_FORWARD_FRAME_BITS, true);   /* the pair, a5 */

    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 2u, 5u);
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_CONFIRMED, check_move(&move, NULL));
}

/*
 * Why the write exists, on the builder's bare frames: the unit at a7 misses the
 * load and hears the pair while DTR0 still holds the 0x0B a Bank 0 read left.
 * It lands on a5, on top of the unit already there.
 */
void test_bare_frames_follow_a_stale_dtr0(void)
{
    unit_at(s_gear, 7u, 7u);
    unit_at(s_gear, 5u, 5u);
    s_gear_dtr0 = DALI_MEMORY_BANK0_OFFSET_IDENTIFICATION;
    s_dtr0_loads_to_miss = 1u;

    DaliSequence seq;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_restore_build_move_sequence(DALI_SNAPSHOT_SPACE_GEAR,
                                                       7u, 2u, &seq));
    TEST_ASSERT_EQUAL(DALI_OK, mock_transact_sequence(&seq, NULL, NULL));

    TEST_ASSERT_FALSE(s_gear[7].present);
    TEST_ASSERT_FALSE(s_gear[2].present);
    TEST_ASSERT_TRUE(s_gear[5].contested);
}

/*
 * The same bus through the write: the stale value reads back, DTR0 is loaded
 * again, and the unit goes where it was sent. The first reading is kept as the
 * evidence of what the unit held.
 */
void test_a_missed_load_is_loaded_again(void)
{
    unit_at(s_gear, 7u, 7u);
    unit_at(s_gear, 5u, 5u);
    s_gear_dtr0 = DALI_MEMORY_BANK0_OFFSET_IDENTIFICATION;
    s_dtr0_loads_to_miss = 1u;

    DaliRestoreWriteResult result =
        write_move(DALI_SNAPSHOT_SPACE_GEAR, 7u, 2u, false);

    TEST_ASSERT_EQUAL(DALI_RESTORE_WRITE_SENT, result.outcome);
    TEST_ASSERT_EQUAL_UINT8(2u, result.loads);
    TEST_ASSERT_EQUAL(DALI_OK, result.first_error);
    TEST_ASSERT_EQUAL_HEX8(0x0Bu, result.first_read);
    TEST_ASSERT_EQUAL(DALI_OK, result.last_error);
    TEST_ASSERT_EQUAL_HEX8(0x05u, result.last_read);
    TEST_ASSERT_TRUE(s_gear[2].present);
    TEST_ASSERT_TRUE(s_gear[5].present);
    TEST_ASSERT_FALSE(s_gear[5].contested);
}

/* A load that never lands sends no pair, and every unit stays put. */
void test_a_load_that_never_lands_sends_no_pair(void)
{
    unit_at(s_gear, 7u, 7u);
    unit_at(s_gear, 5u, 5u);
    s_gear_dtr0 = DALI_MEMORY_BANK0_OFFSET_IDENTIFICATION;
    s_dtr0_loads_to_miss = 2u;

    DaliRestoreWriteResult result =
        write_move(DALI_SNAPSHOT_SPACE_GEAR, 7u, 2u, false);

    TEST_ASSERT_EQUAL(DALI_RESTORE_WRITE_DTR0_MISMATCH, result.outcome);
    TEST_ASSERT_EQUAL_UINT8(2u, result.loads);
    TEST_ASSERT_EQUAL(DALI_OK, result.last_error);
    TEST_ASSERT_EQUAL_HEX8(0x0Bu, result.last_read);
    TEST_ASSERT_FALSE(pair_was_sent());
    TEST_ASSERT_TRUE(s_gear[7].present);
    TEST_ASSERT_TRUE(s_gear[5].present);
    TEST_ASSERT_FALSE(s_gear[5].contested);
}

/* With DTR0 right, a lost pair can only do nothing, and the confirmation
 * finds the unit where it was. */
void test_a_lost_pair_leaves_the_unit_where_it_was(void)
{
    unit_at(s_gear, 7u, 7u);
    s_pairs_to_miss = 1u;

    DaliRestoreWriteResult result =
        write_move(DALI_SNAPSHOT_SPACE_GEAR, 7u, 2u, false);
    TEST_ASSERT_EQUAL(DALI_RESTORE_WRITE_SENT, result.outcome);
    TEST_ASSERT_TRUE(s_gear[7].present);

    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 7u, 2u, 7u);
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_TARGET_SILENT, check_move(&move, NULL));
}

/* Nothing answers the read-back: nothing to write to, and nothing written. */
void test_a_silent_readback_sends_no_pair(void)
{
    DaliRestoreWriteResult result =
        write_move(DALI_SNAPSHOT_SPACE_GEAR, 7u, 2u, false);

    TEST_ASSERT_EQUAL(DALI_RESTORE_WRITE_DTR0_SILENT, result.outcome);
    TEST_ASSERT_EQUAL_UINT8(2u, result.loads);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, result.last_error);
    TEST_ASSERT_FALSE(pair_was_sent());
}

/*
 * Units sharing an address answer the read-back together. A move refuses that,
 * since nothing could say what either holds; the clear that exists to separate
 * them may go ahead without it.
 */
void test_a_collided_readback_is_refused_unless_allowed(void)
{
    s_gear[4].contested = true;

    DaliRestoreWriteResult result =
        write_move(DALI_SNAPSHOT_SPACE_GEAR, 4u, 9u, false);
    TEST_ASSERT_EQUAL(DALI_RESTORE_WRITE_DTR0_UNREADABLE, result.outcome);
    TEST_ASSERT_EQUAL_UINT8(2u, result.loads);
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY, result.last_error);
    TEST_ASSERT_FALSE(pair_was_sent());
    TEST_ASSERT_TRUE(s_gear[4].contested);

    result = write_move(DALI_SNAPSHOT_SPACE_GEAR, 4u,
                        DALI_COMMISSIONING_NO_SHORT_ADDRESS, true);
    TEST_ASSERT_EQUAL(DALI_RESTORE_WRITE_SENT_UNVERIFIED, result.outcome);
    TEST_ASSERT_EQUAL_UINT8(1u, result.loads);
    TEST_ASSERT_TRUE(pair_was_sent());
    TEST_ASSERT_FALSE(s_gear[4].contested);
}

/* A clear reads back 0xFF, the one DTR0 value that is not an encoded address. */
void test_a_clear_reads_back_ff(void)
{
    unit_at(s_gear, 3u, 3u);
    DaliRestoreWriteResult result =
        write_move(DALI_SNAPSHOT_SPACE_GEAR, 3u,
                   DALI_COMMISSIONING_NO_SHORT_ADDRESS, false);

    TEST_ASSERT_EQUAL(DALI_RESTORE_WRITE_SENT, result.outcome);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, result.dtr0);
    assert_sent(0u, 0xA3FFu, DALI_FORWARD_FRAME_BITS, false);
    assert_sent(1u, 0x0798u, DALI_FORWARD_FRAME_BITS, false);
    assert_sent(2u, 0x0780u, DALI_FORWARD_FRAME_BITS, true);
    TEST_ASSERT_FALSE(s_gear[3].present);
}

/* A device reads back its own DTR0, raw, over its own 24-bit query. */
void test_a_device_write_reads_its_own_dtr0_back(void)
{
    unit_at(s_device, 5u, 30u);
    DaliRestoreWriteResult result =
        write_move(DALI_SNAPSHOT_SPACE_DEVICE, 5u, 2u, false);

    TEST_ASSERT_EQUAL(DALI_RESTORE_WRITE_SENT, result.outcome);
    TEST_ASSERT_EQUAL_HEX8(0x02u, result.dtr0);
    TEST_ASSERT_EQUAL_UINT8(3u, s_sent_count);
    assert_sent(0u, 0xC13002u, DALI_EXTENDED_FRAME_BITS, false);
    assert_sent(1u, 0x0BFE36u, DALI_EXTENDED_FRAME_BITS, false);
    assert_sent(2u, 0x0BFE14u, DALI_EXTENDED_FRAME_BITS, true);
    TEST_ASSERT_TRUE(s_device[2].present);
    TEST_ASSERT_FALSE(s_device[5].present);
}

/* The device form of the hazard: its Bank 0 read leaves 0x0B too, which a
 * device reads raw, as d11. */
void test_a_device_that_misses_its_load_is_loaded_again(void)
{
    unit_at(s_device, 5u, 30u);
    s_device_dtr0 = DALI_MEMORY_BANK0_OFFSET_IDENTIFICATION;
    s_dtr0_loads_to_miss = 1u;

    DaliRestoreWriteResult result =
        write_move(DALI_SNAPSHOT_SPACE_DEVICE, 5u, 2u, false);

    TEST_ASSERT_EQUAL(DALI_RESTORE_WRITE_SENT, result.outcome);
    TEST_ASSERT_EQUAL_UINT8(2u, result.loads);
    TEST_ASSERT_EQUAL_HEX8(0x0Bu, result.first_read);
    TEST_ASSERT_TRUE(s_device[2].present);
    TEST_ASSERT_FALSE(s_device[11].present);
}

/* A load that could not go out has learned nothing and sent no pair; the
 * transport's error is passed on rather than read as a verdict. */
void test_a_transport_failure_is_passed_on(void)
{
    unit_at(s_gear, 5u, 5u);
    s_forced_error = DALI_ERR_WAIT_EXPIRED;

    DaliRestoreWriteResult result;
    TEST_ASSERT_EQUAL(DALI_ERR_WAIT_EXPIRED,
                      dali_restore_write_short_address(&s_transport,
                                                       DALI_SNAPSHOT_SPACE_GEAR,
                                                       5u, 2u, false, &result));
    TEST_ASSERT_EQUAL_UINT8(0u, result.loads);
    TEST_ASSERT_FALSE(result.pair_attempted);
    TEST_ASSERT_TRUE(s_gear[5].present);
}

/* A pair that failed on its way out may have moved the unit, and the result
 * says so rather than leaving it to look like the load failing. */
void test_a_failed_pair_says_it_was_attempted(void)
{
    unit_at(s_gear, 5u, 5u);
    s_pair_error = DALI_ERR_BUS_STUCK;

    DaliRestoreWriteResult result;
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK,
                      dali_restore_write_short_address(&s_transport,
                                                       DALI_SNAPSHOT_SPACE_GEAR,
                                                       5u, 2u, false, &result));
    TEST_ASSERT_EQUAL_UINT8(1u, result.loads);
    TEST_ASSERT_TRUE(result.pair_attempted);
}

void test_a_write_rejects_what_it_cannot_run(void)
{
    DaliRestoreWriteResult result;
    const DaliTransport frames_only = { .transact = mock_transact };

    memset(&result, 0xA5, sizeof(result));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_write_short_address(&frames_only,
                                                       DALI_SNAPSHOT_SPACE_GEAR,
                                                       5u, 2u, false, &result));
    TEST_ASSERT_EQUAL_UINT8(0u, result.loads);   /* written even when refused */
    TEST_ASSERT_FALSE(result.pair_attempted);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_write_short_address(NULL,
                                                       DALI_SNAPSHOT_SPACE_GEAR,
                                                       5u, 2u, false, &result));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_write_short_address(&s_transport,
                                                       DALI_SNAPSHOT_SPACE_GEAR,
                                                       5u, 2u, false, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_write_short_address(&s_transport,
                                                       DALI_SNAPSHOT_SPACE_GEAR,
                                                       DALI_SHORT_ADDRESS_COUNT,
                                                       2u, false, &result));
    TEST_ASSERT_EQUAL_UINT8(0u, s_sent_count);
}

/* --------------------------------------------------------------------------
 * Arguments and names
 * -------------------------------------------------------------------------*/

void test_invalid_arguments_are_rejected(void)
{
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);
    DaliRestoreMoveCheck check;
    const DaliTransport empty = {0};

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_confirm_move(NULL, &move, &check, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_confirm_move(&empty, &move, &check, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_confirm_move(&s_transport, NULL, &check, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_confirm_move(&s_transport, &move, NULL, NULL));

    move.to = DALI_SHORT_ADDRESS_COUNT;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_restore_confirm_move(&s_transport, &move, &check, NULL));
}

void test_every_verdict_has_a_name(void)
{
    for (int check = DALI_RESTORE_MOVE_CONFIRMED;
         check <= DALI_RESTORE_MOVE_UNIT_MISSING;
         check++) {
        const char *name = dali_restore_move_check_name((DaliRestoreMoveCheck)check);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strcmp(name, "unknown") != 0);
    }
}

void test_every_write_outcome_has_a_name(void)
{
    for (int outcome = DALI_RESTORE_WRITE_SENT;
         outcome <= DALI_RESTORE_WRITE_DTR0_UNREADABLE;
         outcome++) {
        const char *name =
            dali_restore_write_outcome_name((DaliRestoreWriteOutcome)outcome);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strcmp(name, "unknown") != 0);
    }
    TEST_ASSERT_TRUE(dali_restore_write_was_sent(DALI_RESTORE_WRITE_SENT));
    TEST_ASSERT_TRUE(dali_restore_write_was_sent(DALI_RESTORE_WRITE_SENT_UNVERIFIED));
    TEST_ASSERT_FALSE(dali_restore_write_was_sent(DALI_RESTORE_WRITE_DTR0_MISMATCH));
    TEST_ASSERT_FALSE(dali_restore_write_was_sent(DALI_RESTORE_WRITE_DTR0_SILENT));
    TEST_ASSERT_FALSE(dali_restore_write_was_sent(DALI_RESTORE_WRITE_DTR0_UNREADABLE));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_move_that_landed_is_confirmed);
    RUN_TEST(test_silence_at_the_target_is_reported);
    RUN_TEST(test_silence_at_both_ends_is_a_missing_unit);
    RUN_TEST(test_a_silent_target_with_an_unreadable_source_is_unreadable);
    RUN_TEST(test_a_unit_still_at_the_source_is_reported);
    RUN_TEST(test_a_different_unit_at_the_target_is_reported);
    RUN_TEST(test_a_collision_at_the_target_is_unreadable);
    RUN_TEST(test_an_expired_wait_is_unreadable_not_silence);
    RUN_TEST(test_an_unreadable_identity_is_not_a_confirmation);
    RUN_TEST(test_a_move_without_an_identity_is_confirmed_on_presence);
    RUN_TEST(test_a_device_move_is_confirmed_in_the_device_space);
    RUN_TEST(test_a_device_still_at_its_source_is_reported);
    RUN_TEST(test_a_device_is_identified_from_its_own_bank0);
    RUN_TEST(test_a_device_move_loads_the_raw_destination);
    RUN_TEST(test_a_gear_move_loads_the_encoded_destination);
    RUN_TEST(test_a_clear_loads_ff_in_both_spaces);
    RUN_TEST(test_a_move_sequence_rejects_what_no_address_can_be);
    RUN_TEST(test_a_write_reads_dtr0_back_before_the_pair);
    RUN_TEST(test_bare_frames_follow_a_stale_dtr0);
    RUN_TEST(test_a_missed_load_is_loaded_again);
    RUN_TEST(test_a_load_that_never_lands_sends_no_pair);
    RUN_TEST(test_a_lost_pair_leaves_the_unit_where_it_was);
    RUN_TEST(test_a_silent_readback_sends_no_pair);
    RUN_TEST(test_a_collided_readback_is_refused_unless_allowed);
    RUN_TEST(test_a_clear_reads_back_ff);
    RUN_TEST(test_a_device_write_reads_its_own_dtr0_back);
    RUN_TEST(test_a_device_that_misses_its_load_is_loaded_again);
    RUN_TEST(test_a_transport_failure_is_passed_on);
    RUN_TEST(test_a_failed_pair_says_it_was_attempted);
    RUN_TEST(test_a_write_rejects_what_it_cannot_run);
    RUN_TEST(test_invalid_arguments_are_rejected);
    RUN_TEST(test_every_verdict_has_a_name);
    RUN_TEST(test_every_write_outcome_has_a_name);
    return UNITY_END();
}
