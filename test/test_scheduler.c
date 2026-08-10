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
#define MOCK_TX_HISTORY_SIZE 16u
static DaliFrame       g_mock_last_tx;
static DaliFrame       g_mock_tx_history[MOCK_TX_HISTORY_SIZE];
static int             g_mock_tx_count;
static DaliError       g_mock_tx_result;
static int             g_mock_tx_fail_on_count;
static int             g_mock_tx_advance_on_count;
static uint32_t        g_mock_tx_advance_us;
static DaliPhyRxCallback g_mock_rx_cb;
static void           *g_mock_rx_ctx;
static uint32_t        g_mock_tick_ms;
static uint32_t        g_mock_time_us;

static DaliError mock_tx(const DaliFrame *frame)
{
    g_mock_last_tx = *frame;
    if (g_mock_tx_count < (int)MOCK_TX_HISTORY_SIZE) {
        g_mock_tx_history[g_mock_tx_count] = *frame;
    }
    g_mock_tx_count++;
    if (g_mock_tx_advance_on_count == g_mock_tx_count) {
        g_mock_time_us += g_mock_tx_advance_us;
        g_mock_tick_ms += (g_mock_tx_advance_us + 999u) / 1000u;
    }
    if (g_mock_tx_fail_on_count == g_mock_tx_count) {
        return g_mock_tx_result;
    }
    return g_mock_tx_fail_on_count == 0 ? g_mock_tx_result : DALI_OK;
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
static DaliError  g_seq_result;
static uint8_t    g_seq_failed_step;
static DaliFrame  g_seq_reply;
static bool       g_seq_has_reply;
static DaliSequenceResult g_seq_full;
static int        g_seq_count;
static void      *g_seq_ctx;
static uint8_t    g_seq_marker;

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

static void on_sequence_complete(const DaliSequenceResult *result, void *ctx)
{
    if (result != NULL) {
        g_seq_full        = *result;
        g_seq_result      = result->result;
        g_seq_failed_step = result->failed_step;
        g_seq_has_reply   = dali_sequence_result_last_reply(result, &g_seq_reply);
    } else {
        memset(&g_seq_full, 0, sizeof(g_seq_full));
        g_seq_result      = DALI_ERR_INVALID;
        g_seq_failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
        g_seq_has_reply   = false;
    }
    if (!g_seq_has_reply) {
        memset(&g_seq_reply, 0, sizeof(g_seq_reply));
    }
    g_seq_ctx = ctx;
    g_seq_count++;
}

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------*/
static void advance_past_settle(void)
{
    g_mock_tick_ms += DALI_SETTLE_MS;
    g_mock_time_us += (uint32_t)DALI_SETTLE_MS * 1000u;
}

static void advance_time_ms(uint32_t delta_ms)
{
    g_mock_tick_ms += delta_ms;
    g_mock_time_us += delta_ms * 1000u;
}

static void advance_to_next_forward(void)
{
    g_mock_tick_ms += (DALI_FORWARD_INTERFRAME_US + 999u) / 1000u;
    g_mock_time_us += DALI_FORWARD_INTERFRAME_US;
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

static void inject_legacy_event(uint32_t data)
{
    inject_reply(data, 16u);
}

/* ---------------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------------*/
void setUp(void)
{
    g_mock_tx_count  = 0;
    g_mock_tx_result = DALI_OK;
    g_mock_tx_fail_on_count = 0;
    g_mock_tx_advance_on_count = 0;
    g_mock_tx_advance_us = 0u;
    g_mock_tick_ms   = 0u;
    g_mock_time_us   = 0u;
    g_mock_rx_cb     = NULL;
    g_mock_rx_ctx    = NULL;
    memset(&g_mock_last_tx, 0, sizeof(g_mock_last_tx));
    memset(g_mock_tx_history, 0, sizeof(g_mock_tx_history));

    g_cb_count  = 0;
    g_cb_result = DALI_OK;
    memset(&g_cb_reply, 0, sizeof(g_cb_reply));
    g_event_count = 0;
    g_event_ctx   = NULL;
    memset(&g_event_frame, 0, sizeof(g_event_frame));
    g_trace_count = 0;
    g_trace_ctx   = NULL;
    memset(&g_trace_event, 0, sizeof(g_trace_event));
    g_seq_result = DALI_OK;
    g_seq_failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
    g_seq_has_reply = false;
    g_seq_count = 0;
    g_seq_ctx = NULL;
    memset(&g_seq_reply, 0, sizeof(g_seq_reply));
    memset(&g_seq_full, 0, sizeof(g_seq_full));

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

    /* The repeat waits for both RX handoff and the forward-frame gap. */
    advance_to_next_forward();
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

    advance_time_ms(DALI_REPLY_TIMEOUT_MS);
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

/* 6a. Regression: reply received before run() is called must be accepted
 *     even if the scheduler tick has crossed the timeout boundary by the
 *     time run() executes (this was the original 1ms-sleep bug). */
void test_reply_accepted_when_run_delayed_past_window(void)
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

    /* PHY delivers reply while scheduler is still in WAIT_REPLY */
    inject_reply(0xAFu, 8u);
    /* Simulated task sleep: run() is called past the timeout boundary */
    advance_time_ms(DALI_REPLY_TIMEOUT_MS);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, g_cb_reply.data);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_timeouts);
}

/* 6b. RX arriving after run() has already fired the timeout must be ignored */
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

    /* Tick crosses boundary and run() fires the timeout — no reply injected */
    advance_time_ms(DALI_REPLY_TIMEOUT_MS);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());

    /* Late reply from device arrives after timeout — must be ignored */
    inject_reply(0xAFu, 8u);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.reply_timeouts);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);
    TEST_ASSERT_EQUAL(1, g_cb_count);   /* no additional callback */
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

void test_unsolicited_16bit_idle_routes_event(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_event_callback(on_event, &g_event_marker));

    inject_legacy_event(0x8B10u);

    TEST_ASSERT_EQUAL(1, g_event_count);
    TEST_ASSERT_EQUAL_HEX32(0x8B10u, g_event_frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, g_event_frame.bit_length);
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
    advance_time_ms(DALI_REPLY_TIMEOUT_MS);
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

    advance_time_ms(DALI_REPLY_TIMEOUT_MS);
    dali_sched_run();   /* → ERR_TIMEOUT → IDLE */

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, g_cb_result);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.reply_timeouts);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.tx_retries);   /* no retry */
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_sequence_runs_all_steps_before_next_queue_entry(void)
{
    DaliSequence seq = {
        .steps = {
            {
                .frame = { .data = 0xA3C8u, .bit_length = 16u },
                .needs_reply = false,
                .send_twice = false,
                .retries_left = 0u,
            },
            {
                .frame = { .data = 0x0B2Au, .bit_length = 16u },
                .needs_reply = false,
                .send_twice = true,
                .retries_left = 0u,
            },
        },
        .step_count = 2u,
        .on_complete = on_sequence_complete,
        .cb_ctx = &g_seq_marker,
    };
    DaliTransaction txn = {
        .frame = { .data = 0xFF00u, .bit_length = 16u },
        .needs_reply = false,
        .send_twice = false,
        .retries_left = 0u,
        .on_complete = on_complete,
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&seq));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0xA3C8u, g_mock_last_tx.data);
    TEST_ASSERT_EQUAL(0, g_seq_count);

    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0B2Au, g_mock_last_tx.data);
    TEST_ASSERT_EQUAL(0, g_seq_count);

    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL(3, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0B2Au, g_mock_last_tx.data);
    TEST_ASSERT_EQUAL(0, g_seq_count);

    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL(4, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0xFF00u, g_mock_last_tx.data);
    TEST_ASSERT_EQUAL(1, g_seq_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_seq_result);
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_NO_FAILED_STEP, g_seq_failed_step);
    TEST_ASSERT_EQUAL_PTR(&g_seq_marker, g_seq_ctx);
    TEST_ASSERT_EQUAL(0, g_cb_count);

    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_seven_step_sequence_stays_contiguous_before_next_queue_entry(void)
{
    DaliSequence seq = {
        .steps = {
            { .frame = { .data = 0x010001u, .bit_length = 24u } },
            { .frame = { .data = 0x020002u, .bit_length = 24u } },
            {
                .frame = { .data = 0x030003u, .bit_length = 24u },
                .send_twice = true,
            },
            { .frame = { .data = 0x040004u, .bit_length = 24u } },
            { .frame = { .data = 0x050005u, .bit_length = 24u } },
            {
                .frame = { .data = 0x060006u, .bit_length = 24u },
                .send_twice = true,
            },
            { .frame = { .data = 0x070007u, .bit_length = 24u } },
        },
        .step_count = 7u,
        .on_complete = on_sequence_complete,
        .cb_ctx = &g_seq_marker,
    };
    DaliTransaction txn = {
        .frame = { .data = 0x0080u, .bit_length = 16u },
        .on_complete = on_complete,
    };
    static const uint32_t expected_data[] = {
        0x010001u, 0x020002u, 0x030003u, 0x030003u, 0x040004u,
        0x050005u, 0x060006u, 0x060006u, 0x070007u, 0x0080u,
    };

    TEST_ASSERT_TRUE(DALI_SEQUENCE_MAX_STEPS >= 7u);
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&seq));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    for (uint8_t i = 1u; i < 10u; i++) {
        advance_to_next_forward();
        dali_sched_run();
    }

    TEST_ASSERT_EQUAL(10, g_mock_tx_count);
    for (uint8_t i = 0u; i < 10u; i++) {
        TEST_ASSERT_EQUAL_HEX32(expected_data[i], g_mock_tx_history[i].data);
        TEST_ASSERT_EQUAL_UINT8(i < 9u ? 24u : 16u,
                                g_mock_tx_history[i].bit_length);
    }
    TEST_ASSERT_EQUAL(1, g_seq_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_seq_result);
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_NO_FAILED_STEP, g_seq_failed_step);
    TEST_ASSERT_EQUAL_PTR(&g_seq_marker, g_seq_ctx);
    TEST_ASSERT_EQUAL(0, g_cb_count);

    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_next_forward_waits_for_minimum_gap(void)
{
    DaliTransaction first = {
        .frame = { .data = 0x1111u, .bit_length = 16u },
        .on_complete = on_complete,
    };
    DaliTransaction second = {
        .frame = { .data = 0x2222u, .bit_length = 16u },
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&first));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&second));

    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);

    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(SCHED_TX, dali_sched_state());

    g_mock_time_us = DALI_FORWARD_INTERFRAME_US - 1u;
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);

    g_mock_time_us++;
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x2222u, g_mock_last_tx.data);
}

void test_forward_gap_handles_microsecond_clock_wrap(void)
{
    const uint32_t first_done_us = UINT32_MAX - 1000u;
    DaliTransaction txn = {
        .frame = { .data = 0x1111u, .bit_length = 16u },
    };
    DaliTransaction next = {
        .frame = { .data = 0x2222u, .bit_length = 16u },
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&next));

    g_mock_time_us = first_done_us;
    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(SCHED_TX, dali_sched_state());

    g_mock_time_us = first_done_us + DALI_FORWARD_INTERFRAME_US - 1u;
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);

    g_mock_time_us++;
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x2222u, g_mock_last_tx.data);
}

void test_forward_gap_does_not_reappear_after_full_us_wrap(void)
{
    const uint32_t first_done_us = 12345u;
    DaliTransaction first = {
        .frame = { .data = 0x1111u, .bit_length = 16u },
    };
    DaliTransaction second = {
        .frame = { .data = 0x2222u, .bit_length = 16u },
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&first));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&second));

    g_mock_time_us = first_done_us;
    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);

    /* A full uint32-us epoch plus 5 ms looks younger than the guard in us.
     * The slower millisecond clock proves that the guard expired long ago. */
    g_mock_time_us = first_done_us + 5000u;
    g_mock_tick_ms = (UINT32_MAX / 1000u) + 1u;
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x2222u, g_mock_last_tx.data);
}

void test_send_twice_accepts_exact_100ms_window(void)
{
    const uint32_t first_started_us = 5000u;
    DaliTransaction txn = {
        .frame = { .data = 0xFF00u, .bit_length = 16u },
        .send_twice = true,
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    g_mock_time_us = first_started_us;
    g_mock_tick_ms = first_started_us / 1000u;
    g_mock_tx_advance_on_count = 1;
    g_mock_tx_advance_us = 20000u;
    dali_sched_run();

    /* The first blocking call consumes 20 ms. Start the second after 99 ms
     * total and let it consume the final 1 ms, completing exactly at 100 ms. */
    g_mock_time_us = first_started_us + DALI_SEND_TWICE_WINDOW_US - 1000u;
    g_mock_tick_ms = first_started_us / 1000u +
                     DALI_SEND_TWICE_WINDOW_MS - 1u;
    g_mock_tx_advance_on_count = 2;
    g_mock_tx_advance_us = 1000u;
    dali_sched_run();

    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL(0, g_cb_count);
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
}

void test_send_twice_exact_window_handles_microsecond_clock_wrap(void)
{
    const uint32_t first_started_us = UINT32_MAX - 50000u;
    DaliTransaction txn = {
        .frame = { .data = 0xFF00u, .bit_length = 16u },
        .send_twice = true,
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    g_mock_time_us = first_started_us;
    g_mock_tick_ms = 100u;
    dali_sched_run();
    g_mock_time_us = first_started_us + DALI_SEND_TWICE_WINDOW_US - 1000u;
    g_mock_tick_ms += DALI_SEND_TWICE_WINDOW_MS - 1u;
    g_mock_tx_advance_on_count = 2;
    g_mock_tx_advance_us = 1000u;
    dali_sched_run();

    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
}

void test_send_twice_rejects_repeat_start_at_deadline(void)
{
    const uint32_t first_started_us = 5000u;
    DaliTransaction txn = {
        .frame = { .data = 0xFF00u, .bit_length = 16u },
        .send_twice = true,
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    g_mock_time_us = first_started_us;
    g_mock_tick_ms = first_started_us / 1000u;
    dali_sched_run();
    g_mock_time_us = first_started_us + DALI_SEND_TWICE_WINDOW_US;
    g_mock_tick_ms += DALI_SEND_TWICE_WINDOW_MS;
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMING, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_send_twice_rejects_late_repeat_before_phy(void)
{
    const uint32_t first_started_us = 5000u;
    DaliTransaction txn = {
        .frame = { .data = 0xFF00u, .bit_length = 16u },
        .send_twice = true,
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    g_mock_time_us = first_started_us;
    g_mock_tick_ms = first_started_us / 1000u;
    dali_sched_run();
    g_mock_time_us = first_started_us + DALI_SEND_TWICE_WINDOW_US + 1u;
    g_mock_tick_ms += DALI_SEND_TWICE_WINDOW_MS + 1u;
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMING, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_send_twice_rejects_delay_after_microsecond_clock_wrap(void)
{
    const uint32_t first_done_us = 12345u;
    DaliTransaction txn = {
        .frame = { .data = 0xFF00u, .bit_length = 16u },
        .send_twice = true,
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    g_mock_time_us = first_done_us;
    dali_sched_run();

    /* More than one full 32-bit-us epoch later, the us value looks only 5 ms
     * newer. The slower millisecond clock must still reject the repeat. */
    g_mock_time_us = first_done_us + 5000u;
    g_mock_tick_ms = (UINT32_MAX / 1000u) + 1u;
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMING, g_cb_result);
}

void test_send_twice_detects_phy_crossing_deadline(void)
{
    const uint32_t first_done_us = 5000u;
    DaliTransaction txn = {
        .frame = { .data = 0xFF00u, .bit_length = 16u },
        .send_twice = true,
        .on_complete = on_complete,
    };
    DaliTransaction next = {
        .frame = { .data = 0x2222u, .bit_length = 16u },
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_set_trace_callback(on_trace, NULL));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&next));

    g_mock_time_us = first_done_us;
    g_mock_tick_ms = first_done_us / 1000u;
    dali_sched_run();
    g_mock_time_us = first_done_us + DALI_FORWARD_INTERFRAME_US;
    g_mock_tick_ms += (DALI_FORWARD_INTERFRAME_US + 999u) / 1000u;
    g_mock_tx_advance_on_count = 2;
    g_mock_tx_advance_us = DALI_SEND_TWICE_WINDOW_US -
                           DALI_FORWARD_INTERFRAME_US + 1u;
    dali_sched_run();

    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL(2, g_trace_count);
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMING, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_TX, dali_sched_state());

    const uint32_t second_done_us = first_done_us +
                                    DALI_SEND_TWICE_WINDOW_US + 1u;
    g_mock_time_us = second_done_us + DALI_FORWARD_INTERFRAME_US - 1u;
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    g_mock_time_us = second_done_us + DALI_FORWARD_INTERFRAME_US;
    dali_sched_run();
    TEST_ASSERT_EQUAL(3, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x2222u, g_mock_last_tx.data);
}

void test_send_twice_phy_error_takes_precedence_over_late_completion(void)
{
    const uint32_t first_done_us = 5000u;
    DaliTransaction txn = {
        .frame = { .data = 0xFF00u, .bit_length = 16u },
        .send_twice = true,
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    g_mock_time_us = first_done_us;
    g_mock_tick_ms = first_done_us / 1000u;
    dali_sched_run();
    g_mock_time_us = first_done_us + DALI_FORWARD_INTERFRAME_US;
    g_mock_tick_ms += (DALI_FORWARD_INTERFRAME_US + 999u) / 1000u;
    g_mock_tx_fail_on_count = 2;
    g_mock_tx_result = DALI_ERR_BUS_STUCK;
    g_mock_tx_advance_on_count = 2;
    g_mock_tx_advance_us = DALI_SEND_TWICE_WINDOW_US -
                           DALI_FORWARD_INTERFRAME_US + 1u;
    dali_sched_run();

    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_phy_error_arms_gap_before_following_transaction(void)
{
    const uint32_t failed_tx_duration_us = 4000u;
    DaliTransaction first = {
        .frame = { .data = 0x1111u, .bit_length = 16u },
        .on_complete = on_complete,
    };
    DaliTransaction second = {
        .frame = { .data = 0x2222u, .bit_length = 16u },
    };
    g_mock_tx_result = DALI_ERR_BUS_STUCK;
    g_mock_tx_advance_on_count = 1;
    g_mock_tx_advance_us = failed_tx_duration_us;
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&first));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&second));

    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_TX, dali_sched_state());

    g_mock_tx_result = DALI_OK;
    g_mock_time_us = failed_tx_duration_us +
                     DALI_FORWARD_INTERFRAME_US - 1u;
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    g_mock_time_us++;
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x2222u, g_mock_last_tx.data);
}

void test_send_twice_retry_starts_a_fresh_window(void)
{
    DaliTransaction txn = {
        .frame = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply = true,
        .send_twice = true,
        .retries_left = 1u,
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_to_next_forward();
    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);

    /* Delay well beyond the original pair's deadline before retrying. */
    advance_time_ms(DALI_SEND_TWICE_WINDOW_MS + 1u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(3, g_mock_tx_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.tx_retries);

    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL(4, g_mock_tx_count);
    advance_past_settle();
    dali_sched_run();
    inject_reply(0xAFu, 8u);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, g_cb_reply.data);
}

void test_sequence_reports_send_twice_timing_failure(void)
{
    DaliSequence seq = {
        .steps = {
            { .frame = { .data = 0x1111u, .bit_length = 16u } },
            {
                .frame = { .data = 0x2222u, .bit_length = 16u },
                .send_twice = true,
            },
            { .frame = { .data = 0x3333u, .bit_length = 16u } },
        },
        .step_count = 3u,
        .on_complete = on_sequence_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&seq));

    dali_sched_run();
    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x2222u, g_mock_last_tx.data);

    g_mock_time_us += DALI_SEND_TWICE_WINDOW_US + 1u;
    g_mock_tick_ms += DALI_SEND_TWICE_WINDOW_MS + 1u;
    dali_sched_run();

    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL(1, g_seq_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMING, g_seq_result);
    TEST_ASSERT_EQUAL_UINT8(1u, g_seq_failed_step);
    TEST_ASSERT_FALSE(g_seq_has_reply);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_reset_preserves_active_forward_gap(void)
{
    DaliTransaction first = {
        .frame = { .data = 0x1111u, .bit_length = 16u },
    };
    DaliTransaction second = {
        .frame = { .data = 0x2222u, .bit_length = 16u },
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&first));
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_reset());
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&second));
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(SCHED_TX, dali_sched_state());

    g_mock_time_us = DALI_FORWARD_INTERFRAME_US - 1u;
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    g_mock_time_us++;
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x2222u, g_mock_last_tx.data);
}

void test_coarse_clock_rejects_ambiguous_100ms_repeat(void)
{
    DaliSchedOps coarse_ops = {
        .tx = mock_tx,
        .set_rx_callback = mock_set_rx_callback,
        .get_tick_ms = mock_get_tick_ms,
        .get_time_us = NULL,
    };
    DaliTransaction txn = {
        .frame = { .data = 0xFF00u, .bit_length = 16u },
        .send_twice = true,
        .on_complete = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_init(&coarse_ops));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    g_mock_tick_ms = DALI_SEND_TWICE_WINDOW_MS;
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMING, g_cb_result);
}

void test_seven_step_sequence_uses_one_queue_entry(void)
{
    DaliTransaction txn = {
        .frame = { .data = 0x0080u, .bit_length = 16u },
    };
    DaliSequence seq = { .step_count = 7u };
    for (uint8_t i = 0u; i < seq.step_count; i++) {
        seq.steps[i].frame = (DaliFrame){
            .data = (uint32_t)(0x010000u + i),
            .bit_length = 24u,
        };
    }

    for (uint8_t i = 0u; i < DALI_CMD_QUEUE_SIZE - 1u; i++) {
        TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    }
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&seq));
    TEST_ASSERT_EQUAL(DALI_ERR_QUEUE_FULL, dali_sched_enqueue(&txn));

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_reset());
    for (uint8_t i = 0u; i < DALI_CMD_QUEUE_SIZE; i++) {
        TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    }
    TEST_ASSERT_EQUAL(DALI_ERR_QUEUE_FULL, dali_sched_enqueue_sequence(&seq));
}

void test_sequence_aborts_later_steps_on_tx_error(void)
{
    DaliSequence seq = {
        .steps = {
            {
                .frame = { .data = 0x1111u, .bit_length = 16u },
                .needs_reply = false,
                .send_twice = false,
                .retries_left = 0u,
            },
            {
                .frame = { .data = 0x2222u, .bit_length = 16u },
                .needs_reply = false,
                .send_twice = false,
                .retries_left = 0u,
            },
            {
                .frame = { .data = 0x3333u, .bit_length = 16u },
                .needs_reply = false,
                .send_twice = false,
                .retries_left = 0u,
            },
        },
        .step_count = 3u,
        .on_complete = on_sequence_complete,
    };

    g_mock_tx_fail_on_count = 2;
    g_mock_tx_result = DALI_ERR_BUS_STUCK;

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&seq));

    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x1111u, g_mock_last_tx.data);

    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x2222u, g_mock_last_tx.data);
    TEST_ASSERT_EQUAL(1, g_seq_count);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, g_seq_result);
    TEST_ASSERT_EQUAL_UINT8(1u, g_seq_failed_step);
    TEST_ASSERT_FALSE(g_seq_has_reply);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());

    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
}

void test_sequence_keeps_last_reply_for_completion_callback(void)
{
    DaliSequence seq = {
        .steps = {
            {
                .frame = { .data = 0x0B90u, .bit_length = 16u },
                .needs_reply = true,
                .send_twice = false,
                .retries_left = 0u,
            },
            {
                .frame = { .data = 0xA312u, .bit_length = 16u },
                .needs_reply = false,
                .send_twice = false,
                .retries_left = 0u,
            },
        },
        .step_count = 2u,
        .on_complete = on_sequence_complete,
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&seq));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    inject_reply(0x5Au, 8u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(SCHED_TX, dali_sched_state());

    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0xA312u, g_mock_last_tx.data);
    TEST_ASSERT_EQUAL(0, g_seq_count);

    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_seq_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_seq_result);
    TEST_ASSERT_TRUE(g_seq_has_reply);
    TEST_ASSERT_EQUAL_HEX32(0x5Au, g_seq_reply.data);
    TEST_ASSERT_EQUAL_UINT8(8u, g_seq_reply.bit_length);
}

void test_sequence_captures_a_reply_per_step(void)
{
    DaliSequence seq = {
        .steps = {
            {
                .frame = { .data = 0x0B90u, .bit_length = 16u },
                .needs_reply = true,
            },
            {
                .frame = { .data = 0xA312u, .bit_length = 16u },
                .needs_reply = false,
            },
            {
                .frame = { .data = 0x0BA0u, .bit_length = 16u },
                .needs_reply = true,
            },
        },
        .step_count = 3u,
        .on_complete = on_sequence_complete,
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&seq));

    dali_sched_run();                 /* step 0 transmitted */
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());
    inject_reply(0xAAu, 8u);
    dali_sched_run();

    advance_to_next_forward();
    dali_sched_run();                 /* step 1 transmitted, wants no reply */
    TEST_ASSERT_EQUAL_HEX32(0xA312u, g_mock_last_tx.data);
    advance_past_settle();
    dali_sched_run();

    advance_to_next_forward();
    dali_sched_run();                 /* step 2 transmitted */
    TEST_ASSERT_EQUAL_HEX32(0x0BA0u, g_mock_last_tx.data);
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());
    inject_reply(0xBBu, 8u);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_seq_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_seq_result);
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_NO_FAILED_STEP, g_seq_full.failed_step);
    TEST_ASSERT_EQUAL_UINT8(3u, g_seq_full.steps_run);
    TEST_ASSERT_EQUAL_HEX8(0x05u, g_seq_full.reply_mask);

    DaliFrame frame = {0u, 0u};
    TEST_ASSERT_TRUE(dali_sequence_result_reply(&g_seq_full, 0u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0xAAu, frame.data);
    TEST_ASSERT_EQUAL_UINT8(8u, frame.bit_length);
    TEST_ASSERT_FALSE(dali_sequence_result_reply(&g_seq_full, 1u, &frame));
    TEST_ASSERT_TRUE(dali_sequence_result_reply(&g_seq_full, 2u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0xBBu, frame.data);

    TEST_ASSERT_TRUE(dali_sequence_result_last_reply(&g_seq_full, &frame));
    TEST_ASSERT_EQUAL_HEX32(0xBBu, frame.data);
}

void test_sequence_keeps_earlier_replies_when_a_later_step_fails(void)
{
    DaliSequence seq = {
        .steps = {
            {
                .frame = { .data = 0x0B90u, .bit_length = 16u },
                .needs_reply = true,
            },
            {
                .frame = { .data = 0xA312u, .bit_length = 16u },
                .needs_reply = false,
            },
            {
                .frame = { .data = 0x0BA0u, .bit_length = 16u },
                .needs_reply = true,
            },
        },
        .step_count = 3u,
        .on_complete = on_sequence_complete,
    };

    g_mock_tx_fail_on_count = 2;
    g_mock_tx_result = DALI_ERR_BUS_STUCK;

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&seq));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());
    inject_reply(0x77u, 8u);
    dali_sched_run();

    advance_to_next_forward();
    dali_sched_run();                 /* step 1 transmit fails */

    TEST_ASSERT_EQUAL(1, g_seq_count);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, g_seq_result);
    TEST_ASSERT_EQUAL_UINT8(1u, g_seq_full.failed_step);
    TEST_ASSERT_EQUAL_UINT8(2u, g_seq_full.steps_run);
    TEST_ASSERT_EQUAL_HEX8(0x01u, g_seq_full.reply_mask);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());

    /* The reply gathered before the failure must survive the abort. */
    DaliFrame frame = {0u, 0u};
    TEST_ASSERT_TRUE(dali_sequence_result_reply(&g_seq_full, 0u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x77u, frame.data);
    TEST_ASSERT_FALSE(dali_sequence_result_reply(&g_seq_full, 2u, &frame));
    TEST_ASSERT_TRUE(g_seq_has_reply);
    TEST_ASSERT_EQUAL_HEX32(0x77u, g_seq_reply.data);
}

void test_sequence_result_accessors_reject_missing_replies(void)
{
    DaliSequenceResult result = {
        .result      = DALI_OK,
        .failed_step = DALI_SEQUENCE_NO_FAILED_STEP,
        .steps_run   = 2u,
        .reply_mask  = 0x02u,
    };
    result.replies[1] = (DaliFrame){ .data = 0x42u, .bit_length = 8u };

    DaliFrame frame = {0u, 0u};
    TEST_ASSERT_FALSE(dali_sequence_result_reply(NULL, 1u, &frame));
    TEST_ASSERT_FALSE(dali_sequence_result_reply(&result, 1u, NULL));
    TEST_ASSERT_FALSE(dali_sequence_result_reply(&result, 0u, &frame));
    TEST_ASSERT_FALSE(
        dali_sequence_result_reply(&result, DALI_SEQUENCE_MAX_STEPS, &frame));
    TEST_ASSERT_TRUE(dali_sequence_result_reply(&result, 1u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x42u, frame.data);

    TEST_ASSERT_TRUE(dali_sequence_result_last_reply(&result, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x42u, frame.data);
    TEST_ASSERT_FALSE(dali_sequence_result_last_reply(NULL, &frame));
    TEST_ASSERT_FALSE(dali_sequence_result_last_reply(&result, NULL));

    DaliSequenceResult empty = { .failed_step = DALI_SEQUENCE_NO_FAILED_STEP };
    TEST_ASSERT_FALSE(dali_sequence_result_last_reply(&empty, &frame));
}

void test_sequence_rejects_invalid_args(void)
{
    DaliSequence seq = {0};

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_sched_enqueue_sequence(NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_sched_enqueue_sequence(&seq));

    seq.step_count = (uint8_t)(DALI_SEQUENCE_MAX_STEPS + 1u);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_sched_enqueue_sequence(&seq));

    seq.step_count = 1u;
    seq.steps[0].frame = (DaliFrame){ .data = 0x1234u, .bit_length = 0u };
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_sched_enqueue_sequence(&seq));

    seq.steps[0].frame = (DaliFrame){ .data = 0x1234567u, .bit_length = 25u };
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_sched_enqueue_sequence(&seq));
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
    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_HEX32(2u, g_mock_last_tx.data);

    /* Settle(2) → TX(3) */
    advance_to_next_forward();
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
    RUN_TEST(test_next_forward_waits_for_minimum_gap);
    RUN_TEST(test_forward_gap_handles_microsecond_clock_wrap);
    RUN_TEST(test_forward_gap_does_not_reappear_after_full_us_wrap);
    RUN_TEST(test_send_twice_accepts_exact_100ms_window);
    RUN_TEST(test_send_twice_exact_window_handles_microsecond_clock_wrap);
    RUN_TEST(test_send_twice_rejects_repeat_start_at_deadline);
    RUN_TEST(test_send_twice_rejects_late_repeat_before_phy);
    RUN_TEST(test_send_twice_rejects_delay_after_microsecond_clock_wrap);
    RUN_TEST(test_send_twice_detects_phy_crossing_deadline);
    RUN_TEST(test_send_twice_phy_error_takes_precedence_over_late_completion);
    RUN_TEST(test_phy_error_arms_gap_before_following_transaction);
    RUN_TEST(test_send_twice_retry_starts_a_fresh_window);
    RUN_TEST(test_sequence_reports_send_twice_timing_failure);
    RUN_TEST(test_reset_preserves_active_forward_gap);
    RUN_TEST(test_coarse_clock_rejects_ambiguous_100ms_repeat);
    RUN_TEST(test_reply_received);
    RUN_TEST(test_stray_rx_while_idle_is_ignored);
    RUN_TEST(test_stale_duplicate_rx_does_not_overwrite_latched_reply);
    RUN_TEST(test_reply_accepted_when_run_delayed_past_window);
    RUN_TEST(test_late_rx_after_reply_timeout_is_ignored);
    RUN_TEST(test_unsolicited_24bit_idle_routes_event);
    RUN_TEST(test_unsolicited_16bit_idle_routes_event);
    RUN_TEST(test_24bit_rx_during_settle_is_ignored_not_routed);
    RUN_TEST(test_unsolicited_24bit_during_reply_window_routes_without_completing);
    RUN_TEST(test_16bit_frame_during_reply_window_is_ignored_not_reply);
    RUN_TEST(test_reply_timeout_then_retry_succeeds);
    RUN_TEST(test_reply_timeout_exhausted);
    RUN_TEST(test_sequence_runs_all_steps_before_next_queue_entry);
    RUN_TEST(test_seven_step_sequence_stays_contiguous_before_next_queue_entry);
    RUN_TEST(test_seven_step_sequence_uses_one_queue_entry);
    RUN_TEST(test_sequence_aborts_later_steps_on_tx_error);
    RUN_TEST(test_sequence_keeps_last_reply_for_completion_callback);
    RUN_TEST(test_sequence_captures_a_reply_per_step);
    RUN_TEST(test_sequence_keeps_earlier_replies_when_a_later_step_fails);
    RUN_TEST(test_sequence_result_accessors_reject_missing_replies);
    RUN_TEST(test_sequence_rejects_invalid_args);
    RUN_TEST(test_queue_full);
    RUN_TEST(test_reset_clears_state);
    RUN_TEST(test_multiple_transactions_in_order);

    return UNITY_END();
}
