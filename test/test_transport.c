#include "unity.h"

#include <string.h>

#include "dali_transport.h"

/* ---------------------------------------------------------------------------
 * Frame-level mock — stands in for a transport with no atomic grouping
 * --------------------------------------------------------------------------*/

#define MOCK_MAX_FRAMES 16u

static DaliFrame s_frames[MOCK_MAX_FRAMES];
static bool      s_needs_reply[MOCK_MAX_FRAMES];
static uint8_t   s_retries[MOCK_MAX_FRAMES];
static bool      s_send_twice[MOCK_MAX_FRAMES];
static uint8_t   s_frame_count;

static uint8_t   s_replies[MOCK_MAX_FRAMES];
static uint8_t   s_reply_count;
static uint8_t   s_reply_taken;

static DaliError s_fail_error;
static uint8_t   s_fail_on_frame;   /* 1-based; 0 disables */

static uint8_t   s_sequence_calls;
static DaliSequence s_last_sequence;
static bool      s_sequence_writes_result;
static uint8_t   s_cleanup_calls;

void setUp(void)
{
    memset(s_frames, 0, sizeof(s_frames));
    memset(s_needs_reply, 0, sizeof(s_needs_reply));
    memset(s_retries, 0, sizeof(s_retries));
    memset(s_send_twice, 0, sizeof(s_send_twice));
    s_frame_count = 0u;
    memset(s_replies, 0, sizeof(s_replies));
    s_reply_count = 0u;
    s_reply_taken = 0u;
    s_fail_error = DALI_OK;
    s_fail_on_frame = 0u;
    s_sequence_calls = 0u;
    memset(&s_last_sequence, 0, sizeof(s_last_sequence));
    s_sequence_writes_result = true;
    s_cleanup_calls = 0u;
}

void tearDown(void) {}

static void push_reply(uint8_t value)
{
    if (s_reply_count < MOCK_MAX_FRAMES) {
        s_replies[s_reply_count++] = value;
    }
}

static DaliError mock_transact(const DaliFrame *frame,
                               bool             needs_reply,
                               uint8_t          retries_left,
                               bool             send_twice,
                               DaliFrame       *reply_out,
                               void            *ctx)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_TRUE(s_frame_count < MOCK_MAX_FRAMES);

    s_frames[s_frame_count]      = *frame;
    s_needs_reply[s_frame_count] = needs_reply;
    s_retries[s_frame_count]     = retries_left;
    s_send_twice[s_frame_count]  = send_twice;
    s_frame_count++;

    if (s_fail_on_frame != 0u && s_frame_count == s_fail_on_frame) {
        return s_fail_error;
    }

    if (needs_reply) {
        TEST_ASSERT_NOT_NULL(reply_out);
        TEST_ASSERT_TRUE(s_reply_taken < s_reply_count);
        *reply_out = (DaliFrame){
            .data       = s_replies[s_reply_taken++],
            .bit_length = DALI_BACKWARD_FRAME_BITS,
        };
    } else {
        TEST_ASSERT_NULL(reply_out);
    }
    return DALI_OK;
}

static DaliError mock_transact_sequence(const DaliSequence *seq,
                                        DaliSequenceResult *result_out,
                                        void               *ctx)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(seq);
    s_sequence_calls++;
    s_last_sequence = *seq;

    if (result_out != NULL && s_sequence_writes_result) {
        memset(result_out, 0, sizeof(*result_out));
        result_out->result      = DALI_OK;
        result_out->failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
        result_out->steps_run   = seq->step_count;
    }
    return DALI_OK;
}

static DaliError mock_cleanup_transact(const DaliFrame *frame,
                                       bool needs_reply,
                                       uint8_t retries_left,
                                       bool send_twice,
                                       DaliFrame *reply_out,
                                       void *ctx)
{
    s_cleanup_calls++;
    return mock_transact(frame,
                         needs_reply,
                         retries_left,
                         send_twice,
                         reply_out,
                         ctx);
}

static DaliTransport frame_only_transport(void)
{
    return (DaliTransport){ .transact = mock_transact };
}

static DaliTransport atomic_transport(void)
{
    return (DaliTransport){
        .transact          = mock_transact,
        .transact_sequence = mock_transact_sequence,
    };
}

static DaliSequence two_step_sequence(void)
{
    DaliSequence seq;
    memset(&seq, 0, sizeof(seq));
    seq.steps[0].frame = (DaliFrame){ .data = 0xA310u, .bit_length = 16u };
    seq.steps[1].frame = (DaliFrame){ .data = 0x0BC5u, .bit_length = 16u };
    seq.steps[1].needs_reply = true;
    seq.step_count = 2u;
    return seq;
}

/* ---------------------------------------------------------------------------
 * Capability reporting
 * --------------------------------------------------------------------------*/

void test_transport_validity_and_capability(void)
{
    DaliTransport frame_only = frame_only_transport();
    DaliTransport atomic     = atomic_transport();
    DaliTransport sequence_only = {
        .transact_sequence = mock_transact_sequence,
    };
    DaliTransport empty      = {0};

    TEST_ASSERT_TRUE(dali_transport_valid(&frame_only));
    TEST_ASSERT_TRUE(dali_transport_valid(&atomic));
    TEST_ASSERT_FALSE(dali_transport_valid(&sequence_only));
    TEST_ASSERT_FALSE(dali_transport_valid(&empty));
    TEST_ASSERT_FALSE(dali_transport_valid(NULL));

    /* A frame-only transport must not claim atomicity it cannot provide. */
    TEST_ASSERT_FALSE(dali_transport_supports_atomic_sequence(&frame_only));
    TEST_ASSERT_TRUE(dali_transport_supports_atomic_sequence(&atomic));
    TEST_ASSERT_FALSE(dali_transport_supports_atomic_sequence(&sequence_only));
    TEST_ASSERT_FALSE(dali_transport_supports_atomic_sequence(NULL));
}

void test_cleanup_transaction_prefers_dedicated_callback(void)
{
    DaliTransport transport = frame_only_transport();
    transport.transact_cleanup = mock_cleanup_transact;
    DaliFrame frame = { .data = 0xA100u, .bit_length = 16u };

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_transport_transact_cleanup(&transport,
                                                      &frame,
                                                      false,
                                                      0u,
                                                      false,
                                                      NULL));
    TEST_ASSERT_EQUAL_UINT8(1u, s_cleanup_calls);
    TEST_ASSERT_EQUAL_UINT8(1u, s_frame_count);
    TEST_ASSERT_EQUAL_HEX32(0xA100u, s_frames[0].data);
}

void test_cleanup_transaction_falls_back_to_normal_transport(void)
{
    DaliTransport transport = frame_only_transport();
    DaliFrame frame = { .data = 0xA100u, .bit_length = 16u };

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_transport_transact_cleanup(&transport,
                                                      &frame,
                                                      false,
                                                      0u,
                                                      false,
                                                      NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, s_cleanup_calls);
    TEST_ASSERT_EQUAL_UINT8(1u, s_frame_count);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_transport_transact_cleanup(NULL,
                                                      &frame,
                                                      false,
                                                      0u,
                                                      false,
                                                      NULL));
}

/* ---------------------------------------------------------------------------
 * Atomic path
 * --------------------------------------------------------------------------*/

void test_run_sequence_delegates_when_transport_is_atomic(void)
{
    DaliTransport transport = atomic_transport();
    DaliSequence  seq       = two_step_sequence();
    DaliSequenceResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_transport_run_sequence(&transport, &seq, &result));

    /* The whole sequence goes down in one call; no frame-level traffic. */
    TEST_ASSERT_EQUAL_UINT8(1u, s_sequence_calls);
    TEST_ASSERT_EQUAL_UINT8(0u, s_frame_count);
    TEST_ASSERT_EQUAL_UINT8(2u, s_last_sequence.step_count);
    TEST_ASSERT_EQUAL_UINT8(2u, result.steps_run);
}

void test_run_sequence_seeds_result_before_delegating(void)
{
    DaliTransport transport = atomic_transport();
    DaliSequence  seq       = two_step_sequence();
    DaliSequenceResult result;

    /* A transport that ignores result_out must still leave the caller with a
     * defined outcome rather than whatever the stack held. */
    memset(&result, 0xA5, sizeof(result));
    s_sequence_writes_result = false;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_transport_run_sequence(&transport, &seq, &result));
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_NO_FAILED_STEP, result.failed_step);
    TEST_ASSERT_EQUAL_UINT8(0u, result.reply_mask);
    TEST_ASSERT_EQUAL_UINT8(0u, result.steps_run);
}

void test_run_sequence_atomic_delegates_as_one_group(void)
{
    DaliTransport transport = atomic_transport();
    DaliSequence seq = two_step_sequence();
    DaliSequenceResult result;

    TEST_ASSERT_EQUAL(
        DALI_OK,
        dali_transport_run_sequence_atomic(&transport, &seq, &result));
    TEST_ASSERT_EQUAL_UINT8(1u, s_sequence_calls);
    TEST_ASSERT_EQUAL_UINT8(0u, s_frame_count);
    TEST_ASSERT_EQUAL_UINT8(2u, result.steps_run);
}

void test_run_sequence_atomic_rejects_frame_only_transport(void)
{
    DaliTransport transport = frame_only_transport();
    DaliSequence seq = two_step_sequence();
    DaliSequenceResult result;
    memset(&result, 0xA5, sizeof(result));

    TEST_ASSERT_EQUAL(
        DALI_ERR_INVALID,
        dali_transport_run_sequence_atomic(&transport, &seq, &result));
    TEST_ASSERT_EQUAL_UINT8(0u, s_sequence_calls);
    TEST_ASSERT_EQUAL_UINT8(0u, s_frame_count);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, result.result);
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_NO_FAILED_STEP, result.failed_step);
    TEST_ASSERT_EQUAL_UINT8(0u, result.steps_run);
    TEST_ASSERT_EQUAL_UINT8(0u, result.reply_mask);
}

void test_run_sequence_atomic_rejects_sequence_only_transport(void)
{
    DaliTransport transport = {
        .transact_sequence = mock_transact_sequence,
    };
    DaliSequence seq = two_step_sequence();
    DaliSequenceResult result;
    memset(&result, 0xA5, sizeof(result));

    TEST_ASSERT_EQUAL(
        DALI_ERR_INVALID,
        dali_transport_run_sequence_atomic(&transport, &seq, &result));
    TEST_ASSERT_EQUAL_UINT8(0u, s_sequence_calls);
    TEST_ASSERT_EQUAL_UINT8(0u, s_frame_count);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, result.result);
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_NO_FAILED_STEP, result.failed_step);
    TEST_ASSERT_EQUAL_UINT8(0u, result.steps_run);
    TEST_ASSERT_EQUAL_UINT8(0u, result.reply_mask);
}

/* ---------------------------------------------------------------------------
 * Stepwise fallback
 * --------------------------------------------------------------------------*/

void test_run_sequence_falls_back_to_individual_frames(void)
{
    DaliTransport transport = frame_only_transport();
    DaliSequence  seq       = two_step_sequence();
    DaliSequenceResult result;

    push_reply(0x5Au);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_transport_run_sequence(&transport, &seq, &result));

    TEST_ASSERT_EQUAL_UINT8(0u, s_sequence_calls);
    TEST_ASSERT_EQUAL_UINT8(2u, s_frame_count);
    TEST_ASSERT_EQUAL_HEX32(0xA310u, s_frames[0].data);
    TEST_ASSERT_EQUAL_HEX32(0x0BC5u, s_frames[1].data);
    TEST_ASSERT_FALSE(s_needs_reply[0]);
    TEST_ASSERT_TRUE(s_needs_reply[1]);

    TEST_ASSERT_EQUAL(DALI_OK, result.result);
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_NO_FAILED_STEP, result.failed_step);
    TEST_ASSERT_EQUAL_UINT8(2u, result.steps_run);
    TEST_ASSERT_EQUAL_HEX8(0x02u, result.reply_mask);

    DaliFrame reply;
    TEST_ASSERT_TRUE(dali_sequence_result_reply(&result, 1u, &reply));
    TEST_ASSERT_EQUAL_HEX32(0x5Au, reply.data);
    TEST_ASSERT_FALSE(dali_sequence_result_reply(&result, 0u, &reply));
}

void test_fallback_forwards_step_retry_and_send_twice(void)
{
    DaliTransport transport = frame_only_transport();
    DaliSequence  seq       = two_step_sequence();
    seq.steps[0].send_twice   = true;
    seq.steps[1].retries_left = 3u;
    push_reply(0x11u);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_transport_run_sequence(&transport, &seq, NULL));
    TEST_ASSERT_TRUE(s_send_twice[0]);
    TEST_ASSERT_EQUAL_UINT8(0u, s_retries[0]);
    TEST_ASSERT_FALSE(s_send_twice[1]);
    TEST_ASSERT_EQUAL_UINT8(3u, s_retries[1]);
}

void test_fallback_stops_at_the_failing_step(void)
{
    DaliTransport transport = frame_only_transport();
    DaliSequence  seq       = two_step_sequence();
    seq.step_count = 2u;

    s_fail_on_frame = 1u;
    s_fail_error    = DALI_ERR_BUS_STUCK;

    DaliSequenceResult result;
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK,
                      dali_transport_run_sequence(&transport, &seq, &result));

    /* The second step must never reach the bus. */
    TEST_ASSERT_EQUAL_UINT8(1u, s_frame_count);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, result.result);
    TEST_ASSERT_EQUAL_UINT8(0u, result.failed_step);
    TEST_ASSERT_EQUAL_UINT8(1u, result.steps_run);
    TEST_ASSERT_EQUAL_UINT8(0u, result.reply_mask);
}

void test_fallback_keeps_replies_gathered_before_a_failure(void)
{
    DaliTransport transport = frame_only_transport();
    DaliSequence  seq;
    memset(&seq, 0, sizeof(seq));
    seq.steps[0].frame = (DaliFrame){ .data = 0x0BC5u, .bit_length = 16u };
    seq.steps[0].needs_reply = true;
    seq.steps[1].frame = (DaliFrame){ .data = 0x0BC5u, .bit_length = 16u };
    seq.steps[1].needs_reply = true;
    seq.step_count = 2u;

    push_reply(0x77u);
    s_fail_on_frame = 2u;
    s_fail_error    = DALI_ERR_TIMEOUT;

    DaliSequenceResult result;
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT,
                      dali_transport_run_sequence(&transport, &seq, &result));
    TEST_ASSERT_EQUAL_UINT8(1u, result.failed_step);
    TEST_ASSERT_EQUAL_HEX8(0x01u, result.reply_mask);

    DaliFrame reply;
    TEST_ASSERT_TRUE(dali_sequence_result_reply(&result, 0u, &reply));
    TEST_ASSERT_EQUAL_HEX32(0x77u, reply.data);
}

/* ---------------------------------------------------------------------------
 * Argument handling
 * --------------------------------------------------------------------------*/

void test_run_sequence_rejects_invalid_arguments(void)
{
    DaliTransport transport = frame_only_transport();
    DaliTransport empty     = {0};
    DaliSequence  seq       = two_step_sequence();
    DaliSequenceResult result;

    memset(&result, 0xA5, sizeof(result));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_transport_run_sequence(NULL, &seq, &result));
    /* Even a rejected call must leave result_out defined. */
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, result.result);
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_NO_FAILED_STEP, result.failed_step);

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_transport_run_sequence(&empty, &seq, &result));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_transport_run_sequence(&transport, NULL, &result));

    DaliSequence empty_seq;
    memset(&empty_seq, 0, sizeof(empty_seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_transport_run_sequence(&transport, &empty_seq, &result));

    DaliSequence bad_bits = two_step_sequence();
    bad_bits.steps[1].frame.bit_length = 25u;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_transport_run_sequence(&transport, &bad_bits, &result));

    DaliSequence too_many = two_step_sequence();
    too_many.step_count = (uint8_t)(DALI_SEQUENCE_MAX_STEPS + 1u);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_transport_run_sequence(&transport, &too_many, &result));

    /* Nothing invalid should have reached the bus. */
    TEST_ASSERT_EQUAL_UINT8(0u, s_frame_count);
}

/* ---------------------------------------------------------------------------
 * Blocking-wait budget
 * --------------------------------------------------------------------------*/

void test_sequence_timeout_scales_with_steps_and_retries(void)
{
    DaliSequence seq = two_step_sequence();

    uint32_t two_steps = dali_transport_sequence_timeout_ms(&seq);
    TEST_ASSERT_EQUAL_UINT32(DALI_TRANSPORT_SEQUENCE_QUEUE_BUDGET_MS +
                                 2u * DALI_TRANSPORT_SEQUENCE_STEP_BUDGET_MS,
                             two_steps);

    /* A retry budget buys another full attempt for that step. */
    seq.steps[1].retries_left = 2u;
    TEST_ASSERT_EQUAL_UINT32(two_steps + 2u * DALI_TRANSPORT_SEQUENCE_STEP_BUDGET_MS,
                             dali_transport_sequence_timeout_ms(&seq));

    /* Must exceed a single-frame wait for any real sequence. */
    TEST_ASSERT_TRUE(two_steps > DALI_REPLY_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT32(DALI_TRANSPORT_SEQUENCE_QUEUE_BUDGET_MS,
                             dali_transport_sequence_timeout_ms(NULL));

    /* A bogus step_count must not walk off the step array. */
    DaliSequence too_many = two_step_sequence();
    too_many.step_count = 200u;
    TEST_ASSERT_EQUAL_UINT32(DALI_TRANSPORT_SEQUENCE_QUEUE_BUDGET_MS +
                                 DALI_SEQUENCE_MAX_STEPS *
                                     DALI_TRANSPORT_SEQUENCE_STEP_BUDGET_MS,
                             dali_transport_sequence_timeout_ms(&too_many));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_transport_validity_and_capability);
    RUN_TEST(test_cleanup_transaction_prefers_dedicated_callback);
    RUN_TEST(test_cleanup_transaction_falls_back_to_normal_transport);
    RUN_TEST(test_run_sequence_delegates_when_transport_is_atomic);
    RUN_TEST(test_run_sequence_seeds_result_before_delegating);
    RUN_TEST(test_run_sequence_atomic_delegates_as_one_group);
    RUN_TEST(test_run_sequence_atomic_rejects_frame_only_transport);
    RUN_TEST(test_run_sequence_atomic_rejects_sequence_only_transport);
    RUN_TEST(test_run_sequence_falls_back_to_individual_frames);
    RUN_TEST(test_fallback_forwards_step_retry_and_send_twice);
    RUN_TEST(test_fallback_stops_at_the_failing_step);
    RUN_TEST(test_fallback_keeps_replies_gathered_before_a_failure);
    RUN_TEST(test_run_sequence_rejects_invalid_arguments);
    RUN_TEST(test_sequence_timeout_scales_with_steps_and_retries);
    return UNITY_END();
}
