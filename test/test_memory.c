/*
 * test_memory.c — unit tests for dali_memory (IEC 62386-102 §9.10)
 */

#include "unity.h"
#include "dali_memory.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Mock transport
 * --------------------------------------------------------------------------*/

#define MAX_FRAMES 32u

static DaliFrame s_frames[MAX_FRAMES];
static bool      s_needs_reply[MAX_FRAMES];
static uint8_t   s_frame_count;

static uint8_t   s_replies[MAX_FRAMES];
static uint8_t   s_reply_count;
static uint8_t   s_reply_index;

static DaliError s_transact_err;

static DaliError mock_transact(const DaliFrame *frame,
                               bool             needs_reply,
                               uint8_t          retries_left,
                               bool             send_twice,
                               DaliFrame       *reply_out,
                               void            *ctx)
{
    (void)retries_left;
    (void)send_twice;
    (void)ctx;

    if (s_frame_count < MAX_FRAMES) {
        s_frames[s_frame_count]       = *frame;
        s_needs_reply[s_frame_count]  = needs_reply;
        s_frame_count++;
    }

    if (s_transact_err != DALI_OK) {
        return s_transact_err;
    }

    if (needs_reply && reply_out != NULL) {
        if (s_reply_index < s_reply_count) {
            reply_out->data       = s_replies[s_reply_index++];
            reply_out->bit_length = DALI_BACKWARD_FRAME_BITS;
        } else {
            return DALI_ERR_TIMEOUT;
        }
    }
    return DALI_OK;
}

static DaliError mock_transact_sequence(const DaliSequence *seq,
                                        DaliSequenceResult *result_out,
                                        void *ctx)
{
    if (seq == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliSequenceResult result = {
        .result = DALI_OK,
        .failed_step = DALI_SEQUENCE_NO_FAILED_STEP,
    };
    for (uint8_t i = 0u; i < seq->step_count; i++) {
        const DaliSequenceStep *step = &seq->steps[i];
        DaliFrame reply = {0u, 0u};
        DaliError err = mock_transact(&step->frame,
                                      step->needs_reply,
                                      step->retries_left,
                                      step->send_twice,
                                      step->needs_reply ? &reply : NULL,
                                      ctx);
        result.steps_run = (uint8_t)(i + 1u);
        if (err != DALI_OK) {
            result.result = err;
            result.failed_step = i;
            if (result_out != NULL) {
                *result_out = result;
            }
            return err;
        }
        if (step->needs_reply) {
            result.replies[i] = reply;
            result.reply_mask |= (uint8_t)(1u << i);
        }
    }

    if (result_out != NULL) {
        *result_out = result;
    }
    return DALI_OK;
}

static DaliMemoryTransport s_transport = {
    .transact = mock_transact,
    .transact_sequence = mock_transact_sequence,
    .ctx      = NULL,
};

static void push_reply(uint8_t byte)
{
    if (s_reply_count < MAX_FRAMES) {
        s_replies[s_reply_count++] = byte;
    }
}

void setUp(void)
{
    memset(s_frames,      0, sizeof(s_frames));
    memset(s_needs_reply, 0, sizeof(s_needs_reply));
    s_frame_count   = 0u;
    memset(s_replies, 0, sizeof(s_replies));
    s_reply_count   = 0u;
    s_reply_index   = 0u;
    s_transact_err  = DALI_OK;
}

void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Frame builders
 * --------------------------------------------------------------------------*/

void test_build_dtr1_bank_is_16bit(void)
{
    DaliFrame f = dali_memory_build_dtr1_bank(0u);
    TEST_ASSERT_EQUAL_UINT8(16u, f.bit_length);
}

void test_build_dtr1_bank_matches_dtr1_data(void)
{
    /* dali_memory_build_dtr1_bank is a thin wrapper — verify identical output */
    for (uint8_t v = 0u; v < 8u; v++) {
        DaliFrame got      = dali_memory_build_dtr1_bank(v);
        DaliFrame expected = dali_cmd_dtr1_data(v);
        TEST_ASSERT_EQUAL_UINT32(expected.data,       got.data);
        TEST_ASSERT_EQUAL_UINT8 (expected.bit_length, got.bit_length);
    }
}

void test_build_dtr0_offset_matches_dtr0_data(void)
{
    for (uint8_t v = 0u; v < 8u; v++) {
        DaliFrame got      = dali_memory_build_dtr0_offset(v);
        DaliFrame expected = dali_cmd_dtr0_data(v);
        TEST_ASSERT_EQUAL_UINT32(expected.data,       got.data);
        TEST_ASSERT_EQUAL_UINT8 (expected.bit_length, got.bit_length);
    }
}

void test_build_read_is_16bit(void)
{
    DaliFrame f = dali_memory_build_read(0u);
    TEST_ASSERT_EQUAL_UINT8(16u, f.bit_length);
}

void test_build_read_matches_build_command(void)
{
    for (uint8_t addr = 0u; addr < 4u; addr++) {
        DaliFrame got      = dali_memory_build_read(addr);
        DaliFrame expected = {0u, 0u};
        dali_build_command(DALI_ADDR_SHORT, addr,
                           DALI_CMD_READ_MEMORY_LOCATION, 0u, &expected);
        TEST_ASSERT_EQUAL_UINT32(expected.data,       got.data);
        TEST_ASSERT_EQUAL_UINT8 (expected.bit_length, got.bit_length);
    }
}

void test_build_read_accepts_max_short_address(void)
{
    DaliFrame frame = dali_memory_build_read(63u);
    TEST_ASSERT_EQUAL_UINT8(DALI_FORWARD_FRAME_BITS, frame.bit_length);
    TEST_ASSERT_EQUAL_HEX32(0x7FC5u, frame.data);
}

void test_build_read_rejects_invalid_short_addresses(void)
{
    DaliFrame frame = dali_memory_build_read(64u);
    TEST_ASSERT_EQUAL_UINT8(0u, frame.bit_length);
    TEST_ASSERT_EQUAL_UINT32(0u, frame.data);

    frame = dali_memory_build_read(255u);
    TEST_ASSERT_EQUAL_UINT8(0u, frame.bit_length);
    TEST_ASSERT_EQUAL_UINT32(0u, frame.data);
}

void test_build_control_device_write_sequence_matches_standard_frames(void)
{
    static const uint32_t expected_data[7] = {
        0xC13102u, 0xC13002u, 0x0BFE15u, 0xC12155u,
        0xC13004u, 0x0BFE15u, 0xC121FFu,
    };
    static const bool expected_send_twice[7] = {
        false, false, true, false, false, true, false,
    };
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_memory_build_control_device_write_sequence(
                          5u, 2u, 4u, 0xFFu, &seq));
    TEST_ASSERT_EQUAL_UINT8(7u, seq.step_count);
    TEST_ASSERT_NULL(seq.on_complete);
    TEST_ASSERT_NULL(seq.cb_ctx);
    for (uint8_t i = 0u; i < 7u; i++) {
        TEST_ASSERT_EQUAL_HEX32(expected_data[i], seq.steps[i].frame.data);
        TEST_ASSERT_EQUAL_UINT8(24u, seq.steps[i].frame.bit_length);
        TEST_ASSERT_FALSE(seq.steps[i].needs_reply);
        TEST_ASSERT_EQUAL(expected_send_twice[i], seq.steps[i].send_twice);
        TEST_ASSERT_EQUAL_UINT8(0u, seq.steps[i].retries_left);
    }
}

void test_build_control_device_write_sequence_accepts_boundary_values(void)
{
    DaliSequence seq;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_memory_build_control_device_write_sequence(
                          63u, 255u, 255u, 255u, &seq));
    TEST_ASSERT_EQUAL_HEX32(0x7FFE15u, seq.steps[2].frame.data);
}

void test_build_control_device_write_sequence_rejects_invalid_inputs(void)
{
    DaliSequence seq;
    memset(&seq, 0xA5, sizeof(seq));

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_build_control_device_write_sequence(
                          64u, 1u, 3u, 0u, &seq));
    TEST_ASSERT_EQUAL_HEX8(0xA5u, ((const uint8_t *)&seq)[0]);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_build_control_device_write_sequence(
                          0u, 0u, 3u, 0u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_build_control_device_write_sequence(
                          0u, 1u, 3u, 0u, NULL));
}

/* ---------------------------------------------------------------------------
 * Read sequence builders
 *
 * Independent frame vectors, address 5 / bank 2 / offset 0x10:
 *   control gear   DTR1 = 0xC3 0x02, DTR0 = 0xA3 0x10, READ = 0x0B 0xC5
 *   control device DTR1 = 0xC1 0x31 0x02, DTR0 = 0xC1 0x30 0x10,
 *                  READ = 0x0B 0xFE 0x3C
 * --------------------------------------------------------------------------*/

static DaliSequenceResult make_read_result(const uint8_t *bytes, uint8_t count)
{
    DaliSequenceResult result = {
        .result      = DALI_OK,
        .failed_step = DALI_SEQUENCE_NO_FAILED_STEP,
        .steps_run   = (uint8_t)(DALI_MEMORY_READ_SETUP_STEPS + count),
    };

    for (uint8_t i = 0u; i < count; i++) {
        uint8_t step = (uint8_t)(DALI_MEMORY_READ_SETUP_STEPS + i);
        result.replies[step] = (DaliFrame){
            .data       = bytes[i],
            .bit_length = DALI_BACKWARD_FRAME_BITS,
        };
        result.reply_mask |= (uint8_t)(1u << step);
    }
    return result;
}

void test_build_read_sequence_matches_control_gear_frames(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_memory_build_read_sequence(5u, 2u, 0x10u, 1u, &seq));
    TEST_ASSERT_EQUAL_UINT8(3u, seq.step_count);
    TEST_ASSERT_NULL(seq.on_complete);

    TEST_ASSERT_EQUAL_HEX32(0xC302u, seq.steps[0].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xA310u, seq.steps[1].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0x0BC5u, seq.steps[2].frame.data);
    for (uint8_t i = 0u; i < 3u; i++) {
        TEST_ASSERT_EQUAL_UINT8(DALI_FORWARD_FRAME_BITS, seq.steps[i].frame.bit_length);
        TEST_ASSERT_FALSE(seq.steps[i].send_twice);
    }
    TEST_ASSERT_FALSE(seq.steps[0].needs_reply);
    TEST_ASSERT_FALSE(seq.steps[1].needs_reply);
    TEST_ASSERT_TRUE(seq.steps[2].needs_reply);
    /* A repeated READ would land on the next location — never auto-retry. */
    TEST_ASSERT_EQUAL_UINT8(0u, seq.steps[2].retries_left);
}

void test_build_read_sequence_repeats_read_for_a_block(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_memory_build_read_sequence(
                          5u, 2u, 0x10u, DALI_MEMORY_MAX_SEQUENCE_READ_BYTES, &seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_MAX_STEPS, seq.step_count);

    /* One DTR1/DTR0 setup, then the same READ frame per byte. */
    for (uint8_t i = DALI_MEMORY_READ_SETUP_STEPS; i < seq.step_count; i++) {
        TEST_ASSERT_EQUAL_HEX32(0x0BC5u, seq.steps[i].frame.data);
        TEST_ASSERT_TRUE(seq.steps[i].needs_reply);
    }
}

void test_build_control_device_read_sequence_matches_standard_frames(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_memory_build_control_device_read_sequence(
                          5u, 2u, 0x10u, 1u, &seq));
    TEST_ASSERT_EQUAL_UINT8(3u, seq.step_count);

    TEST_ASSERT_EQUAL_HEX32(0xC13102u, seq.steps[0].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC13010u, seq.steps[1].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0x0BFE3Cu, seq.steps[2].frame.data);
    for (uint8_t i = 0u; i < 3u; i++) {
        TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, seq.steps[i].frame.bit_length);
    }
    TEST_ASSERT_TRUE(seq.steps[2].needs_reply);
    TEST_ASSERT_EQUAL_UINT8(0u, seq.steps[2].retries_left);
}

void test_build_read_sequences_reject_invalid_inputs(void)
{
    DaliSequence seq;
    memset(&seq, 0xA5, sizeof(seq));

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_build_read_sequence(64u, 0u, 0u, 1u, &seq));
    TEST_ASSERT_EQUAL_HEX8(0xA5u, ((const uint8_t *)&seq)[0]);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_build_read_sequence(5u, 0u, 0u, 0u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_build_read_sequence(
                          5u, 0u, 0u, DALI_MEMORY_MAX_SEQUENCE_READ_BYTES + 1u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_build_read_sequence(5u, 0u, 0u, 1u, NULL));

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_build_control_device_read_sequence(64u, 0u, 0u, 1u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_build_control_device_read_sequence(5u, 0u, 0u, 0u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_build_control_device_read_sequence(5u, 0u, 0u, 1u, NULL));
}

void test_read_from_sequence_collects_reply_bytes(void)
{
    const uint8_t bytes[3] = { 0xDEu, 0xADu, 0xBEu };
    DaliSequenceResult result = make_read_result(bytes, 3u);
    uint8_t buf[3] = {0u, 0u, 0u};

    TEST_ASSERT_EQUAL(DALI_OK, dali_memory_read_from_sequence(&result, 3u, buf));
    TEST_ASSERT_EQUAL_HEX8(0xDEu, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xADu, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBEu, buf[2]);

    /* Reading fewer bytes than the sequence returned stops at the request. */
    uint8_t one = 0u;
    TEST_ASSERT_EQUAL(DALI_OK, dali_memory_read_from_sequence(&result, 1u, &one));
    TEST_ASSERT_EQUAL_HEX8(0xDEu, one);
}

void test_read_from_sequence_rejects_partial_and_failed_reads(void)
{
    const uint8_t bytes[3] = { 0xDEu, 0xADu, 0xBEu };
    uint8_t buf[3] = {0u, 0u, 0u};

    /* Only the first read replied — never hand back a half-filled buffer. */
    DaliSequenceResult partial = make_read_result(bytes, 1u);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_memory_read_from_sequence(&partial, 2u, buf));

    DaliSequenceResult failed = make_read_result(bytes, 1u);
    failed.result      = DALI_ERR_TIMEOUT;
    failed.failed_step = 3u;
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT,
                      dali_memory_read_from_sequence(&failed, 2u, buf));

    /* A forward frame in a reply slot is not a backward reply. */
    DaliSequenceResult wrong_width = make_read_result(bytes, 1u);
    wrong_width.replies[DALI_MEMORY_READ_SETUP_STEPS].bit_length =
        DALI_FORWARD_FRAME_BITS;
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_memory_read_from_sequence(&wrong_width, 1u, buf));

    DaliSequenceResult ok = make_read_result(bytes, 1u);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_read_from_sequence(NULL, 1u, buf));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_read_from_sequence(&ok, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_read_from_sequence(&ok, 0u, buf));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_read_from_sequence(
                          &ok, DALI_MEMORY_MAX_SEQUENCE_READ_BYTES + 1u, buf));
}

/* ---------------------------------------------------------------------------
 * dali_memory_read_byte — transaction sequence
 * --------------------------------------------------------------------------*/

void test_read_byte_sends_three_frames(void)
{
    push_reply(0xABu);
    uint8_t out = 0u;
    DaliError err = dali_memory_read_byte(&s_transport, 3u, 0u, 0x02u, &out);

    TEST_ASSERT_EQUAL_INT(DALI_OK, err);
    TEST_ASSERT_EQUAL_UINT8(3u, s_frame_count);
}

void test_read_byte_first_two_frames_no_reply(void)
{
    push_reply(0x00u);
    uint8_t out = 0u;
    dali_memory_read_byte(&s_transport, 0u, 0u, 0x00u, &out);

    TEST_ASSERT_FALSE(s_needs_reply[0]);
    TEST_ASSERT_FALSE(s_needs_reply[1]);
}

void test_read_byte_third_frame_needs_reply(void)
{
    push_reply(0x00u);
    uint8_t out = 0u;
    dali_memory_read_byte(&s_transport, 0u, 0u, 0x00u, &out);

    TEST_ASSERT_TRUE(s_needs_reply[2]);
}

void test_read_byte_correct_frame_sequence(void)
{
    push_reply(0x42u);
    uint8_t out   = 0u;
    uint8_t addr  = 5u;
    uint8_t bank  = 2u;
    uint8_t offs  = 0x0Au;
    dali_memory_read_byte(&s_transport, addr, bank, offs, &out);

    DaliFrame exp_dtr1 = dali_memory_build_dtr1_bank(bank);
    DaliFrame exp_dtr0 = dali_memory_build_dtr0_offset(offs);
    DaliFrame exp_read = dali_memory_build_read(addr);

    TEST_ASSERT_EQUAL_UINT32(exp_dtr1.data, s_frames[0].data);
    TEST_ASSERT_EQUAL_UINT32(exp_dtr0.data, s_frames[1].data);
    TEST_ASSERT_EQUAL_UINT32(exp_read.data, s_frames[2].data);
}

void test_read_byte_returns_reply_value(void)
{
    push_reply(0xBEu);
    uint8_t out = 0u;
    DaliError err = dali_memory_read_byte(&s_transport, 0u, 0u, 0u, &out);

    TEST_ASSERT_EQUAL_INT(DALI_OK, err);
    TEST_ASSERT_EQUAL_UINT8(0xBEu, out);
}

void test_read_byte_timeout_propagates(void)
{
    /* no reply programmed — mock returns TIMEOUT */
    uint8_t out = 0u;
    DaliError err = dali_memory_read_byte(&s_transport, 0u, 0u, 0u, &out);
    TEST_ASSERT_EQUAL_INT(DALI_ERR_TIMEOUT, err);
}

void test_read_byte_null_out_returns_invalid(void)
{
    DaliError err = dali_memory_read_byte(&s_transport, 0u, 0u, 0u, NULL);
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, err);
}

void test_read_byte_null_transport_returns_invalid(void)
{
    uint8_t out = 0u;
    DaliError err = dali_memory_read_byte(NULL, 0u, 0u, 0u, &out);
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, err);
}

void test_read_byte_rejects_invalid_short_addresses_without_traffic(void)
{
    uint8_t out = 0xA5u;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_memory_read_byte(&s_transport, 64u, 0u, 0u, &out));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_memory_read_byte(&s_transport, 255u, 0u, 0u, &out));
    TEST_ASSERT_EQUAL_UINT8(0u, s_frame_count);
    TEST_ASSERT_EQUAL_UINT8(0xA5u, out);
}

/* ---------------------------------------------------------------------------
 * dali_memory_read_bytes — block read with auto-increment
 * --------------------------------------------------------------------------*/

void test_read_bytes_sends_dtr_once_then_n_reads(void)
{
    /* 3 bytes: DTR1 + DTR0 + 3 READs = 5 frames */
    push_reply(0x01u);
    push_reply(0x02u);
    push_reply(0x03u);

    uint8_t buf[3] = {0u};
    DaliError err = dali_memory_read_bytes(&s_transport, 1u, 0u, 0x02u, buf, 3u);

    TEST_ASSERT_EQUAL_INT(DALI_OK, err);
    TEST_ASSERT_EQUAL_UINT8(5u, s_frame_count);
    TEST_ASSERT_FALSE(s_needs_reply[0]);   /* DTR1 */
    TEST_ASSERT_FALSE(s_needs_reply[1]);   /* DTR0 */
    TEST_ASSERT_TRUE (s_needs_reply[2]);   /* READ 0 */
    TEST_ASSERT_TRUE (s_needs_reply[3]);   /* READ 1 */
    TEST_ASSERT_TRUE (s_needs_reply[4]);   /* READ 2 */
}

void test_read_bytes_same_read_frame_reused(void)
{
    /* All READ frames must address the same device — DTR0 auto-increments internally */
    push_reply(0xAAu);
    push_reply(0xBBu);
    push_reply(0xCCu);

    uint8_t buf[3] = {0u};
    uint8_t addr = 7u;
    dali_memory_read_bytes(&s_transport, addr, 0u, 0u, buf, 3u);

    DaliFrame exp_read = dali_memory_build_read(addr);
    TEST_ASSERT_EQUAL_UINT32(exp_read.data, s_frames[2].data);
    TEST_ASSERT_EQUAL_UINT32(exp_read.data, s_frames[3].data);
    TEST_ASSERT_EQUAL_UINT32(exp_read.data, s_frames[4].data);
}

void test_read_bytes_values_returned_in_order(void)
{
    push_reply(0x10u);
    push_reply(0x20u);
    push_reply(0x30u);

    uint8_t buf[3] = {0u};
    dali_memory_read_bytes(&s_transport, 0u, 0u, 0u, buf, 3u);

    TEST_ASSERT_EQUAL_UINT8(0x10u, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x20u, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x30u, buf[2]);
}

void test_read_bytes_count_zero_sends_no_frames(void)
{
    uint8_t buf[4] = {0u};
    DaliError err = dali_memory_read_bytes(&s_transport, 0u, 0u, 0u, buf, 0u);

    TEST_ASSERT_EQUAL_INT(DALI_OK, err);
    TEST_ASSERT_EQUAL_UINT8(0u, s_frame_count);
}

void test_read_bytes_mid_sequence_error_propagates(void)
{
    push_reply(0xAAu);
    /* second reply absent — will timeout */

    uint8_t buf[3] = {0u};
    DaliError err = dali_memory_read_bytes(&s_transport, 0u, 0u, 0u, buf, 3u);

    TEST_ASSERT_EQUAL_INT(DALI_ERR_TIMEOUT, err);
}

void test_read_bytes_null_buf_returns_invalid(void)
{
    DaliError err = dali_memory_read_bytes(&s_transport, 0u, 0u, 0u, NULL, 4u);
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, err);
}

void test_read_bytes_rejects_invalid_short_addresses_without_traffic(void)
{
    uint8_t buf[2] = {0xA5u, 0x5Au};
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_memory_read_bytes(&s_transport, 64u, 0u, 0u,
                                                 buf, 2u));
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_memory_read_bytes(&s_transport, 255u, 0u, 0u,
                                                 buf, 0u));
    TEST_ASSERT_EQUAL_UINT8(0u, s_frame_count);
    TEST_ASSERT_EQUAL_HEX8(0xA5u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x5Au, buf[1]);
}

void test_read_bytes_rejects_frame_only_transport_without_traffic(void)
{
    DaliMemoryTransport frame_only = {
        .transact = mock_transact,
        .ctx = NULL,
    };
    uint8_t value = 0xA5u;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_memory_read_bytes(&frame_only,
                                             0u,
                                             0u,
                                             0u,
                                             &value,
                                             1u));
    TEST_ASSERT_EQUAL_UINT8(0u, s_frame_count);
    TEST_ASSERT_EQUAL_HEX8(0xA5u, value);
}

/* ---------------------------------------------------------------------------
 * dali_memory_read_bank0_identity
 * --------------------------------------------------------------------------*/

/* Standard-derived bytes at Bank 0 locations 0x03..0x14. Keep this literal
 * independent of the production offset constants. */
static const uint8_t s_bank0_identity_vector[18] = {
    0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, /* GTIN, 0x03..0x08 */
    0x02u, 0x05u,                               /* firmware, 0x09..0x0A */
    0x10u, 0x32u, 0x54u, 0x76u, 0x98u, 0xBAu, 0xDCu, 0xFEu,
                                                    /* identification, 0x0B..0x12 */
    0x03u, 0x07u,                               /* hardware, 0x13..0x14 */
};

static void push_bank0_identity_vector(void)
{
    for (uint8_t i = 0u; i < (uint8_t)sizeof(s_bank0_identity_vector); i++) {
        push_reply(s_bank0_identity_vector[i]);
    }
}

void test_bank0_identity_layout_constants_match_standard_locations(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x00u, DALI_MEMORY_BANK0_OFFSET_LAST_ADDR);
    TEST_ASSERT_EQUAL_HEX8(0x02u, DALI_MEMORY_BANK0_OFFSET_LAST_BANK);
    TEST_ASSERT_EQUAL_HEX8(0x03u, DALI_MEMORY_BANK0_OFFSET_GTIN);
    TEST_ASSERT_EQUAL_HEX8(0x09u, DALI_MEMORY_BANK0_OFFSET_FW_MAJOR);
    TEST_ASSERT_EQUAL_HEX8(0x0Au, DALI_MEMORY_BANK0_OFFSET_FW_MINOR);
    TEST_ASSERT_EQUAL_HEX8(0x0Bu, DALI_MEMORY_BANK0_OFFSET_IDENTIFICATION);
    TEST_ASSERT_EQUAL_HEX8(0x13u, DALI_MEMORY_BANK0_OFFSET_HW_MAJOR);
    TEST_ASSERT_EQUAL_HEX8(0x14u, DALI_MEMORY_BANK0_OFFSET_HW_MINOR);
    TEST_ASSERT_EQUAL_UINT8(18u, DALI_MEMORY_BANK0_IDENTITY_SIZE);
}

void test_bank0_identity_parses_standard_vector(void)
{
    const uint8_t expected_gtin[6] = {
        0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu,
    };
    const uint8_t expected_identification[8] = {
        0x10u, 0x32u, 0x54u, 0x76u, 0x98u, 0xBAu, 0xDCu, 0xFEu,
    };
    push_bank0_identity_vector();

    DaliMemoryBank0Identity id;
    memset(&id, 0, sizeof(id));
    DaliError err = dali_memory_read_bank0_identity(&s_transport, 3u, &id);

    TEST_ASSERT_EQUAL_INT(DALI_OK, err);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_gtin, id.gtin, 6u);
    TEST_ASSERT_EQUAL_UINT8(2u, id.fw_major);
    TEST_ASSERT_EQUAL_UINT8(5u, id.fw_minor);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_identification, id.serial, 8u);
    TEST_ASSERT_EQUAL_UINT8(3u, id.hw_major);
    TEST_ASSERT_EQUAL_UINT8(7u, id.hw_minor);
}

void test_bank0_identity_starts_at_0x03_and_skips_reserved_0x01(void)
{
    push_bank0_identity_vector();

    DaliMemoryBank0Identity id;
    dali_memory_read_bank0_identity(&s_transport, 0u, &id);

    TEST_ASSERT_EQUAL_HEX32(0xC300u, s_frames[0].data); /* DTR1 = Bank 0 */
    TEST_ASSERT_EQUAL_HEX32(0xA303u, s_frames[1].data); /* DTR0 = 0x03 */
    TEST_ASSERT_FALSE(s_needs_reply[0]);
    TEST_ASSERT_FALSE(s_needs_reply[1]);
}

void test_bank0_identity_transport_error_propagates(void)
{
    s_transact_err = DALI_ERR_TIMEOUT;

    DaliMemoryBank0Identity id;
    DaliError err = dali_memory_read_bank0_identity(&s_transport, 0u, &id);

    TEST_ASSERT_EQUAL_INT(DALI_ERR_TIMEOUT, err);
}

void test_bank0_identity_null_out_returns_invalid(void)
{
    DaliError err = dali_memory_read_bank0_identity(&s_transport, 0u, NULL);
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, err);
}

void test_bank0_identity_rejects_invalid_short_address_without_traffic(void)
{
    DaliMemoryBank0Identity id;
    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID,
                          dali_memory_read_bank0_identity(&s_transport, 64u, &id));
    TEST_ASSERT_EQUAL_UINT8(0u, s_frame_count);
}

void test_bank0_identity_total_frame_count(void)
{
    /* 18 bytes exceed one sequence, so the read is split into chunks of
     * DALI_MEMORY_MAX_SEQUENCE_READ_BYTES that each re-issue DTR1 + DTR0:
     * ceil(18/5) = 4 chunks → 4*2 setup frames + 18 READs = 26 frames. The
     * extra setup is what lets a chunk boundary re-establish the offset. */
    push_bank0_identity_vector();

    DaliMemoryBank0Identity id;
    TEST_ASSERT_EQUAL_INT(DALI_OK,
                          dali_memory_read_bank0_identity(&s_transport, 0u, &id));

    uint8_t chunks = (uint8_t)((DALI_MEMORY_BANK0_IDENTITY_SIZE +
                                DALI_MEMORY_MAX_SEQUENCE_READ_BYTES - 1u) /
                               DALI_MEMORY_MAX_SEQUENCE_READ_BYTES);
    TEST_ASSERT_EQUAL_UINT8(
        (uint8_t)(chunks * DALI_MEMORY_READ_SETUP_STEPS +
                  DALI_MEMORY_BANK0_IDENTITY_SIZE),
        s_frame_count);
}

/* ---------------------------------------------------------------------------
 * Runner
 * --------------------------------------------------------------------------*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_build_dtr1_bank_is_16bit);
    RUN_TEST(test_build_dtr1_bank_matches_dtr1_data);
    RUN_TEST(test_build_dtr0_offset_matches_dtr0_data);
    RUN_TEST(test_build_read_is_16bit);
    RUN_TEST(test_build_read_matches_build_command);
    RUN_TEST(test_build_read_accepts_max_short_address);
    RUN_TEST(test_build_read_rejects_invalid_short_addresses);
    RUN_TEST(test_build_control_device_write_sequence_matches_standard_frames);
    RUN_TEST(test_build_control_device_write_sequence_accepts_boundary_values);
    RUN_TEST(test_build_control_device_write_sequence_rejects_invalid_inputs);

    RUN_TEST(test_build_read_sequence_matches_control_gear_frames);
    RUN_TEST(test_build_read_sequence_repeats_read_for_a_block);
    RUN_TEST(test_build_control_device_read_sequence_matches_standard_frames);
    RUN_TEST(test_build_read_sequences_reject_invalid_inputs);
    RUN_TEST(test_read_from_sequence_collects_reply_bytes);
    RUN_TEST(test_read_from_sequence_rejects_partial_and_failed_reads);

    RUN_TEST(test_read_byte_sends_three_frames);
    RUN_TEST(test_read_byte_first_two_frames_no_reply);
    RUN_TEST(test_read_byte_third_frame_needs_reply);
    RUN_TEST(test_read_byte_correct_frame_sequence);
    RUN_TEST(test_read_byte_returns_reply_value);
    RUN_TEST(test_read_byte_timeout_propagates);
    RUN_TEST(test_read_byte_null_out_returns_invalid);
    RUN_TEST(test_read_byte_null_transport_returns_invalid);
    RUN_TEST(test_read_byte_rejects_invalid_short_addresses_without_traffic);

    RUN_TEST(test_read_bytes_sends_dtr_once_then_n_reads);
    RUN_TEST(test_read_bytes_same_read_frame_reused);
    RUN_TEST(test_read_bytes_values_returned_in_order);
    RUN_TEST(test_read_bytes_count_zero_sends_no_frames);
    RUN_TEST(test_read_bytes_mid_sequence_error_propagates);
    RUN_TEST(test_read_bytes_null_buf_returns_invalid);
    RUN_TEST(test_read_bytes_rejects_invalid_short_addresses_without_traffic);
    RUN_TEST(test_read_bytes_rejects_frame_only_transport_without_traffic);

    RUN_TEST(test_bank0_identity_layout_constants_match_standard_locations);
    RUN_TEST(test_bank0_identity_parses_standard_vector);
    RUN_TEST(test_bank0_identity_starts_at_0x03_and_skips_reserved_0x01);
    RUN_TEST(test_bank0_identity_transport_error_propagates);
    RUN_TEST(test_bank0_identity_null_out_returns_invalid);
    RUN_TEST(test_bank0_identity_rejects_invalid_short_address_without_traffic);
    RUN_TEST(test_bank0_identity_total_frame_count);

    return UNITY_END();
}
