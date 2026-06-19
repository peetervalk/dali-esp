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

static DaliMemoryTransport s_transport = {
    .transact = mock_transact,
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

/* ---------------------------------------------------------------------------
 * dali_memory_read_bank0_identity
 * --------------------------------------------------------------------------*/

/* Push 18 bytes for a full Bank 0 read (offsets 0x00..0x11) */
static void push_bank0(uint8_t last_addr,
                       uint8_t indicator,
                       const uint8_t gtin[6],
                       uint8_t fw_major,
                       uint8_t fw_minor,
                       const uint8_t serial[8])
{
    push_reply(last_addr);
    push_reply(indicator);
    for (uint8_t i = 0u; i < 6u; i++) { push_reply(gtin[i]);   }
    push_reply(fw_major);
    push_reply(fw_minor);
    for (uint8_t i = 0u; i < 8u; i++) { push_reply(serial[i]); }
}

void test_bank0_identity_parses_correctly(void)
{
    const uint8_t gtin[6]   = {0x04u, 0x00u, 0x78u, 0x1Au, 0x00u, 0x01u};
    const uint8_t serial[8] = {0x01u, 0x02u, 0x03u, 0x04u,
                                0x05u, 0x06u, 0x07u, 0x08u};
    push_bank0(0x11u, DALI_MEMORY_BANK_IMPLEMENTED, gtin, 2u, 5u, serial);

    DaliMemoryBank0Identity id;
    memset(&id, 0, sizeof(id));
    DaliError err = dali_memory_read_bank0_identity(&s_transport, 3u, &id);

    TEST_ASSERT_EQUAL_INT(DALI_OK, err);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(gtin,   id.gtin,   6u);
    TEST_ASSERT_EQUAL_UINT8(2u, id.fw_major);
    TEST_ASSERT_EQUAL_UINT8(5u, id.fw_minor);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(serial, id.serial, 8u);
}

void test_bank0_identity_reads_correct_bank_and_offset(void)
{
    const uint8_t gtin[6]   = {0u};
    const uint8_t serial[8] = {0u};
    push_bank0(0x11u, DALI_MEMORY_BANK_IMPLEMENTED, gtin, 0u, 0u, serial);

    DaliMemoryBank0Identity id;
    dali_memory_read_bank0_identity(&s_transport, 0u, &id);

    /* Frame 0 must be DTR1 for bank 0 */
    DaliFrame exp_dtr1 = dali_memory_build_dtr1_bank(DALI_MEMORY_BANK0);
    TEST_ASSERT_EQUAL_UINT32(exp_dtr1.data, s_frames[0].data);

    /* Frame 1 must be DTR0 for offset 0x00 */
    DaliFrame exp_dtr0 = dali_memory_build_dtr0_offset(DALI_MEMORY_BANK0_OFFSET_LAST_ADDR);
    TEST_ASSERT_EQUAL_UINT32(exp_dtr0.data, s_frames[1].data);
}

void test_bank0_identity_bad_indicator_returns_invalid(void)
{
    const uint8_t gtin[6]   = {0u};
    const uint8_t serial[8] = {0u};
    push_bank0(0x11u, 0x00u /* not 0xFF */, gtin, 0u, 0u, serial);

    DaliMemoryBank0Identity id;
    DaliError err = dali_memory_read_bank0_identity(&s_transport, 0u, &id);

    TEST_ASSERT_EQUAL_INT(DALI_ERR_INVALID, err);
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

void test_bank0_identity_total_frame_count(void)
{
    /* DTR1 + DTR0 + 18 READs = 20 frames */
    const uint8_t gtin[6]   = {0u};
    const uint8_t serial[8] = {0u};
    push_bank0(0x11u, DALI_MEMORY_BANK_IMPLEMENTED, gtin, 0u, 0u, serial);

    DaliMemoryBank0Identity id;
    dali_memory_read_bank0_identity(&s_transport, 0u, &id);

    TEST_ASSERT_EQUAL_UINT8(2u + DALI_MEMORY_BANK0_IDENTITY_SIZE, s_frame_count);
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

    RUN_TEST(test_read_byte_sends_three_frames);
    RUN_TEST(test_read_byte_first_two_frames_no_reply);
    RUN_TEST(test_read_byte_third_frame_needs_reply);
    RUN_TEST(test_read_byte_correct_frame_sequence);
    RUN_TEST(test_read_byte_returns_reply_value);
    RUN_TEST(test_read_byte_timeout_propagates);
    RUN_TEST(test_read_byte_null_out_returns_invalid);
    RUN_TEST(test_read_byte_null_transport_returns_invalid);

    RUN_TEST(test_read_bytes_sends_dtr_once_then_n_reads);
    RUN_TEST(test_read_bytes_same_read_frame_reused);
    RUN_TEST(test_read_bytes_values_returned_in_order);
    RUN_TEST(test_read_bytes_count_zero_sends_no_frames);
    RUN_TEST(test_read_bytes_mid_sequence_error_propagates);
    RUN_TEST(test_read_bytes_null_buf_returns_invalid);

    RUN_TEST(test_bank0_identity_parses_correctly);
    RUN_TEST(test_bank0_identity_reads_correct_bank_and_offset);
    RUN_TEST(test_bank0_identity_bad_indicator_returns_invalid);
    RUN_TEST(test_bank0_identity_transport_error_propagates);
    RUN_TEST(test_bank0_identity_null_out_returns_invalid);
    RUN_TEST(test_bank0_identity_total_frame_count);

    return UNITY_END();
}
