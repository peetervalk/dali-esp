/*
 * test_restore_confirm.c — dali_restore_confirm_move()
 *
 * `restore apply` used to report a move as done once its frames were sent.
 * SET SHORT ADDRESS is unacknowledged, so a unit that silently discarded the
 * pair still read as moved, and the plan's next move could land a second unit
 * on the address the first never vacated. These vectors pin the check the
 * executor now runs after every move, against a mock bus that answers the
 * presence probes and the Bank 0 reads in both address spaces.
 */

#include "unity.h"
#include "dali_restore.h"

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

static DaliError mock_transact(const DaliFrame *frame,
                               bool             needs_reply,
                               uint8_t          retries_left,
                               bool             send_twice,
                               DaliFrame       *reply_out,
                               void            *ctx)
{
    (void)needs_reply;
    (void)retries_left;
    (void)send_twice;
    (void)ctx;

    if (s_forced_error != DALI_OK) {
        return s_forced_error;
    }

    if (frame->bit_length == DALI_FORWARD_FRAME_BITS) {
        const uint8_t addr_byte = (uint8_t)(frame->data >> 8u);
        const uint8_t opcode    = (uint8_t)(frame->data & 0xFFu);

        if (addr_byte == 0xA3u) {          /* DTR0 DATA */
            s_gear_dtr0 = opcode;
            return DALI_OK;
        }
        if (addr_byte == 0xC3u) {          /* DTR1 DATA */
            s_gear_dtr1 = opcode;
            return DALI_OK;
        }
        if ((addr_byte & 0x01u) != 0u && addr_byte <= 0x7Fu) {
            const MockUnit *unit = &s_gear[addr_byte >> 1u];
            if (opcode == 0x90u) {         /* QUERY STATUS */
                return unit_reply(unit, 0x04u, reply_out);
            }
            if (opcode == 0xC5u) {         /* READ MEMORY LOCATION */
                return memory_reply(unit, s_gear_dtr1, &s_gear_dtr0, reply_out);
            }
        }
        return DALI_ERR_INVALID;
    }

    if (frame->bit_length == DALI_EXTENDED_FRAME_BITS) {
        const uint8_t addr_byte = (uint8_t)(frame->data >> 16u);
        const uint8_t middle    = (uint8_t)((frame->data >> 8u) & 0xFFu);
        const uint8_t opcode    = (uint8_t)(frame->data & 0xFFu);

        if (addr_byte == 0xC1u && middle == 0x30u) {   /* device DTR0 */
            s_device_dtr0 = opcode;
            return DALI_OK;
        }
        if (addr_byte == 0xC1u && middle == 0x31u) {   /* device DTR1 */
            s_device_dtr1 = opcode;
            return DALI_OK;
        }
        if ((addr_byte & 0x01u) != 0u && addr_byte <= 0x7Fu &&
            middle == DALI_DEVICE_INSTANCE) {
            const MockUnit *unit = &s_device[addr_byte >> 1u];
            if (opcode == 0x35u) {         /* QUERY NUMBER OF INSTANCES */
                return unit_reply(unit, 4u, reply_out);
            }
            if (opcode == 0x3Cu) {         /* READ MEMORY LOCATION (device) */
                return memory_reply(unit, s_device_dtr1, &s_device_dtr0, reply_out);
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

/* The pair was discarded: nothing at the destination. */
void test_silence_at_the_target_is_reported(void)
{
    DaliRestoreMove move = move_of(DALI_SNAPSHOT_SPACE_GEAR, 5u, 8u, 5u);
    TEST_ASSERT_EQUAL(DALI_RESTORE_MOVE_TARGET_SILENT, check_move(&move, NULL));
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
         check <= DALI_RESTORE_MOVE_UNREADABLE;
         check++) {
        const char *name = dali_restore_move_check_name((DaliRestoreMoveCheck)check);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(strcmp(name, "unknown") != 0);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_move_that_landed_is_confirmed);
    RUN_TEST(test_silence_at_the_target_is_reported);
    RUN_TEST(test_a_unit_still_at_the_source_is_reported);
    RUN_TEST(test_a_different_unit_at_the_target_is_reported);
    RUN_TEST(test_a_collision_at_the_target_is_unreadable);
    RUN_TEST(test_an_expired_wait_is_unreadable_not_silence);
    RUN_TEST(test_an_unreadable_identity_is_not_a_confirmation);
    RUN_TEST(test_a_move_without_an_identity_is_confirmed_on_presence);
    RUN_TEST(test_a_device_move_is_confirmed_in_the_device_space);
    RUN_TEST(test_a_device_still_at_its_source_is_reported);
    RUN_TEST(test_a_device_is_identified_from_its_own_bank0);
    RUN_TEST(test_invalid_arguments_are_rejected);
    RUN_TEST(test_every_verdict_has_a_name);
    return UNITY_END();
}
