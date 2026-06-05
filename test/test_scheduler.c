/*
 * test_scheduler.c — unit tests for DaliScheduler
 *
 * Uses a mock PHY (controllable tx result, injectable rx frames) and a
 * manually-advanced tick counter so tests run deterministically with no
 * real hardware and no FreeRTOS.
 */

#include "unity.h"
#include "dali_scheduler.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Mock PHY state
 * --------------------------------------------------------------------------*/
static DaliFrame       g_mock_last_tx;
static int             g_mock_tx_count;
static DaliError       g_mock_tx_result;
static DaliPhyRxCallback g_mock_rx_cb;
static void           *g_mock_rx_ctx;
static uint32_t        g_mock_tick_ms;
static uint32_t        g_mock_time_us;

static DaliError mock_tx(const DaliFrame *frame)
{
    g_mock_last_tx = *frame;
    g_mock_tx_count++;
    return g_mock_tx_result;
}

static void mock_set_rx_callback(DaliPhyRxCallback cb, void *ctx)
{
    g_mock_rx_cb  = cb;
    g_mock_rx_ctx = ctx;
}

static uint32_t mock_get_tick_ms(void)
{
    return g_mock_tick_ms;
}

static uint32_t mock_get_time_us(void)
{
    return g_mock_time_us;
}

/* ---------------------------------------------------------------------------
 * Completion capture
 * --------------------------------------------------------------------------*/
static DaliError  g_cb_result;
static DaliFrame  g_cb_reply;
static int        g_cb_count;
static DaliFrame  g_event_frame;
static int        g_event_count;
static void      *g_event_ctx;
static uint8_t    g_event_marker;
static DaliSchedTraceEvent g_trace_event;
static int        g_trace_count;
static void      *g_trace_ctx;
static uint8_t    g_trace_marker;

static void on_complete(DaliError result, const DaliFrame *reply, void *ctx)
{
    (void)ctx;
    g_cb_result = result;
    if (reply != NULL) {
        g_cb_reply = *reply;
    } else {
        memset(&g_cb_reply, 0, sizeof(g_cb_reply));
    }
    g_cb_count++;
}

static void on_event(const DaliFrame *frame, void *ctx)
{
    if (frame != NULL) {
        g_event_frame = *frame;
    } else {
        memset(&g_event_frame, 0, sizeof(g_event_frame));
    }
    g_event_ctx = ctx;
    g_event_count++;
}

static void on_trace(const DaliSchedTraceEvent *event, void *ctx)
{
    if (event != NULL) {
        g_trace_event = *event;
    } else {
        memset(&g_trace_event, 0, sizeof(g_trace_event));
    }
    g_trace_ctx = ctx;
    g_trace_count++;
}

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------*/
static void advance_past_settle(void)
{
    g_mock_tick_ms += DALI_SETTLE_MS;
    g_mock_time_us += (uint32_t)DALI_SETTLE_MS * 1000u;
}

static void inject_reply(uint32_t data, uint8_t bits)
{
    DaliFrame f = { .data = data, .bit_length = bits };
    dali_sched_notify_rx(&f);
}

static void inject_event(uint32_t data)
{
    inject_reply(data, 24u);
}

/* ---------------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------------*/
void setUp(void)
{
    g_mock_tx_count  = 0;
    g_mock_tx_result = DALI_OK;
    g_mock_tick_ms   = 0u;
    g_mock_time_us   = 0u;
    g_mock_rx_cb     = NULL;
    g_mock_rx_ctx    = NULL;
    memset(&g_mock_last_tx, 0, sizeof(g_mock_last_tx));

    g_cb_count  = 0;
    g_cb_result = DALI_OK;
    memset(&g_cb_reply, 0, sizeof(g_cb_reply));
    g_event_count = 0;
    g_event_ctx   = NULL;
    memset(&g_event_frame, 0, sizeof(g_event_frame));
    g_trace_count = 0;
    g_trace_ctx   = NULL;
    memset(&g_trace_event, 0, sizeof(g_trace_event));

    memset(&g_dali_stats, 0, sizeof(g_dali_stats));

    DaliSchedOps ops = {
        .tx              = mock_tx,
        .set_rx_callback = mock_set_rx_callback,
        .get_tick_ms     = mock_get_tick_ms,
        .get_time_us     = mock_get_time_us,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_init(&ops));
}

void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------------*/

/* 1. Simple send — no reply, no send_twice */
void test_simple_send(void)
{
    DaliTransaction txn = {
        .frame       = { .data = 0x0080u, .bit_length = 16u },
        .needs_reply = false,
        .send_twice  = false,
        .retries_left = 0u,
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    /* First run: IDLE → TX → WAIT_SETTLE (returns waiting) */
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0080u, g_mock_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, g_mock_last_tx.bit_length);
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());
    TEST_ASSERT_EQUAL(0, g_cb_count);

    /* Still in settle — should not advance */
    g_mock_tick_ms = DALI_SETTLE_MS - 1u;
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(0, g_cb_count);

    /* Settle elapsed: complete → IDLE */
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);   /* no extra TX */
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_trace_callback_reports_tx_from_task_context(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_trace_callback(on_trace, &g_trace_marker));

    g_mock_time_us = 123400u;
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = false,
        .send_twice   = false,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_trace_count);
    TEST_ASSERT_EQUAL(DALI_SCHED_TRACE_TX, g_trace_event.direction);
    TEST_ASSERT_EQUAL_HEX32(0x0B90u, g_trace_event.frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, g_trace_event.frame.bit_length);
    TEST_ASSERT_EQUAL_UINT32(123400u, g_trace_event.timestamp_us);
    TEST_ASSERT_FALSE(g_trace_event.has_since_tx);
    TEST_ASSERT_EQUAL_PTR(&g_trace_marker, g_trace_ctx);
}

void test_trace_callback_reports_rx_time_since_last_tx(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_trace_callback(on_trace, &g_trace_marker));

    g_mock_time_us = 100000u;
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    g_mock_time_us = 110100u;
    inject_reply(0xAFu, 8u);

    TEST_ASSERT_EQUAL(2, g_trace_count);
    TEST_ASSERT_EQUAL(DALI_SCHED_TRACE_RX, g_trace_event.direction);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, g_trace_event.frame.data);
    TEST_ASSERT_EQUAL_UINT8(8u, g_trace_event.frame.bit_length);
    TEST_ASSERT_EQUAL_UINT32(110100u, g_trace_event.timestamp_us);
    TEST_ASSERT_TRUE(g_trace_event.has_since_tx);
    TEST_ASSERT_EQUAL_UINT32(10100u, g_trace_event.since_tx_us);
    TEST_ASSERT_EQUAL_PTR(&g_trace_marker, g_trace_ctx);
}

/* 2. Send-twice — frame must be transmitted exactly twice */
void test_send_twice(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0xFF00u, .bit_length = 16u },
        .needs_reply  = false,
        .send_twice   = true,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    /* First TX */
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());

    /* After first settle: second TX */
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());
    TEST_ASSERT_EQUAL(0, g_cb_count);   /* not done yet */

    /* After second settle: complete */
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);   /* no third TX */
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

/* 3. Reply received within timeout */
void test_reply_received(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();   /* TX → WAIT_SETTLE */
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);

    advance_past_settle();
    dali_sched_run();   /* WAIT_SETTLE → WAIT_REPLY */
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());
    TEST_ASSERT_EQUAL(0, g_cb_count);

    inject_reply(0xAFu, 8u);
    dali_sched_run();   /* WAIT_REPLY → complete → IDLE */
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, g_cb_reply.data);
    TEST_ASSERT_EQUAL_UINT8(8u, g_cb_reply.bit_length);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

/* 4. Stray RX while idle must not complete the next transaction */
void test_stray_rx_while_idle_is_ignored(void)
{
    inject_reply(0x11u, 8u);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);

    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    g_mock_tick_ms += DALI_REPLY_TIMEOUT_MS;
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, g_cb_result);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.reply_timeouts);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);
}

/* 5. A duplicate/stale RX frame must not overwrite the latched reply */
void test_stale_duplicate_rx_does_not_overwrite_latched_reply(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    inject_reply(0xAAu, 8u);
    inject_reply(0xBBu, 8u);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAAu, g_cb_reply.data);
    TEST_ASSERT_EQUAL_UINT8(8u, g_cb_reply.bit_length);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);
}

/* 6. RX arriving after the reply window must time out, not complete OK */
void test_late_rx_after_reply_timeout_is_ignored(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    g_mock_tick_ms += DALI_REPLY_TIMEOUT_MS;
    inject_reply(0xAFu, 8u);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, g_cb_result);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.reply_timeouts);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);
}

/* 7. Unsolicited 24-bit frames are routed to the raw event path */
void test_unsolicited_24bit_idle_routes_event(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_event_callback(on_event, &g_event_marker));

    inject_event(0x123456u);

    TEST_ASSERT_EQUAL(1, g_event_count);
    TEST_ASSERT_EQUAL_HEX32(0x123456u, g_event_frame.data);
    TEST_ASSERT_EQUAL_UINT8(24u, g_event_frame.bit_length);
    TEST_ASSERT_EQUAL_PTR(&g_event_marker, g_event_ctx);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.unsolicited_events_routed);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_ignored_outside_reply);
}

/* 8. A 24-bit RX candidate during settle is ignored, not routed as an event */
void test_24bit_rx_during_settle_is_ignored_not_routed(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_event_callback(on_event, &g_event_marker));

    DaliTransaction txn = {
        .frame        = { .data = 0x123456u, .bit_length = 24u },
        .needs_reply  = false,
        .send_twice   = false,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());

    inject_event(0x123456u);
    dali_sched_run();

    TEST_ASSERT_EQUAL(0, g_event_count);
    TEST_ASSERT_EQUAL(0, g_cb_count);
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.unsolicited_events_routed);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);

    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
}

/* 9. A 24-bit event during WAIT_REPLY must not complete the transaction */
void test_unsolicited_24bit_during_reply_window_routes_without_completing(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_event_callback(on_event, &g_event_marker));

    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    inject_event(0x123456u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(0, g_cb_count);
    TEST_ASSERT_EQUAL(1, g_event_count);
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.unsolicited_events_routed);

    inject_reply(0xAFu, 8u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, g_cb_reply.data);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_ignored_outside_reply);
}

/* 10. Non-8-bit traffic cannot satisfy a solicited backward reply */
void test_16bit_frame_during_reply_window_is_ignored_not_reply(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    inject_reply(0x1234u, 16u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(0, g_cb_count);
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);

    inject_reply(0xAFu, 8u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, g_cb_reply.data);
}

/* 11. Reply timeout followed by successful retry */
void test_reply_timeout_then_retry_succeeds(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 1u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    /* First attempt */
    dali_sched_run();                           /* TX(1) → WAIT_SETTLE */
    advance_past_settle();
    dali_sched_run();                           /* → WAIT_REPLY */

    /* Timeout → retry → TX(2) → WAIT_SETTLE, all in one run() call */
    g_mock_tick_ms += DALI_REPLY_TIMEOUT_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL(0, g_cb_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.reply_timeouts);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.tx_retries);

    /* Second attempt: reply arrives */
    advance_past_settle();
    dali_sched_run();                           /* → WAIT_REPLY */
    inject_reply(0xAFu, 8u);
    dali_sched_run();                           /* → complete OK */
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, g_cb_reply.data);
}

/* 12. Reply timeout with retries exhausted -> ERR_TIMEOUT */
void test_reply_timeout_exhausted(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 0u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();   /* → WAIT_REPLY */

    g_mock_tick_ms += DALI_REPLY_TIMEOUT_MS;
    dali_sched_run();   /* → ERR_TIMEOUT → IDLE */

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, g_cb_result);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.reply_timeouts);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.tx_retries);   /* no retry */
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

/* 13. Queue full returns DALI_ERR_QUEUE_FULL */
void test_queue_full(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0080u, .bit_length = 16u },
        .needs_reply  = false,
        .retries_left = 0u,
    };
    for (uint8_t i = 0u; i < DALI_CMD_QUEUE_SIZE; i++) {
        TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    }
    TEST_ASSERT_EQUAL(DALI_ERR_QUEUE_FULL, dali_sched_enqueue(&txn));
}

/* 14. Reset clears mid-flight state */
void test_reset_clears_state(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0080u, .bit_length = 16u },
        .needs_reply  = true,
        .retries_left = 0u,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    dali_sched_run();               /* TX → WAIT_SETTLE */

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_reset());
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());

    /* No further TX calls after reset */
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
}

/* 15. Multiple queued transactions drain in order */
void test_multiple_transactions_in_order(void)
{
    DaliTransaction txn = { .needs_reply = false, .retries_left = 0u };

    for (uint8_t i = 1u; i <= 3u; i++) {
        txn.frame.data       = (uint32_t)i;
        txn.frame.bit_length = 16u;
        TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    }

    /* Transaction 1 */
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(1u, g_mock_last_tx.data);

    /* Settle(1) → TX(2) within the same run() call */
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(2u, g_mock_last_tx.data);

    /* Settle(2) → TX(3) */
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(3, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(3u, g_mock_last_tx.data);

    /* Settle(3) → queue empty, IDLE */
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(3, g_mock_tx_count);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------*/
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_simple_send);
    RUN_TEST(test_trace_callback_reports_tx_from_task_context);
    RUN_TEST(test_trace_callback_reports_rx_time_since_last_tx);
    RUN_TEST(test_send_twice);
    RUN_TEST(test_reply_received);
    RUN_TEST(test_stray_rx_while_idle_is_ignored);
    RUN_TEST(test_stale_duplicate_rx_does_not_overwrite_latched_reply);
    RUN_TEST(test_late_rx_after_reply_timeout_is_ignored);
    RUN_TEST(test_unsolicited_24bit_idle_routes_event);
    RUN_TEST(test_24bit_rx_during_settle_is_ignored_not_routed);
    RUN_TEST(test_unsolicited_24bit_during_reply_window_routes_without_completing);
    RUN_TEST(test_16bit_frame_during_reply_window_is_ignored_not_reply);
    RUN_TEST(test_reply_timeout_then_retry_succeeds);
    RUN_TEST(test_reply_timeout_exhausted);
    RUN_TEST(test_queue_full);
    RUN_TEST(test_reset_clears_state);
    RUN_TEST(test_multiple_transactions_in_order);

    return UNITY_END();
}
