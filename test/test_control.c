/*
 * test_control.c - unit tests for the DALI command translator.
 */

#include "unity.h"
#include "dali_control.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Mock scheduler ops
 * --------------------------------------------------------------------------*/
static DaliFrame         s_last_tx;
static uint8_t           s_tx_count;
static DaliError         s_tx_result;
static DaliPhyRxCallback s_rx_cb;
static void             *s_rx_ctx;
static uint32_t          s_tick_ms;

static DaliError mock_tx(const DaliFrame *frame)
{
    s_last_tx = *frame;
    s_tx_count++;
    return s_tx_result;
}

static void mock_set_rx_callback(DaliPhyRxCallback cb, void *ctx)
{
    s_rx_cb  = cb;
    s_rx_ctx = ctx;
}

static uint32_t mock_get_tick_ms(void)
{
    return s_tick_ms;
}

/* ---------------------------------------------------------------------------
 * Completion capture
 * --------------------------------------------------------------------------*/
static DaliError s_cb_result;
static DaliFrame s_cb_reply;
static uint8_t   s_cb_count;

static void on_complete(DaliError result, const DaliFrame *reply, void *cb_ctx)
{
    (void)cb_ctx;
    s_cb_result = result;
    if (reply != NULL) {
        s_cb_reply = *reply;
    } else {
        memset(&s_cb_reply, 0, sizeof(s_cb_reply));
    }
    s_cb_count++;
}

static DaliTarget target(DaliAddressType type, uint8_t address)
{
    DaliTarget t = {
        .type    = type,
        .address = address,
    };
    return t;
}

/* ---------------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------------*/
void setUp(void)
{
    memset(&s_last_tx, 0, sizeof(s_last_tx));
    memset(&s_cb_reply, 0, sizeof(s_cb_reply));
    s_tx_count  = 0u;
    s_tx_result = DALI_OK;
    s_rx_cb     = NULL;
    s_rx_ctx    = NULL;
    s_tick_ms   = 0u;
    s_cb_result = DALI_OK;
    s_cb_count  = 0u;
    memset(&g_dali_stats, 0, sizeof(g_dali_stats));

    DaliSchedOps ops = {
        .tx              = mock_tx,
        .set_rx_callback = mock_set_rx_callback,
        .get_tick_ms     = mock_get_tick_ms,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_init(&ops));
}

void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------------*/
void test_brightness_conversions(void)
{
    TEST_ASSERT_EQUAL_UINT8(0u, dali_control_ha_brightness_to_dapc(0u));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_control_ha_brightness_to_dapc(1u));
    TEST_ASSERT_EQUAL_UINT8(128u, dali_control_ha_brightness_to_dapc(128u));
    TEST_ASSERT_EQUAL_UINT8(254u, dali_control_ha_brightness_to_dapc(255u));

    TEST_ASSERT_EQUAL_UINT8(0u, dali_control_percent_to_dapc(0u));
    TEST_ASSERT_EQUAL_UINT8(128u, dali_control_percent_to_dapc(50u));
    TEST_ASSERT_EQUAL_UINT8(254u, dali_control_percent_to_dapc(100u));
    TEST_ASSERT_EQUAL_UINT8(254u, dali_control_percent_to_dapc(150u));
}

void test_build_group_dapc_example(void)
{
    DaliFrame frame;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_dapc(target(DALI_ADDR_GROUP, 0u), 128u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8080u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_build_short_and_broadcast_dapc(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_dapc(target(DALI_ADDR_SHORT, 1u), 128u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0280u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_dapc(target(DALI_ADDR_BROADCAST, 0u), 128u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFE80u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_build_off_for_group_and_broadcast(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_off(target(DALI_ADDR_GROUP, 0u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8100u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_off(target(DALI_ADDR_BROADCAST, 0u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFF00u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_invalid_targets_and_levels_are_rejected(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_dapc(target(DALI_ADDR_SHORT, 64u), 1u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_dapc(target(DALI_ADDR_GROUP, 16u), 1u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_dapc(target(DALI_ADDR_GROUP, 0u), 255u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_dapc(target(DALI_ADDR_GROUP, 0u), 1u, NULL));
}

void test_set_brightness_group_enqueues_dapc(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_set_brightness(target(DALI_ADDR_GROUP, 0u), 128u));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8080u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
}

void test_set_brightness_zero_enqueues_off(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_set_brightness(target(DALI_ADDR_GROUP, 0u), 0u));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8100u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
}

void test_set_percent_group_50_enqueues_dapc_128(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_set_percent(target(DALI_ADDR_GROUP, 0u), 50u));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8080u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
}

void test_query_status_short_address_completes_with_reply(void)
{
    DaliFrame reply = {
        .data       = 0xAFu,
        .bit_length = 8u,
    };

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_query_status(target(DALI_ADDR_SHORT, 5u),
                                                on_complete,
                                                NULL));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0B90u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);

    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    dali_sched_notify_rx(&reply);
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, s_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, s_cb_reply.data);
    TEST_ASSERT_EQUAL_UINT8(8u, s_cb_reply.bit_length);
}

void test_query_status_rejects_group_broadcast_and_null_callback(void)
{
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_query_status(target(DALI_ADDR_GROUP, 0u),
                                                on_complete,
                                                NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_query_status(target(DALI_ADDR_BROADCAST, 0u),
                                                on_complete,
                                                NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_query_status(target(DALI_ADDR_SHORT, 0u),
                                                NULL,
                                                NULL));
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------*/
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_brightness_conversions);
    RUN_TEST(test_build_group_dapc_example);
    RUN_TEST(test_build_short_and_broadcast_dapc);
    RUN_TEST(test_build_off_for_group_and_broadcast);
    RUN_TEST(test_invalid_targets_and_levels_are_rejected);
    RUN_TEST(test_set_brightness_group_enqueues_dapc);
    RUN_TEST(test_set_brightness_zero_enqueues_off);
    RUN_TEST(test_set_percent_group_50_enqueues_dapc_128);
    RUN_TEST(test_query_status_short_address_completes_with_reply);
    RUN_TEST(test_query_status_rejects_group_broadcast_and_null_callback);
    return UNITY_END();
}
