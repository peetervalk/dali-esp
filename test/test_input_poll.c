#include "unity.h"

#include "dali_input_poll.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    uint8_t calls;
    uint8_t replies[4];
    DaliFrame frames[4];
} MockPollTransport;

static DaliError mock_transact(const DaliFrame *frame,
                               bool needs_reply,
                               uint8_t retries_left,
                               bool send_twice,
                               DaliFrame *reply_out,
                               void *ctx)
{
    MockPollTransport *mock = (MockPollTransport *)ctx;

    TEST_ASSERT_NOT_NULL(mock);
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_TRUE(needs_reply);
    TEST_ASSERT_EQUAL_UINT8(1u, retries_left);
    TEST_ASSERT_FALSE(send_twice);
    TEST_ASSERT_NOT_NULL(reply_out);
    TEST_ASSERT_TRUE(mock->calls < 4u);

    mock->frames[mock->calls] = *frame;
    *reply_out = (DaliFrame){
        .data = mock->replies[mock->calls],
        .bit_length = DALI_BACKWARD_FRAME_BITS,
    };
    mock->calls++;
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
        .ctx = &mock,
    };
    DaliInputPollResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_input_poll_value(&transport, 5u, 2u, 2u, &result));

    TEST_ASSERT_EQUAL_UINT8(2u, mock.calls);
    TEST_ASSERT_EQUAL_HEX32(0x0B028Cu, mock.frames[0].data);
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, mock.frames[0].bit_length);
    TEST_ASSERT_EQUAL_HEX32(0x0B028Du, mock.frames[1].data);
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, mock.frames[1].bit_length);
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
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bytes_for_resolution_clamps_to_one_to_four);
    RUN_TEST(test_poll_value_reads_msb_then_latch_bytes);
    RUN_TEST(test_poll_value_rejects_invalid_args);
    return UNITY_END();
}
