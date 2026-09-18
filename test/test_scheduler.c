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
static DaliError       g_mock_last_tx_end_result;
static uint32_t        g_mock_last_tx_end_us;

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

static DaliError mock_get_last_tx_end_us(uint32_t *timestamp_out)
{
    if (timestamp_out == NULL || g_mock_last_tx_end_result != DALI_OK) {
        return g_mock_last_tx_end_result;
    }
    *timestamp_out = g_mock_last_tx_end_us;
    return DALI_OK;
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
/* Second observer of each stream, for the subscriber fan-out tests. */
static DaliFrame  g_event2_frame;
static int        g_event2_count;
static void      *g_event2_ctx;
static uint8_t    g_event2_marker;
static int        g_trace2_count;
static void      *g_trace2_ctx;
static uint8_t    g_trace2_marker;
static DaliError  g_seq_result;
static uint8_t    g_seq_failed_step;
static DaliFrame  g_seq_reply;
static bool       g_seq_has_reply;
static DaliSequenceResult g_seq_full;
static int        g_seq_count;
static void      *g_seq_ctx;
static uint8_t    g_seq_marker;
static uint8_t    g_reset_callback_order;

typedef struct {
    int       calls;
    DaliError result;
    bool      had_reply;
    uint8_t   order;
} ResetTxnCapture;

typedef struct {
    int                calls;
    DaliSequenceResult result;
    uint8_t            order;
} ResetSequenceCapture;

typedef struct {
    int       calls;
    bool      pending_during_callback;
    DaliError enqueue_result;
    uint8_t   order;
} ResetDoneCapture;

typedef struct {
    ResetTxnCapture  completion;
    ResetDoneCapture reset;
    DaliError        request_result;
} ResetFromCompletionCapture;

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

static void on_event2(const DaliFrame *frame, void *ctx)
{
    if (frame != NULL) {
        g_event2_frame = *frame;
    } else {
        memset(&g_event2_frame, 0, sizeof(g_event2_frame));
    }
    g_event2_ctx = ctx;
    g_event2_count++;
}

static void on_trace2(const DaliSchedTraceEvent *event, void *ctx)
{
    (void)event;
    g_trace2_ctx = ctx;
    g_trace2_count++;
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

static void on_reset_transaction(DaliError result,
                                 const DaliFrame *reply,
                                 void *ctx)
{
    ResetTxnCapture *capture = (ResetTxnCapture *)ctx;
    capture->calls++;
    capture->result = result;
    capture->had_reply = reply != NULL;
    capture->order = ++g_reset_callback_order;
}

static void on_reset_sequence(const DaliSequenceResult *result, void *ctx)
{
    ResetSequenceCapture *capture = (ResetSequenceCapture *)ctx;
    capture->calls++;
    if (result != NULL) {
        capture->result = *result;
    } else {
        memset(&capture->result, 0, sizeof(capture->result));
        capture->result.result = DALI_ERR_INVALID;
    }
    capture->order = ++g_reset_callback_order;
}

static void on_reset_done(void *ctx)
{
    ResetDoneCapture *capture = (ResetDoneCapture *)ctx;
    DaliTransaction txn = {
        .frame = { .data = 0x7777u, .bit_length = 16u },
    };
    capture->calls++;
    capture->pending_during_callback = dali_sched_reset_pending();
    capture->enqueue_result = dali_sched_enqueue(&txn);
    capture->order = ++g_reset_callback_order;
}

static void on_complete_request_reset(DaliError result,
                                      const DaliFrame *reply,
                                      void *ctx)
{
    ResetFromCompletionCapture *capture =
        (ResetFromCompletionCapture *)ctx;
    on_reset_transaction(result, reply, &capture->completion);
    capture->request_result =
        dali_sched_request_reset(on_reset_done, &capture->reset);
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

/* Past the hold-off a reply timeout arms before the frame may be retransmitted. */
static void advance_past_retry_backoff(void)
{
    g_mock_tick_ms += (DALI_REPLY_TIMEOUT_BACKOFF_US + 999u) / 1000u;
    g_mock_time_us += DALI_REPLY_TIMEOUT_BACKOFF_US;
}

static void inject_reply(uint32_t data, uint8_t bits)
{
    DaliFrame f = { .data = data, .bit_length = bits };
    dali_sched_notify_rx(&f);
}

static void inject_rx_activity(uint32_t first_edge_us,
                               uint32_t last_edge_us,
                               uint8_t edge_count,
                               DaliError result)
{
    DaliPhyRxObservation observation = {
        .result = result,
        .first_edge_us = first_edge_us,
        .last_edge_us = last_edge_us,
        .edge_count = edge_count,
        .has_timestamps = true,
    };
    dali_sched_notify_rx_observation(&observation);
}

static void inject_phy_observation(const DaliPhyRxObservation *observation)
{
    TEST_ASSERT_NOT_NULL(g_mock_rx_cb);
    g_mock_rx_cb(observation, g_mock_rx_ctx);
}

static void inject_event(uint32_t data)
{
    inject_reply(data, 24u);
}

static void inject_legacy_event(uint32_t data)
{
    inject_reply(data, 16u);
}

/*
 * Every ignored observation increments exactly one bucket and the aggregate,
 * so the sum is an invariant and not a convention. Worth asserting next to the
 * per-bucket numbers because the aggregate is what the shell, the integration
 * and every older vector still read.
 */
static void assert_ignored_buckets_sum(void)
{
    uint32_t sum = g_dali_stats.rx_reply_early +
                   g_dali_stats.rx_reply_late +
                   g_dali_stats.rx_reply_superseded +
                   g_dali_stats.rx_event_unroutable +
                   g_dali_stats.rx_event_no_subscriber +
                   g_dali_stats.rx_undecodable_ignored +
                   g_dali_stats.rx_ignored_unclassified;
    TEST_ASSERT_EQUAL_UINT32(g_dali_stats.rx_ignored_outside_reply, sum);
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
    g_mock_last_tx_end_result = DALI_ERR_INVALID;
    g_mock_last_tx_end_us = 0u;
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
    g_event2_count = 0;
    g_event2_ctx   = NULL;
    memset(&g_event2_frame, 0, sizeof(g_event2_frame));
    g_trace2_count = 0;
    g_trace2_ctx   = NULL;
    g_seq_result = DALI_OK;
    g_seq_failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
    g_seq_has_reply = false;
    g_seq_count = 0;
    g_seq_ctx = NULL;
    memset(&g_seq_reply, 0, sizeof(g_seq_reply));
    memset(&g_seq_full, 0, sizeof(g_seq_full));
    g_reset_callback_order = 0u;

    memset(&g_dali_stats, 0, sizeof(g_dali_stats));

    DaliSchedOps ops = {
        .tx              = mock_tx,
        .set_rx_callback = mock_set_rx_callback,
        .get_tick_ms     = mock_get_tick_ms,
        .get_time_us     = mock_get_time_us,
        .get_last_tx_end_us = mock_get_last_tx_end_us,
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

/*
 * An RX frame whose edges predate the last TX is ordinary, not a fault: edges
 * are stamped in the ISR and decoded in a task, so the next transmission can
 * complete first. Observed on the 2k bus, where a Steinel event frame decoded
 * 24.7 ms "before" the TX that had already gone out reported since_tx_us as
 * 4294942572 — about 71 minutes, and indistinguishable from a real reading.
 */
void test_trace_rx_stamped_before_last_tx_reports_no_offset(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_trace_callback(on_trace, &g_trace_marker));

    g_mock_time_us = 200000u;
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

    /* Decoded 24.7 ms before the TX above, handed over after it. */
    DaliPhyRxObservation observation = {
        .result         = DALI_OK,
        .frame          = { .data = 0x00840Fu,
                            .bit_length = DALI_EXTENDED_FRAME_BITS },
        .first_edge_us  = 175300u - DALI_BIT_US * 24u,
        .last_edge_us   = 175300u,
        .edge_count     = 48u,
        .has_timestamps = true,
    };
    dali_sched_notify_rx_observation(&observation);

    TEST_ASSERT_EQUAL(2, g_trace_count);
    TEST_ASSERT_EQUAL(DALI_SCHED_TRACE_RX, g_trace_event.direction);
    TEST_ASSERT_EQUAL_HEX32(0x00840Fu, g_trace_event.frame.data);
    TEST_ASSERT_EQUAL_UINT32(175300u, g_trace_event.timestamp_us);
    TEST_ASSERT_FALSE(g_trace_event.has_since_tx);
    TEST_ASSERT_EQUAL_UINT32(0u, g_trace_event.since_tx_us);
}

/* A frame stamped exactly at the TX end still reports an offset, of zero. */
void test_trace_rx_stamped_at_last_tx_reports_zero_offset(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_trace_callback(on_trace, &g_trace_marker));

    g_mock_time_us = 200000u;
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = false,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    dali_sched_run();

    DaliPhyRxObservation observation = {
        .result         = DALI_OK,
        .frame          = { .data = 0x00840Fu,
                            .bit_length = DALI_EXTENDED_FRAME_BITS },
        .first_edge_us  = 200000u - DALI_BIT_US * 24u,
        .last_edge_us   = 200000u,
        .edge_count     = 48u,
        .has_timestamps = true,
    };
    dali_sched_notify_rx_observation(&observation);

    TEST_ASSERT_EQUAL(2, g_trace_count);
    TEST_ASSERT_TRUE(g_trace_event.has_since_tx);
    TEST_ASSERT_EQUAL_UINT32(0u, g_trace_event.since_tx_us);
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

void test_frame_like_rx_activity_completes_reply_with_distinct_error(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .retries_left = 2u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    inject_rx_activity(DALI_REPLY_WINDOW_OPEN_US,
                       DALI_REPLY_WINDOW_OPEN_US +
                           DALI_BACKWARD_ACTIVITY_MIN_SPAN_US,
                       16u,
                       DALI_ERR_MALFORMED);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY, g_cb_result);
    TEST_ASSERT_EQUAL_UINT8(0u, g_cb_reply.bit_length);
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.reply_rx_activity);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_timeouts);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.tx_retries);
}

/*
 * The reply window opens at the minimum forward-to-backward settling time, not
 * the 7 ms nominal. Gear answering at the fast end of the range must be
 * attributed to its own transaction; raising the opening back towards the
 * nominal would silently time these replies out.
 */
void test_reply_between_minimum_and_nominal_settling_is_accepted(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    TEST_ASSERT_EQUAL_UINT32(5500u, (uint32_t)DALI_REPLY_WINDOW_OPEN_US);

    /* 6 ms: inside the settling range, below the 7 ms nominal. */
    DaliPhyRxObservation observation = {
        .result         = DALI_OK,
        .frame          = { .data = 0xAFu, .bit_length = 8u },
        .first_edge_us  = 6000u,
        .last_edge_us   = 6000u + DALI_BACKWARD_ACTIVITY_MIN_SPAN_US,
        .edge_count     = 16u,
        .has_timestamps = true,
    };
    dali_sched_notify_rx_observation(&observation);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, g_cb_reply.data);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_ignored_outside_reply);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_timeouts);
}

/*
 * Gear that settles faster than the standard's 5.5 ms minimum still gets read,
 * as long as what arrived decoded as a complete backward frame.
 *
 * Measured on a real installation: four LED-strip drivers reply at 5.24-5.62 ms
 * and straddle the edge, so most of their replies were discarded and the caller
 * saw a timeout while a correctly decoded byte sat in the buffer. Retries do
 * not help gear sitting on a threshold, so the window has to give.
 */
void test_decoded_backward_frame_is_accepted_before_the_standard_open(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x1BC0u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    /* The edge is the self-echo suppression itself, not a chosen margin above
     * it: for a decoded frame there is no other floor with a physical meaning,
     * and every fixed margin so far has been overtaken by the next device. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)DALI_SETTLE_MS * 1000u,
                             (uint32_t)DALI_REPLY_WINDOW_OPEN_DECODED_US);
    TEST_ASSERT_TRUE(DALI_REPLY_WINDOW_OPEN_DECODED_US < DALI_REPLY_WINDOW_OPEN_US);

    /* 4.122 ms — the fastest reply actually observed on the 1k bus, from the
     * a0 DT6 driver answering QUERY STATUS. 25% faster than the standard's
     * minimum, and it decoded correctly. */
    DaliPhyRxObservation observation = {
        .result         = DALI_OK,
        .frame          = { .data = 0x08u, .bit_length = 8u },
        .first_edge_us  = 4122u,
        .last_edge_us   = 4122u + DALI_BACKWARD_ACTIVITY_MIN_SPAN_US,
        .edge_count     = 16u,
        .has_timestamps = true,
    };
    dali_sched_notify_rx_observation(&observation);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0x08u, g_cb_reply.data);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_ignored_outside_reply);
}

/*
 * The relaxation is for decoded frames only. Undecodable activity keeps the
 * standard's edge, because that is the path COMPARE reads as YES: accepting it
 * early would invent gear that is not there, which is the one error a
 * commissioning walk must not make.
 */
void test_malformed_activity_keeps_the_standard_open_edge(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();

    /* Same 4.122 ms the decoded frame above is accepted at. */
    inject_rx_activity(4122u,
                       4122u + DALI_BACKWARD_ACTIVITY_MIN_SPAN_US,
                       16u,
                       DALI_ERR_MALFORMED);

    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_rx_activity);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_undecodable_ignored);
    assert_ignored_buckets_sum();

    advance_time_ms(DALI_REPLY_TIMEOUT_MS);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, g_cb_result);
}

void test_early_and_late_malformed_activity_do_not_become_replies(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();

    inject_rx_activity(DALI_REPLY_WINDOW_OPEN_US - 2u,
                       DALI_REPLY_WINDOW_OPEN_US - 2u +
                           DALI_BACKWARD_ACTIVITY_MIN_SPAN_US,
                       16u,
                       DALI_ERR_MALFORMED);
    inject_rx_activity(DALI_REPLY_WINDOW_CLOSE_US + 2u,
                       DALI_REPLY_WINDOW_CLOSE_US + 2u +
                           DALI_BACKWARD_ACTIVITY_MIN_SPAN_US,
                       16u,
                       DALI_ERR_MALFORMED);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_rx_activity);
    TEST_ASSERT_EQUAL_UINT32(2u, g_dali_stats.rx_ignored_outside_reply);
    /* Both are undecodable waveforms. Neither is a statement about reply
     * timing, which is the distinction the single counter could not make. */
    TEST_ASSERT_EQUAL_UINT32(2u, g_dali_stats.rx_undecodable_ignored);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_reply_early);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_reply_late);
    assert_ignored_buckets_sum();

    advance_time_ms(DALI_REPLY_TIMEOUT_MS);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, g_cb_result);
}

void test_in_window_short_malformed_activity_aborts_instead_of_becoming_no(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    inject_rx_activity(DALI_REPLY_WINDOW_OPEN_US + 1000u,
                       DALI_REPLY_WINDOW_OPEN_US + 1200u,
                       2u,
                       DALI_ERR_MALFORMED);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED, g_cb_result);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_rx_activity);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_timeouts);
}

void test_in_window_ring_overflow_is_preserved_as_overflow(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    inject_rx_activity(DALI_REPLY_WINDOW_OPEN_US + 1000u,
                       DALI_REPLY_WINDOW_OPEN_US + 1000u,
                       DALI_PHY_RXDEBUG_MAX_EDGES,
                       DALI_ERR_OVERFLOW);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_OVERFLOW, g_cb_result);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_rx_activity);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_timeouts);
}

/*
 * A reply-window error is not automatically the end of the step.
 *
 * MALFORMED is one waveform the decoder could not read, and OVERFLOW is an
 * event dropped because the RX ring filled — a local resource fault, not a fact
 * about the bus. Neither carries meaning, and both are what a retry budget
 * exists to survive. Before this split, either one ended the step outright, and
 * with it the sequence that owned it.
 *
 * These anchor their injected timestamps on the microsecond clock reading at
 * each TX. The default mock reports no precise TX end, so that reading is what
 * the scheduler uses to open the reply window — and it moves on a retry, unlike
 * the untimestamped inject_reply() the older retry tests use.
 */
void test_in_window_malformed_spends_a_retry_and_second_attempt_succeeds(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .retries_left = 1u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    uint32_t tx1_us = g_mock_time_us;
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    /* Short/ambiguous activity: stays MALFORMED rather than being promoted to
     * RX_ACTIVITY, so it is a candidate for the retry. */
    inject_rx_activity(tx1_us + DALI_REPLY_WINDOW_OPEN_US + 1000u,
                       tx1_us + DALI_REPLY_WINDOW_OPEN_US + 1200u,
                       2u,
                       DALI_ERR_MALFORMED);
    dali_sched_run();

    /* The retry is booked but held behind the same backoff a timeout uses: a
     * straggling reply may still be on the wire. */
    TEST_ASSERT_EQUAL(0, g_cb_count);
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.tx_retries);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_timeouts);

    advance_past_retry_backoff();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);

    advance_past_settle();
    dali_sched_run();
    inject_reply(0xAFu, 8u);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, g_cb_reply.data);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_in_window_overflow_spends_a_retry(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .retries_left = 1u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    uint32_t tx1_us = g_mock_time_us;
    advance_past_settle();
    dali_sched_run();

    inject_rx_activity(tx1_us + DALI_REPLY_WINDOW_OPEN_US + 1000u,
                       tx1_us + DALI_REPLY_WINDOW_OPEN_US + 1000u,
                       DALI_PHY_RXDEBUG_MAX_EDGES,
                       DALI_ERR_OVERFLOW);
    dali_sched_run();

    TEST_ASSERT_EQUAL(0, g_cb_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.tx_retries);

    advance_past_retry_backoff();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
}

/* The budget is finite: a bus that keeps producing noise still terminates, and
 * reports the error rather than a timeout. */
void test_in_window_malformed_reports_error_once_the_budget_is_gone(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .retries_left = 1u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    uint32_t tx1_us = g_mock_time_us;
    advance_past_settle();
    dali_sched_run();
    inject_rx_activity(tx1_us + DALI_REPLY_WINDOW_OPEN_US + 1000u,
                       tx1_us + DALI_REPLY_WINDOW_OPEN_US + 1200u,
                       2u,
                       DALI_ERR_MALFORMED);
    dali_sched_run();

    advance_past_retry_backoff();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    uint32_t tx2_us = g_mock_time_us;
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    /* Same noise on the second attempt, with nothing left to spend. */
    inject_rx_activity(tx2_us + DALI_REPLY_WINDOW_OPEN_US + 1000u,
                       tx2_us + DALI_REPLY_WINDOW_OPEN_US + 1200u,
                       2u,
                       DALI_ERR_MALFORMED);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED, g_cb_result);
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.tx_retries);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_timestamped_activity_survives_delayed_wait_settle_state(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());
    advance_time_ms(8u);
    inject_rx_activity(DALI_REPLY_WINDOW_OPEN_US,
                       DALI_REPLY_WINDOW_OPEN_US +
                           DALI_BACKWARD_ACTIVITY_MIN_SPAN_US,
                       16u,
                       DALI_ERR_MALFORMED);

    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY, g_cb_result);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_timeouts);
}

void test_precise_phy_tx_end_is_preferred_over_delayed_task_clock(void)
{
    g_mock_time_us = 50000u;
    g_mock_last_tx_end_us = 10000u;
    g_mock_last_tx_end_result = DALI_OK;

    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    inject_rx_activity(g_mock_last_tx_end_us + DALI_REPLY_WINDOW_OPEN_US,
                       g_mock_last_tx_end_us + DALI_REPLY_WINDOW_OPEN_US +
                           DALI_BACKWARD_ACTIVITY_MIN_SPAN_US,
                       16u,
                       DALI_ERR_MALFORMED);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY, g_cb_result);
}

void test_timestamped_reply_survives_delayed_wait_settle_state(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());
    advance_time_ms(8u);
    DaliPhyRxObservation observation = {
        .result = DALI_OK,
        .frame = { .data = 0xA5u, .bit_length = DALI_BACKWARD_FRAME_BITS },
        .first_edge_us = DALI_REPLY_WINDOW_OPEN_US,
        .last_edge_us = DALI_REPLY_WINDOW_OPEN_US + DALI_BIT_US * 9u,
        .edge_count = 16u,
        .has_timestamps = true,
    };
    dali_sched_notify_rx_observation(&observation);

    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX8(0xA5u, g_cb_reply.data);
}

void test_timestamped_phy_callback_delivers_normal_backward_reply(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    DaliPhyRxObservation observation = {
        .result = DALI_OK,
        .frame = { .data = 0x5Au, .bit_length = DALI_BACKWARD_FRAME_BITS },
        .first_edge_us = DALI_REPLY_WINDOW_OPEN_US,
        .last_edge_us = DALI_REPLY_WINDOW_OPEN_US + DALI_BIT_US * 9u,
        .edge_count = 16u,
        .has_timestamps = true,
    };
    inject_phy_observation(&observation);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
    TEST_ASSERT_EQUAL_HEX8(0x5Au, g_cb_reply.data);
}

/*
 * The open edge a decoded backward frame is held to is
 * DALI_REPLY_WINDOW_OPEN_DECODED_US, not the standard's
 * DALI_REPLY_WINDOW_OPEN_US — see the constant for why the two differ. This
 * test is about frames outside the window at all, so it uses the edge that
 * actually applies to a decoded frame.
 */
void test_timestamped_phy_callback_rejects_early_and_late_backward_frames(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    DaliPhyRxObservation observation = {
        .result = DALI_OK,
        .frame = { .data = 0x5Au, .bit_length = DALI_BACKWARD_FRAME_BITS },
        .first_edge_us = DALI_REPLY_WINDOW_OPEN_DECODED_US - 2u,
        .last_edge_us = DALI_REPLY_WINDOW_OPEN_DECODED_US + DALI_BIT_US * 9u,
        .edge_count = 16u,
        .has_timestamps = true,
    };
    inject_phy_observation(&observation);
    observation.first_edge_us = DALI_REPLY_WINDOW_CLOSE_US + 2u;
    observation.last_edge_us =
        observation.first_edge_us + DALI_BIT_US * 9u;
    inject_phy_observation(&observation);

    advance_time_ms(DALI_REPLY_TIMEOUT_MS);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, g_cb_result);
    TEST_ASSERT_EQUAL_UINT32(2u, g_dali_stats.rx_ignored_outside_reply);
    /*
     * One of each, and they mean opposite things: gear that answered before
     * the window opened, and gear that answered after it closed. The aggregate
     * has always read 2 here, which is why a bus reporting "2" could be read
     * as either fault, or as neither.
     */
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_reply_early);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_reply_late);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_ignored_unclassified);
    assert_ignored_buckets_sum();
}

/* 4. Stray RX while idle must not complete the next transaction */
void test_stray_rx_while_idle_is_ignored(void)
{
    inject_reply(0x11u, 8u);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);
    /* Nothing has transmitted yet, so there is no edge to measure this
     * against. Unclassified is the honest answer; guessing "late" here is the
     * kind of inference the split exists to stop. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_unclassified);
    assert_ignored_buckets_sum();

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
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_unclassified);
    assert_ignored_buckets_sum();
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
    /* A duplicate arriving after the reply was latched. Not a timing fault:
     * the first frame was in the window and was accepted. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_reply_superseded);
    assert_ignored_buckets_sum();
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

void test_timestamped_activity_captured_in_window_survives_delayed_run(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    advance_time_ms(DALI_REPLY_TIMEOUT_MS);

    inject_rx_activity(DALI_REPLY_WINDOW_OPEN_US,
                       DALI_REPLY_WINDOW_OPEN_US +
                           DALI_BACKWARD_ACTIVITY_MIN_SPAN_US,
                       16u,
                       DALI_ERR_MALFORMED);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY, g_cb_result);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.reply_timeouts);
}

void test_rx_activity_window_handles_microsecond_clock_wrap(void)
{
    const uint32_t tx_end_us = 0xFFFFF000u;
    g_mock_time_us = tx_end_us;
    g_mock_tick_ms = 1000u;

    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    inject_rx_activity(tx_end_us + DALI_REPLY_WINDOW_OPEN_US,
                       tx_end_us + DALI_REPLY_WINDOW_OPEN_US +
                           DALI_BACKWARD_ACTIVITY_MIN_SPAN_US,
                       16u,
                       DALI_ERR_MALFORMED);
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY, g_cb_result);
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
    /*
     * This frame really is late, but it arrives through the frame-only entry
     * point, which synthesises an observation with no timestamps. With no edge
     * to compare against, the scheduler cannot prove late over early and says
     * so. The timestamped version of exactly this case is 6c below.
     */
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_unclassified);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_reply_late);
    assert_ignored_buckets_sum();
    TEST_ASSERT_EQUAL(1, g_cb_count);   /* no additional callback */
}

/*
 * 6c. The same late reply, timestamped, is nameable.
 *
 * Late and early are the only two buckets that say anything about bus timing,
 * and they point in opposite directions. This is the vector that makes a
 * claim like "gear is answering past the timeout" checkable instead of
 * inferred from a counter that also holds event traffic and noise.
 */
void test_timestamped_late_reply_after_timeout_counts_as_late(void)
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

    advance_time_ms(DALI_REPLY_TIMEOUT_MS);
    dali_sched_run();
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());

    DaliPhyRxObservation observation = {
        .result         = DALI_OK,
        .frame          = { .data = 0xAFu,
                            .bit_length = DALI_BACKWARD_FRAME_BITS },
        .first_edge_us  = DALI_REPLY_WINDOW_CLOSE_US + 2u,
        .last_edge_us   = DALI_REPLY_WINDOW_CLOSE_US + 2u + DALI_BIT_US * 9u,
        .edge_count     = 16u,
        .has_timestamps = true,
    };
    inject_phy_observation(&observation);

    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_reply_late);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_reply_early);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_ignored_unclassified);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);
    assert_ignored_buckets_sum();
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

/* ---------------------------------------------------------------------------
 * Subscriber fan-out
 *
 * The integration's dispatch path and a diagnostic shell session both want the
 * raw event stream at once. These assert that neither displaces the other, and
 * that the counter still answers "did this frame reach the application" rather
 * than counting one delivery per listener.
 * --------------------------------------------------------------------------*/

void test_init_starts_with_no_subscribers(void)
{
    TEST_ASSERT_EQUAL_UINT8(0u, dali_sched_event_subscriber_count());
    TEST_ASSERT_EQUAL_UINT8(0u, dali_sched_trace_subscriber_count());
}

void test_event_reaches_primary_callback_and_added_subscriber(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_event_callback(on_event, &g_event_marker));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_add_event_subscriber(on_event2, &g_event2_marker));
    TEST_ASSERT_EQUAL_UINT8(2u, dali_sched_event_subscriber_count());

    inject_event(0x123456u);

    TEST_ASSERT_EQUAL(1, g_event_count);
    TEST_ASSERT_EQUAL(1, g_event2_count);
    TEST_ASSERT_EQUAL_HEX32(0x123456u, g_event_frame.data);
    TEST_ASSERT_EQUAL_HEX32(0x123456u, g_event2_frame.data);
    TEST_ASSERT_EQUAL_PTR(&g_event_marker, g_event_ctx);
    TEST_ASSERT_EQUAL_PTR(&g_event2_marker, g_event2_ctx);
    /* One frame routed, not one per subscriber. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.unsolicited_events_routed);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_ignored_outside_reply);
}

void test_added_event_subscriber_receives_without_a_primary_callback(void)
{
    /* The shell must still see events on a build where nothing calls the
     * single-callback setter. */
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_add_event_subscriber(on_event2, &g_event2_marker));

    inject_event(0x123456u);

    TEST_ASSERT_EQUAL(0, g_event_count);
    TEST_ASSERT_EQUAL(1, g_event2_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.unsolicited_events_routed);
}

void test_set_event_callback_replaces_instead_of_accumulating(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_event_callback(on_event, &g_event_marker));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_event_callback(on_event, &g_event2_marker));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_sched_event_subscriber_count());

    inject_event(0x123456u);

    TEST_ASSERT_EQUAL(1, g_event_count);
    TEST_ASSERT_EQUAL_PTR(&g_event2_marker, g_event_ctx);
}

void test_set_event_callback_null_releases_only_its_own_slot(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_event_callback(on_event, &g_event_marker));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_add_event_subscriber(on_event2, &g_event2_marker));

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_set_event_callback(NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_sched_event_subscriber_count());

    inject_event(0x123456u);

    TEST_ASSERT_EQUAL(0, g_event_count);
    TEST_ASSERT_EQUAL(1, g_event2_count);
}

void test_event_subscriber_add_is_idempotent(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_add_event_subscriber(on_event2, &g_event2_marker));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_add_event_subscriber(on_event2, &g_event2_marker));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_sched_event_subscriber_count());

    inject_event(0x123456u);

    TEST_ASSERT_EQUAL(1, g_event2_count);
}

void test_event_subscriber_same_callback_different_ctx_is_a_second_slot(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_add_event_subscriber(on_event2, &g_event2_marker));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_add_event_subscriber(on_event2, &g_event_marker));
    TEST_ASSERT_EQUAL_UINT8(2u, dali_sched_event_subscriber_count());

    inject_event(0x123456u);

    TEST_ASSERT_EQUAL(2, g_event2_count);
}

void test_event_subscriber_table_full_is_reported(void)
{
    /* Distinct ctx values, so each add claims a slot instead of deduplicating. */
    static uint8_t markers[DALI_SCHED_MAX_EVENT_SUBSCRIBERS];

    /* Slot 0 is reserved for the primary setter, so the add path owns one
     * fewer slot than the table holds. */
    for (uint8_t i = 0u; i < DALI_SCHED_MAX_EVENT_SUBSCRIBERS - 1u; i++) {
        TEST_ASSERT_EQUAL(DALI_OK,
                          dali_sched_add_event_subscriber(on_event2, &markers[i]));
    }
    TEST_ASSERT_EQUAL(DALI_ERR_FULL,
                      dali_sched_add_event_subscriber(on_event, &g_event_marker));
    /* A full add table does not cost the primary setter its reserved slot. */
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_event_callback(on_event, &g_event_marker));

    inject_event(0x123456u);
    TEST_ASSERT_EQUAL(1, g_event_count);
}

void test_event_subscriber_removal_stops_delivery(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_add_event_subscriber(on_event2, &g_event2_marker));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_remove_event_subscriber(on_event2, &g_event2_marker));
    TEST_ASSERT_EQUAL_UINT8(0u, dali_sched_event_subscriber_count());

    inject_event(0x123456u);

    TEST_ASSERT_EQUAL(0, g_event2_count);
    /* With nothing listening the frame is ignored, not routed. */
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.unsolicited_events_routed);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);
    /* The state would have routed it; there was nobody to route it to. That
     * is a fact about this build, not about the bus. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_event_no_subscriber);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_event_unroutable);
    assert_ignored_buckets_sum();
}

void test_removing_an_unregistered_event_subscriber_is_ok(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_remove_event_subscriber(on_event2, &g_event2_marker));
    TEST_ASSERT_EQUAL_UINT8(0u, dali_sched_event_subscriber_count());
}

void test_event_subscriber_rejects_null_callback(void)
{
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_sched_add_event_subscriber(NULL, &g_event2_marker));
    TEST_ASSERT_EQUAL_UINT8(0u, dali_sched_event_subscriber_count());
}

void test_trace_reaches_primary_callback_and_added_subscriber(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_trace_callback(on_trace, &g_trace_marker));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_add_trace_subscriber(on_trace2, &g_trace2_marker));
    TEST_ASSERT_EQUAL_UINT8(2u, dali_sched_trace_subscriber_count());

    inject_event(0x123456u);

    TEST_ASSERT_EQUAL(1, g_trace_count);
    TEST_ASSERT_EQUAL(1, g_trace2_count);
    TEST_ASSERT_EQUAL_PTR(&g_trace_marker, g_trace_ctx);
    TEST_ASSERT_EQUAL_PTR(&g_trace2_marker, g_trace2_ctx);
    TEST_ASSERT_EQUAL(DALI_SCHED_TRACE_RX, g_trace_event.direction);
}

void test_trace_subscriber_removal_stops_delivery(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_add_trace_subscriber(on_trace2, &g_trace2_marker));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_remove_trace_subscriber(on_trace2, &g_trace2_marker));

    inject_event(0x123456u);

    TEST_ASSERT_EQUAL(0, g_trace2_count);
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
    /*
     * This is the 2k bus in miniature: a subscribed control device emits while
     * the scheduler is mid-transmission, and the frame is dropped for the
     * state it arrived in. A 64-address walk is back-to-back TX, so this is
     * the bucket that made the scan note track whether someone was standing
     * in the corridor. It is event traffic, not a timing fault.
     */
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_event_unroutable);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_reply_early);
    TEST_ASSERT_EQUAL_UINT32(0u, g_dali_stats.rx_reply_late);
    assert_ignored_buckets_sum();

    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, g_cb_result);
}

/* 9. Another 24-bit forward frame invalidates the pending local query. */
void test_unsolicited_24bit_during_reply_window_invalidates_query(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_set_event_callback(on_event, &g_event_marker));

    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 2u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    inject_event(0x123456u);
    /* This backward frame belongs to unknown bus traffic, not our query. */
    inject_reply(0xAFu, 8u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_SCHED_INTERVENED_ERROR, g_cb_result);
    TEST_ASSERT_EQUAL_UINT8(0u, g_cb_reply.bit_length);
    TEST_ASSERT_EQUAL(1, g_event_count);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.unsolicited_events_routed);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);
    /* The event routed; the backward frame behind it belongs to no query the
     * scheduler can still complete, because the event invalidated it. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_reply_superseded);
    assert_ignored_buckets_sum();

    /* Neither a late frame nor another run may complete or retry it again. */
    inject_reply(0x55u, 8u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
}

/* 10. A legacy 16-bit forward frame has the same invalidation semantics. */
void test_16bit_frame_during_reply_window_invalidates_query(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0B90u, .bit_length = 16u },
        .needs_reply  = true,
        .send_twice   = false,
        .retries_left = 2u,
        .on_complete  = on_complete,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));

    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    inject_reply(0x1234u, 16u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_SCHED_INTERVENED_ERROR, g_cb_result);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_ignored_outside_reply);
    /* No event callback is registered in this test, so the intervening 16-bit
     * frame is ignored for want of a subscriber rather than for its state. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.rx_event_no_subscriber);
    assert_ignored_buckets_sum();

    inject_reply(0xAFu, 8u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_cb_count);
    TEST_ASSERT_EQUAL(DALI_SCHED_INTERVENED_ERROR, g_cb_result);
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
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

    /* Timeout books the retry but must NOT put it on the wire yet: gear that
     * answered just past the window is still transmitting, and the retry would
     * land on top of that backward frame. */
    advance_time_ms(DALI_REPLY_TIMEOUT_MS);
    dali_sched_run();
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
    TEST_ASSERT_EQUAL(0, g_cb_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.reply_timeouts);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.tx_retries);

    /* Once the straggler has had its say, TX(2) goes out. */
    advance_past_retry_backoff();
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL(0, g_cb_count);

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

    /* Delay well beyond the original pair's deadline before retrying. The
     * timeout books the retry; the reply-timeout hold-off still applies before
     * the fresh pair may start. */
    advance_time_ms(DALI_SEND_TWICE_WINDOW_MS + 1u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(2, g_mock_tx_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_dali_stats.tx_retries);

    advance_past_retry_backoff();
    dali_sched_run();
    TEST_ASSERT_EQUAL(3, g_mock_tx_count);

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

void test_reset_cancels_active_and_queued_callbacks_once(void)
{
    ResetTxnCapture active_capture = {0};
    ResetTxnCapture queued_capture = {0};
    ResetSequenceCapture sequence_capture = {0};
    ResetDoneCapture done_capture = {0};
    DaliTransaction active = {
        .frame = { .data = 0x1111u, .bit_length = 16u },
        .needs_reply = true,
        .on_complete = on_reset_transaction,
        .cb_ctx = &active_capture,
    };
    DaliTransaction queued = {
        .frame = { .data = 0x2222u, .bit_length = 16u },
        .on_complete = on_reset_transaction,
        .cb_ctx = &queued_capture,
    };
    DaliSequence sequence = {
        .steps = {{
            .frame = { .data = 0x3333u, .bit_length = 16u },
        }},
        .step_count = 1u,
        .on_complete = on_reset_sequence,
        .cb_ctx = &sequence_capture,
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&active));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&queued));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&sequence));
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_request_reset(on_reset_done, &done_capture));
    TEST_ASSERT_TRUE(dali_sched_reset_pending());
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());
    TEST_ASSERT_EQUAL(DALI_ERR_BUSY,
                      dali_sched_request_reset(NULL, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_BUSY, dali_sched_enqueue(&queued));
    TEST_ASSERT_EQUAL(0, active_capture.calls);

    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
    TEST_ASSERT_FALSE(dali_sched_reset_pending());
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);

    TEST_ASSERT_EQUAL(1, active_capture.calls);
    TEST_ASSERT_EQUAL(DALI_SCHED_RESET_ERROR, active_capture.result);
    TEST_ASSERT_FALSE(active_capture.had_reply);
    TEST_ASSERT_EQUAL_UINT8(1u, active_capture.order);
    TEST_ASSERT_EQUAL(1, queued_capture.calls);
    TEST_ASSERT_EQUAL(DALI_SCHED_RESET_ERROR, queued_capture.result);
    TEST_ASSERT_FALSE(queued_capture.had_reply);
    TEST_ASSERT_EQUAL_UINT8(2u, queued_capture.order);

    TEST_ASSERT_EQUAL(1, sequence_capture.calls);
    TEST_ASSERT_EQUAL(DALI_SCHED_RESET_ERROR,
                      sequence_capture.result.result);
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_NO_FAILED_STEP,
                            sequence_capture.result.failed_step);
    TEST_ASSERT_EQUAL_UINT8(0u, sequence_capture.result.steps_run);
    TEST_ASSERT_EQUAL_UINT8(0u, sequence_capture.result.reply_mask);
    TEST_ASSERT_EQUAL_UINT8(3u, sequence_capture.order);

    TEST_ASSERT_EQUAL(1, done_capture.calls);
    TEST_ASSERT_TRUE(done_capture.pending_during_callback);
    TEST_ASSERT_EQUAL(DALI_ERR_BUSY, done_capture.enqueue_result);
    TEST_ASSERT_EQUAL_UINT8(4u, done_capture.order);

    dali_sched_run();
    TEST_ASSERT_EQUAL(1, active_capture.calls);
    TEST_ASSERT_EQUAL(1, queued_capture.calls);
    TEST_ASSERT_EQUAL(1, sequence_capture.calls);
    TEST_ASSERT_EQUAL(1, done_capture.calls);
}

void test_reset_active_sequence_preserves_partial_result(void)
{
    ResetSequenceCapture sequence_capture = {0};
    ResetDoneCapture done_capture = {0};
    DaliSequence sequence = {
        .steps = {
            {
                .frame = { .data = 0x1111u, .bit_length = 16u },
                .needs_reply = true,
            },
            {
                .frame = { .data = 0x2222u, .bit_length = 16u },
            },
        },
        .step_count = 2u,
        .on_complete = on_reset_sequence,
        .cb_ctx = &sequence_capture,
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&sequence));
    dali_sched_run();
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());
    inject_reply(0x5Au, 8u);
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_TX, dali_sched_state());
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_sched_request_reset(on_reset_done, &done_capture));
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, sequence_capture.calls);
    TEST_ASSERT_EQUAL(DALI_SCHED_RESET_ERROR,
                      sequence_capture.result.result);
    TEST_ASSERT_EQUAL_UINT8(DALI_SEQUENCE_NO_FAILED_STEP,
                            sequence_capture.result.failed_step);
    TEST_ASSERT_EQUAL_UINT8(1u, sequence_capture.result.steps_run);
    TEST_ASSERT_EQUAL_UINT8(1u, sequence_capture.result.reply_mask);
    TEST_ASSERT_EQUAL_HEX32(0x5Au,
                            sequence_capture.result.replies[0].data);
    TEST_ASSERT_EQUAL_UINT8(1u, sequence_capture.order);
    TEST_ASSERT_EQUAL(1, done_capture.calls);
    TEST_ASSERT_EQUAL_UINT8(2u, done_capture.order);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
}

void test_reset_requested_from_completion_prevents_next_tx(void)
{
    ResetFromCompletionCapture first_capture = {0};
    ResetTxnCapture second_capture = {0};
    DaliTransaction first = {
        .frame = { .data = 0x1111u, .bit_length = 16u },
        .on_complete = on_complete_request_reset,
        .cb_ctx = &first_capture,
    };
    DaliTransaction second = {
        .frame = { .data = 0x2222u, .bit_length = 16u },
        .on_complete = on_reset_transaction,
        .cb_ctx = &second_capture,
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&first));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&second));
    dali_sched_run();
    advance_past_settle();
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, first_capture.completion.calls);
    TEST_ASSERT_EQUAL(DALI_OK, first_capture.completion.result);
    TEST_ASSERT_EQUAL(DALI_OK, first_capture.request_result);
    TEST_ASSERT_EQUAL_UINT8(1u, first_capture.completion.order);
    TEST_ASSERT_EQUAL(1, second_capture.calls);
    TEST_ASSERT_EQUAL(DALI_SCHED_RESET_ERROR, second_capture.result);
    TEST_ASSERT_EQUAL_UINT8(2u, second_capture.order);
    TEST_ASSERT_EQUAL(1, first_capture.reset.calls);
    TEST_ASSERT_TRUE(first_capture.reset.pending_during_callback);
    TEST_ASSERT_EQUAL_UINT8(3u, first_capture.reset.order);
    TEST_ASSERT_FALSE(dali_sched_reset_pending());
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);

    dali_sched_run();
    TEST_ASSERT_EQUAL(1, second_capture.calls);
    TEST_ASSERT_EQUAL(1, g_mock_tx_count);
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
    TEST_ASSERT_TRUE(dali_sched_reset_pending());
    TEST_ASSERT_EQUAL(DALI_ERR_BUSY, dali_sched_enqueue(&second));
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
    TEST_ASSERT_FALSE(dali_sched_reset_pending());
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
    TEST_ASSERT_EQUAL(DALI_ERR_BUSY, dali_sched_enqueue(&txn));
    dali_sched_run();
    TEST_ASSERT_FALSE(dali_sched_reset_pending());
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

/* 13b. Queue diagnostics: depth, high-water, admitted, and both rejections. */
void test_queue_stats_report_depth_high_water_and_rejections(void)
{
    DaliSchedQueueStats stats;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_sched_queue_stats(NULL));

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_queue_stats(&stats));
    TEST_ASSERT_EQUAL_UINT8(0u, stats.depth);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)DALI_CMD_QUEUE_SIZE, stats.capacity);
    TEST_ASSERT_EQUAL_UINT8(0u, stats.high_water);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.admitted);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.rejected_full);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.rejected_busy);

    DaliTransaction txn = {
        .frame        = { .data = 0x0080u, .bit_length = 16u },
        .needs_reply  = false,
        .retries_left = 0u,
    };
    for (uint8_t i = 0u; i < DALI_CMD_QUEUE_SIZE; i++) {
        TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    }
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_queue_stats(&stats));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)DALI_CMD_QUEUE_SIZE, stats.depth);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)DALI_CMD_QUEUE_SIZE, stats.high_water);
    TEST_ASSERT_EQUAL_UINT32(DALI_CMD_QUEUE_SIZE, stats.admitted);

    /* A refused submission is dropped work, so it must be counted. */
    TEST_ASSERT_EQUAL(DALI_ERR_QUEUE_FULL, dali_sched_enqueue(&txn));
    TEST_ASSERT_EQUAL(DALI_ERR_QUEUE_FULL, dali_sched_enqueue(&txn));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_queue_stats(&stats));
    TEST_ASSERT_EQUAL_UINT32(2u, stats.rejected_full);
    TEST_ASSERT_EQUAL_UINT32(DALI_CMD_QUEUE_SIZE, stats.admitted);

    /* Draining lowers depth but retains the high-water mark. depth counts only
     * queued work: the entry the scheduler is executing has already been
     * popped, so one run leaves DALI_CMD_QUEUE_SIZE - 1 waiting. */
    dali_sched_run();
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_queue_stats(&stats));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(DALI_CMD_QUEUE_SIZE - 1u), stats.depth);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)DALI_CMD_QUEUE_SIZE, stats.high_water);

    /* A pending reset barrier rejects with BUSY, counted separately. */
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_request_reset(NULL, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_BUSY, dali_sched_enqueue(&txn));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_queue_stats(&stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.rejected_busy);
    TEST_ASSERT_EQUAL_UINT32(2u, stats.rejected_full);
}

/* 13c. Resetting the stats clears counters and rebases high-water on depth. */
void test_queue_stats_reset_rebases_high_water_on_current_depth(void)
{
    DaliTransaction txn = {
        .frame        = { .data = 0x0080u, .bit_length = 16u },
        .needs_reply  = false,
        .retries_left = 0u,
    };
    for (uint8_t i = 0u; i < 4u; i++) {
        TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    }
    dali_sched_run();   /* one entry popped for execution; depth 3, high-water 4 */

    DaliSchedQueueStats stats;
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_queue_stats(&stats));
    TEST_ASSERT_EQUAL_UINT8(3u, stats.depth);
    TEST_ASSERT_EQUAL_UINT8(4u, stats.high_water);

    dali_sched_reset_queue_stats();
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_queue_stats(&stats));
    TEST_ASSERT_EQUAL_UINT8(3u, stats.depth);
    /* Rebased on live depth, never below it, so the reading stays meaningful. */
    TEST_ASSERT_EQUAL_UINT8(3u, stats.high_water);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.admitted);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.rejected_full);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.rejected_busy);
}

/* 13d. A sequence occupies exactly one queue entry in the diagnostics. */
void test_queue_stats_count_a_sequence_as_one_entry(void)
{
    DaliSequence seq = {
        .steps = {
            { .frame = { .data = 0xA300u, .bit_length = 16u } },
            { .frame = { .data = 0xA500u, .bit_length = 16u } },
            { .frame = { .data = 0xA700u, .bit_length = 16u } },
        },
        .step_count = 3u,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue_sequence(&seq));

    DaliSchedQueueStats stats;
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_queue_stats(&stats));
    TEST_ASSERT_EQUAL_UINT8(1u, stats.depth);
    TEST_ASSERT_EQUAL_UINT8(1u, stats.high_water);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.admitted);
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
    TEST_ASSERT_TRUE(dali_sched_reset_pending());
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
    TEST_ASSERT_FALSE(dali_sched_reset_pending());

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

void test_quiescent_reports_active_queued_and_drained_work(void)
{
    DaliTransaction txn = {
        .frame = { .data = 0x1111u, .bit_length = 16u },
    };

    TEST_ASSERT_TRUE(dali_sched_is_quiescent());
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&txn));
    TEST_ASSERT_FALSE(dali_sched_is_quiescent());
    dali_sched_run();
    TEST_ASSERT_FALSE(dali_sched_is_quiescent());
    advance_past_settle();
    dali_sched_run();
    TEST_ASSERT_TRUE(dali_sched_is_quiescent());

    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_reset());
    TEST_ASSERT_FALSE(dali_sched_is_quiescent());
    dali_sched_run();
    TEST_ASSERT_TRUE(dali_sched_is_quiescent());
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
    RUN_TEST(test_trace_rx_stamped_before_last_tx_reports_no_offset);
    RUN_TEST(test_trace_rx_stamped_at_last_tx_reports_zero_offset);
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
    RUN_TEST(test_reset_cancels_active_and_queued_callbacks_once);
    RUN_TEST(test_reset_active_sequence_preserves_partial_result);
    RUN_TEST(test_reset_requested_from_completion_prevents_next_tx);
    RUN_TEST(test_reset_preserves_active_forward_gap);
    RUN_TEST(test_coarse_clock_rejects_ambiguous_100ms_repeat);
    RUN_TEST(test_reply_received);
    RUN_TEST(test_frame_like_rx_activity_completes_reply_with_distinct_error);
    RUN_TEST(test_reply_between_minimum_and_nominal_settling_is_accepted);
    RUN_TEST(test_decoded_backward_frame_is_accepted_before_the_standard_open);
    RUN_TEST(test_malformed_activity_keeps_the_standard_open_edge);
    RUN_TEST(test_early_and_late_malformed_activity_do_not_become_replies);
    RUN_TEST(test_in_window_short_malformed_activity_aborts_instead_of_becoming_no);
    RUN_TEST(test_in_window_ring_overflow_is_preserved_as_overflow);
    RUN_TEST(test_in_window_malformed_spends_a_retry_and_second_attempt_succeeds);
    RUN_TEST(test_in_window_overflow_spends_a_retry);
    RUN_TEST(test_in_window_malformed_reports_error_once_the_budget_is_gone);
    RUN_TEST(test_timestamped_activity_survives_delayed_wait_settle_state);
    RUN_TEST(test_precise_phy_tx_end_is_preferred_over_delayed_task_clock);
    RUN_TEST(test_timestamped_reply_survives_delayed_wait_settle_state);
    RUN_TEST(test_timestamped_phy_callback_delivers_normal_backward_reply);
    RUN_TEST(test_timestamped_phy_callback_rejects_early_and_late_backward_frames);
    RUN_TEST(test_stray_rx_while_idle_is_ignored);
    RUN_TEST(test_stale_duplicate_rx_does_not_overwrite_latched_reply);
    RUN_TEST(test_reply_accepted_when_run_delayed_past_window);
    RUN_TEST(test_timestamped_activity_captured_in_window_survives_delayed_run);
    RUN_TEST(test_rx_activity_window_handles_microsecond_clock_wrap);
    RUN_TEST(test_late_rx_after_reply_timeout_is_ignored);
    RUN_TEST(test_timestamped_late_reply_after_timeout_counts_as_late);
    RUN_TEST(test_unsolicited_24bit_idle_routes_event);
    RUN_TEST(test_init_starts_with_no_subscribers);
    RUN_TEST(test_event_reaches_primary_callback_and_added_subscriber);
    RUN_TEST(test_added_event_subscriber_receives_without_a_primary_callback);
    RUN_TEST(test_set_event_callback_replaces_instead_of_accumulating);
    RUN_TEST(test_set_event_callback_null_releases_only_its_own_slot);
    RUN_TEST(test_event_subscriber_add_is_idempotent);
    RUN_TEST(test_event_subscriber_same_callback_different_ctx_is_a_second_slot);
    RUN_TEST(test_event_subscriber_table_full_is_reported);
    RUN_TEST(test_event_subscriber_removal_stops_delivery);
    RUN_TEST(test_removing_an_unregistered_event_subscriber_is_ok);
    RUN_TEST(test_event_subscriber_rejects_null_callback);
    RUN_TEST(test_trace_reaches_primary_callback_and_added_subscriber);
    RUN_TEST(test_trace_subscriber_removal_stops_delivery);
    RUN_TEST(test_unsolicited_16bit_idle_routes_event);
    RUN_TEST(test_24bit_rx_during_settle_is_ignored_not_routed);
    RUN_TEST(test_unsolicited_24bit_during_reply_window_invalidates_query);
    RUN_TEST(test_16bit_frame_during_reply_window_invalidates_query);
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
    RUN_TEST(test_queue_stats_report_depth_high_water_and_rejections);
    RUN_TEST(test_queue_stats_reset_rebases_high_water_on_current_depth);
    RUN_TEST(test_queue_stats_count_a_sequence_as_one_entry);
    RUN_TEST(test_reset_clears_state);
    RUN_TEST(test_multiple_transactions_in_order);
    RUN_TEST(test_quiescent_reports_active_queued_and_drained_work);

    return UNITY_END();
}
