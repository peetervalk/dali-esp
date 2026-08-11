#include "unity.h"

#include "dali_input_poll.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    uint8_t frame_calls;
    uint8_t sequence_calls;
    uint8_t replies[4];
    DaliSequence sequence;
    DaliError fail_error;
    uint8_t fail_step;
} MockPollTransport;

static DaliError mock_transact(const DaliFrame *frame,
                               bool needs_reply,
                               uint8_t retries_left,
                               bool send_twice,
                               DaliFrame *reply_out,
                               void *ctx)
{
    MockPollTransport *mock = (MockPollTransport *)ctx;
    (void)frame;
    (void)needs_reply;
    (void)retries_left;
    (void)send_twice;
    (void)reply_out;
    TEST_ASSERT_NOT_NULL(mock);
    mock->frame_calls++;
    return DALI_ERR_INVALID;
}

static DaliError mock_transact_sequence(const DaliSequence *seq,
                                        DaliSequenceResult *result_out,
                                        void *ctx)
{
    MockPollTransport *mock = (MockPollTransport *)ctx;
    TEST_ASSERT_NOT_NULL(mock);
    TEST_ASSERT_NOT_NULL(seq);
    TEST_ASSERT_NOT_NULL(result_out);
    TEST_ASSERT_TRUE(seq->step_count <= DALI_INPUT_POLL_MAX_BYTES);

    mock->sequence_calls++;
    mock->sequence = *seq;
    memset(result_out, 0, sizeof(*result_out));
    result_out->result = DALI_OK;
    result_out->failed_step = DALI_SEQUENCE_NO_FAILED_STEP;

    for (uint8_t i = 0u; i < seq->step_count; i++) {
        TEST_ASSERT_TRUE(seq->steps[i].needs_reply);
        TEST_ASSERT_FALSE(seq->steps[i].send_twice);
        TEST_ASSERT_EQUAL_UINT8(0u, seq->steps[i].retries_left);
        result_out->steps_run = (uint8_t)(i + 1u);

        if (mock->fail_error != DALI_OK && i == mock->fail_step) {
            result_out->result = mock->fail_error;
            result_out->failed_step = i;
            return mock->fail_error;
        }

        result_out->replies[i] = (DaliFrame){
            .data = mock->replies[i],
            .bit_length = DALI_BACKWARD_FRAME_BITS,
        };
        result_out->reply_mask |= (uint8_t)(1u << i);
    }
    return DALI_OK;
}

void test_bytes_for_resolution_clamps_to_one_to_four(void)
{
    TEST_ASSERT_EQUAL_UINT8(1u, dali_input_poll_bytes_for_resolution(0u));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_input_poll_bytes_for_resolution(1u));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_input_poll_bytes_for_resolution(8u));
    TEST_ASSERT_EQUAL_UINT8(2u, dali_input_poll_bytes_for_resolution(9u));
    TEST_ASSERT_EQUAL_UINT8(2u, dali_input_poll_bytes_for_resolution(16u));
    TEST_ASSERT_EQUAL_UINT8(4u, dali_input_poll_bytes_for_resolution(32u));
    TEST_ASSERT_EQUAL_UINT8(4u, dali_input_poll_bytes_for_resolution(64u));
}

void test_poll_value_reads_msb_then_latch_bytes(void)
{
    MockPollTransport mock = {
        .replies = { 0x12u, 0x34u },
    };
    DaliDiscoveryTransport transport = {
        .transact = mock_transact,
        .transact_sequence = mock_transact_sequence,
        .ctx = &mock,
    };
    DaliInputPollResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_poll_value(&transport, 5u, 2u, 2u, &result));

    TEST_ASSERT_EQUAL_UINT8(0u, mock.frame_calls);
    TEST_ASSERT_EQUAL_UINT8(1u, mock.sequence_calls);
    TEST_ASSERT_EQUAL_UINT8(2u, mock.sequence.step_count);
    TEST_ASSERT_EQUAL_HEX32(0x0B028Cu, mock.sequence.steps[0].frame.data);
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS,
                            mock.sequence.steps[0].frame.bit_length);
    TEST_ASSERT_EQUAL_HEX32(0x0B028Du, mock.sequence.steps[1].frame.data);
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS,
                            mock.sequence.steps[1].frame.bit_length);
    TEST_ASSERT_EQUAL_UINT8(5u, result.address);
    TEST_ASSERT_EQUAL_UINT8(2u, result.instance);
    TEST_ASSERT_EQUAL_UINT8(2u, result.requested_bytes);
    TEST_ASSERT_TRUE(result.value.complete);
    TEST_ASSERT_EQUAL_UINT8(2u, result.value.byte_count);
    TEST_ASSERT_EQUAL_HEX32(0x1234u, result.value.value);
    TEST_ASSERT_EQUAL(DALI_OK, result.byte_errors[0]);
    TEST_ASSERT_EQUAL(DALI_OK, result.byte_errors[1]);
}

void test_poll_value_rejects_invalid_args(void)
{
    MockPollTransport mock = {0};
    DaliDiscoveryTransport transport = {
        .transact = mock_transact,
        .transact_sequence = mock_transact_sequence,
        .ctx = &mock,
    };
    DaliDiscoveryTransport frame_only = {
        .transact = mock_transact,
        .ctx = &mock,
    };
    DaliInputPollResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value(NULL, 5u, 0u, 1u, &result));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value(&transport, 64u, 0u, 1u, &result));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value(&transport, 5u, 32u, 1u, &result));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value(&transport, 5u, 0u, 0u, &result));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value(&transport, 5u, 0u, 5u, &result));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value(&transport, 5u, 0u, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value(&frame_only, 5u, 0u, 1u, &result));
    TEST_ASSERT_EQUAL_UINT8(0u, mock.frame_calls);
    TEST_ASSERT_EQUAL_UINT8(0u, mock.sequence_calls);
}

void test_poll_value_reports_atomic_step_failure_without_partial_value(void)
{
    MockPollTransport mock = {
        .replies = { 0x12u, 0x34u, 0x56u },
        .fail_error = DALI_ERR_TIMEOUT,
        .fail_step = 1u,
    };
    DaliDiscoveryTransport transport = {
        .transact = mock_transact,
        .transact_sequence = mock_transact_sequence,
        .ctx = &mock,
    };
    DaliInputPollResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT,
                      dali_input_poll_value(&transport, 5u, 2u, 3u, &result));
    TEST_ASSERT_EQUAL_UINT8(1u, mock.sequence_calls);
    TEST_ASSERT_EQUAL_UINT8(0u, mock.frame_calls);
    TEST_ASSERT_FALSE(result.value.complete);
    TEST_ASSERT_EQUAL(DALI_OK, result.byte_errors[0]);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, result.byte_errors[1]);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, result.byte_errors[2]);
}

/* Independent frame vectors: address byte (addr << 1 | 1), instance byte, then
 * QUERY INPUT VALUE (0x8C) or QUERY INPUT VALUE LATCH (0x8D). */
#define POLL_SEQ_ADDR      5u
#define POLL_SEQ_INSTANCE  2u
#define POLL_SEQ_FRAME_MSB   0x0B028Cu
#define POLL_SEQ_FRAME_LATCH 0x0B028Du

static DaliSequenceResult make_poll_result(const uint8_t *bytes, uint8_t count)
{
    DaliSequenceResult result = {
        .result      = DALI_OK,
        .failed_step = DALI_SEQUENCE_NO_FAILED_STEP,
        .steps_run   = count,
    };

    for (uint8_t i = 0u; i < count; i++) {
        result.replies[i] = (DaliFrame){
            .data       = bytes[i],
            .bit_length = DALI_BACKWARD_FRAME_BITS,
        };
        result.reply_mask |= (uint8_t)(1u << i);
    }
    return result;
}

void test_build_value_sequence_latches_then_reads_remaining_bytes(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_poll_build_value_sequence(
                          POLL_SEQ_ADDR, POLL_SEQ_INSTANCE, 2u, &seq));
    TEST_ASSERT_EQUAL_UINT8(2u, seq.step_count);

    TEST_ASSERT_EQUAL_HEX32(POLL_SEQ_FRAME_MSB, seq.steps[0].frame.data);
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, seq.steps[0].frame.bit_length);
    TEST_ASSERT_TRUE(seq.steps[0].needs_reply);
    TEST_ASSERT_FALSE(seq.steps[0].send_twice);
    TEST_ASSERT_EQUAL_UINT8(0u, seq.steps[0].retries_left);

    TEST_ASSERT_EQUAL_HEX32(POLL_SEQ_FRAME_LATCH, seq.steps[1].frame.data);
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, seq.steps[1].frame.bit_length);
    TEST_ASSERT_TRUE(seq.steps[1].needs_reply);

    /* A one-byte instance needs the latching query only. */
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_poll_build_value_sequence(
                          POLL_SEQ_ADDR, POLL_SEQ_INSTANCE, 1u, &seq));
    TEST_ASSERT_EQUAL_UINT8(1u, seq.step_count);
    TEST_ASSERT_EQUAL_HEX32(POLL_SEQ_FRAME_MSB, seq.steps[0].frame.data);

    /* The widest supported reading still fits one sequence. */
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_poll_build_value_sequence(
                          POLL_SEQ_ADDR, POLL_SEQ_INSTANCE,
                          DALI_INPUT_POLL_MAX_BYTES, &seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_INPUT_POLL_MAX_BYTES, seq.step_count);
    TEST_ASSERT_EQUAL_HEX32(POLL_SEQ_FRAME_LATCH,
                            seq.steps[DALI_INPUT_POLL_MAX_BYTES - 1u].frame.data);
}

void test_build_value_sequence_rejects_invalid_args(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_build_value_sequence(5u, 0u, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_build_value_sequence(64u, 0u, 1u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_build_value_sequence(5u, 32u, 1u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_build_value_sequence(5u, 0u, 0u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_build_value_sequence(
                          5u, 0u, DALI_INPUT_POLL_MAX_BYTES + 1u, &seq));
}

void test_value_from_sequence_combines_bytes_msb_first(void)
{
    const uint8_t bytes[2] = { 0x12u, 0x34u };
    DaliSequenceResult result = make_poll_result(bytes, 2u);
    DaliInputValue value;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_poll_value_from_sequence(&result, 2u, &value));
    TEST_ASSERT_EQUAL_UINT32(0x1234u, value.value);
    TEST_ASSERT_EQUAL_UINT8(2u, value.byte_count);
    TEST_ASSERT_TRUE(value.complete);

    /* Reading fewer bytes than the sequence returned stops at the request. */
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_poll_value_from_sequence(&result, 1u, &value));
    TEST_ASSERT_EQUAL_UINT32(0x12u, value.value);
    TEST_ASSERT_TRUE(value.complete);
}

void test_value_from_sequence_rejects_partial_and_failed_reads(void)
{
    const uint8_t bytes[2] = { 0x12u, 0x34u };
    DaliInputValue value;

    /* Only the latching step replied — never publish a half-formed reading. */
    DaliSequenceResult partial = make_poll_result(bytes, 1u);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_input_poll_value_from_sequence(&partial, 2u, &value));

    /* A failed sequence reports its own error rather than a value. */
    DaliSequenceResult failed = make_poll_result(bytes, 1u);
    failed.result      = DALI_ERR_TIMEOUT;
    failed.failed_step = 1u;
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT,
                      dali_input_poll_value_from_sequence(&failed, 2u, &value));

    /* A forward frame in a reply slot is not a backward reply. */
    DaliSequenceResult wrong_width = make_poll_result(bytes, 2u);
    wrong_width.replies[1].bit_length = DALI_FORWARD_FRAME_BITS;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value_from_sequence(&wrong_width, 2u, &value));

    DaliSequenceResult ok = make_poll_result(bytes, 2u);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value_from_sequence(NULL, 2u, &value));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value_from_sequence(&ok, 2u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value_from_sequence(&ok, 0u, &value));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_input_poll_value_from_sequence(
                          &ok, DALI_INPUT_POLL_MAX_BYTES + 1u, &value));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bytes_for_resolution_clamps_to_one_to_four);
    RUN_TEST(test_poll_value_reads_msb_then_latch_bytes);
    RUN_TEST(test_poll_value_rejects_invalid_args);
    RUN_TEST(test_poll_value_reports_atomic_step_failure_without_partial_value);
    RUN_TEST(test_build_value_sequence_latches_then_reads_remaining_bytes);
    RUN_TEST(test_build_value_sequence_rejects_invalid_args);
    RUN_TEST(test_value_from_sequence_combines_bytes_msb_first);
    RUN_TEST(test_value_from_sequence_rejects_partial_and_failed_reads);
    return UNITY_END();
}
