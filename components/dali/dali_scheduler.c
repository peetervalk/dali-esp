#include "dali_scheduler.h"
#include <string.h>

#ifndef DALI_HOST_BUILD
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
static const char *TAG = "DALI-SCHED";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define SCHED_ENTER_CRITICAL()  taskENTER_CRITICAL(&s_mux)
#define SCHED_EXIT_CRITICAL()   taskEXIT_CRITICAL(&s_mux)
#else
#define SCHED_ENTER_CRITICAL()  do {} while (0)
#define SCHED_EXIT_CRITICAL()   do {} while (0)
#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
static const char *TAG = "DALI-SCHED";
#endif

_Static_assert((DALI_CMD_QUEUE_SIZE & (DALI_CMD_QUEUE_SIZE - 1u)) == 0u,
               "DALI_CMD_QUEUE_SIZE must be a power of two");

/* ---------------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------------*/
typedef enum {
    SCHED_QUEUE_TRANSACTION = 0,
    SCHED_QUEUE_SEQUENCE    = 1,
} SchedQueueEntryKind;

typedef struct {
    SchedQueueEntryKind kind;
    union {
        DaliTransaction txn;
        DaliSequence    sequence;
    } item;
} SchedQueueEntry;

static DaliSchedOps    s_ops;
static bool            s_initialized;

/* Queue — circular buffer of transactions */
static SchedQueueEntry s_queue[DALI_CMD_QUEUE_SIZE];
static uint8_t         s_q_head;    /* consumer index (dali_sched_run)    */
static uint8_t         s_q_tail;    /* producer index (dali_sched_enqueue)*/
static uint8_t         s_q_count;   /* number of items currently queued   */

/* Active transaction state */
static DaliSchedState  s_state;
static DaliTransaction s_active;          /* copy of the current transaction */
static uint8_t         s_send_count;      /* 1 = first TX done, 2 = both done */
static uint32_t        s_state_entered_ms;
static bool            s_active_is_sequence;
static DaliSequence    s_active_sequence;
static uint8_t         s_active_sequence_step;
static bool            s_sequence_has_reply;
static DaliFrame       s_sequence_last_reply;

/* Reply notification — written by notify_rx, read by dali_sched_run */
static volatile bool   s_reply_received;
static DaliFrame       s_reply_frame;

/* Unsolicited raw-event routing */
static DaliSchedEventCb s_event_cb;
static void            *s_event_cb_ctx;

/* Diagnostic trace routing */
static DaliSchedTraceCb s_trace_cb;
static void            *s_trace_cb_ctx;
static uint32_t         s_last_tx_us;
static bool             s_have_last_tx_us;
static uint32_t         s_tx_guard_started_us;
static uint32_t         s_tx_guard_started_ms;
static uint32_t         s_tx_guard_duration_us;
static uint32_t         s_send_twice_first_started_us;
static uint32_t         s_send_twice_first_started_ms;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------*/

static uint32_t elapsed_ms(uint32_t since_ms)
{
    return s_ops.get_tick_ms() - since_ms;   /* wraps safely with uint32_t */
}

static uint32_t sched_now_us(void)
{
    if (s_ops.get_time_us != NULL) {
        return s_ops.get_time_us();
    }
    return s_ops.get_tick_ms() * 1000u;
}

static bool sched_tx_guard_active(uint32_t now_us)
{
    if (s_tx_guard_duration_us == 0u) {
        return false;
    }

    uint32_t elapsed_guard_ms = s_ops.get_tick_ms() - s_tx_guard_started_ms;
    uint32_t guard_ceiling_ms = (s_tx_guard_duration_us + 999u) / 1000u;
    if (elapsed_guard_ms > guard_ceiling_ms ||
        now_us - s_tx_guard_started_us >= s_tx_guard_duration_us) {
        s_tx_guard_duration_us = 0u;
        return false;
    }
    return true;
}

static bool sched_send_twice_window_missed(uint32_t now_us,
                                           bool before_repeat)
{
    uint32_t elapsed_us = now_us - s_send_twice_first_started_us;
    uint32_t pair_elapsed_ms =
        s_ops.get_tick_ms() - s_send_twice_first_started_ms;

    /* The millisecond clock disambiguates a delayed repeat after the 32-bit
     * microsecond clock wraps. */
    if (pair_elapsed_ms > DALI_SEND_TWICE_WINDOW_MS) {
        return true;
    }

    /* A millisecond-only clock cannot prove the inclusive 100 ms boundary.
     * Reject its 100 ms tick conservatively; the device path uses esp_timer. */
    if (s_ops.get_time_us == NULL) {
        return pair_elapsed_ms >= DALI_SEND_TWICE_WINDOW_MS;
    }
    /* Completion exactly at 100 ms is valid, but starting a blocking PHY call
     * at that boundary cannot complete within the window. */
    return before_repeat ? elapsed_us >= DALI_SEND_TWICE_WINDOW_US
                         : elapsed_us > DALI_SEND_TWICE_WINDOW_US;
}

static void sched_arm_tx_gap(uint32_t frame_done_us, uint32_t gap_us)
{
    s_tx_guard_started_us = frame_done_us;
    s_tx_guard_started_ms = s_ops.get_tick_ms();
    s_tx_guard_duration_us = gap_us;
}

static void sched_complete_transaction(DaliError result, const DaliFrame *reply)
{
    if (s_active.on_complete != NULL) {
        s_active.on_complete(result, reply, s_active.cb_ctx);
    }
}

static void sched_load_sequence_step(uint8_t step)
{
    const DaliSequenceStep *src = &s_active_sequence.steps[step];
    s_active = (DaliTransaction){
        .frame        = src->frame,
        .needs_reply  = src->needs_reply,
        .send_twice   = src->send_twice,
        .retries_left = src->retries_left,
        .on_complete  = NULL,
        .cb_ctx       = NULL,
    };
}

static void sched_complete_sequence(DaliError result, uint8_t failed_step)
{
    DaliSequenceCompletionCb cb = s_active_sequence.on_complete;
    void *cb_ctx = s_active_sequence.cb_ctx;
    const DaliFrame *reply = s_sequence_has_reply ? &s_sequence_last_reply : NULL;

    s_active_is_sequence = false;
    s_active_sequence_step = 0u;
    s_sequence_has_reply = false;

    if (cb != NULL) {
        cb(result, failed_step, reply, cb_ctx);
    }
}

static bool sched_finish_active_step(DaliError result, const DaliFrame *reply)
{
    if (!s_active_is_sequence) {
        sched_complete_transaction(result, reply);
        return false;
    }

    if (reply != NULL) {
        s_sequence_last_reply = *reply;
        s_sequence_has_reply = true;
    }

    if (result != DALI_OK) {
        sched_complete_sequence(result, s_active_sequence_step);
        return false;
    }

    s_active_sequence_step++;
    if (s_active_sequence_step >= s_active_sequence.step_count) {
        sched_complete_sequence(DALI_OK, DALI_SEQUENCE_NO_FAILED_STEP);
        return false;
    }

    sched_load_sequence_step(s_active_sequence_step);
    s_send_count = 0u;
    s_send_twice_first_started_us = 0u;
    s_send_twice_first_started_ms = 0u;
    s_reply_received = false;
    return true;
}

static void sched_trace(DaliSchedTraceDirection direction,
                        const DaliFrame *frame,
                        uint32_t timestamp_us)
{
    if (s_trace_cb == NULL || frame == NULL) {
        return;
    }

    DaliSchedTraceEvent event = {
        .direction    = direction,
        .frame        = *frame,
        .timestamp_us = timestamp_us,
        .since_tx_us  = 0u,
        .has_since_tx = false,
    };

    if (direction == DALI_SCHED_TRACE_RX && s_have_last_tx_us) {
        event.since_tx_us  = timestamp_us - s_last_tx_us;
        event.has_since_tx = true;
    }

    s_trace_cb(&event, s_trace_cb_ctx);
}

static bool sched_is_unsolicited_event_frame(const DaliFrame *frame)
{
    return frame->bit_length == DALI_FORWARD_FRAME_BITS ||
           frame->bit_length == DALI_EXTENDED_FRAME_BITS;
}

static bool sched_can_route_unsolicited_event(void)
{
    return s_state == SCHED_IDLE || s_state == SCHED_WAIT_REPLY;
}

static void sched_route_unsolicited_or_ignore(const DaliFrame *frame)
{
    if (sched_can_route_unsolicited_event() &&
        sched_is_unsolicited_event_frame(frame) &&
        s_event_cb != NULL) {
        g_dali_stats.unsolicited_events_routed++;
        s_event_cb(frame, s_event_cb_ctx);
        return;
    }
    g_dali_stats.rx_ignored_outside_reply++;
}

static DaliError queue_push(const SchedQueueEntry *entry)
{
    if (entry == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliError result;
    SCHED_ENTER_CRITICAL();
    if (s_q_count >= DALI_CMD_QUEUE_SIZE) {
        result = DALI_ERR_QUEUE_FULL;
    } else {
        s_queue[s_q_tail] = *entry;
        s_q_tail  = (uint8_t)((s_q_tail + 1u) & (DALI_CMD_QUEUE_SIZE - 1u));
        s_q_count++;
        result = DALI_OK;
    }
    SCHED_EXIT_CRITICAL();
    return result;
}

static bool sequence_valid(const DaliSequence *seq)
{
    if (seq == NULL ||
        seq->step_count == 0u ||
        seq->step_count > DALI_SEQUENCE_MAX_STEPS) {
        return false;
    }

    for (uint8_t i = 0u; i < seq->step_count; i++) {
        const DaliFrame *frame = &seq->steps[i].frame;
        if (frame->bit_length == 0u ||
            frame->bit_length > DALI_MAX_FRAME_BITS) {
            return false;
        }
    }
    return true;
}

/* Dequeue one item. Caller must hold critical section. */
static bool queue_pop_locked(SchedQueueEntry *entry)
{
    if (entry == NULL || s_q_count == 0u) {
        return false;
    }
    *entry = s_queue[s_q_head];
    s_q_head = (uint8_t)((s_q_head + 1u) & (DALI_CMD_QUEUE_SIZE - 1u));
    s_q_count--;
    return true;
}

/* ---------------------------------------------------------------------------
 * PHY RX callback — registered with PHY during init
 * --------------------------------------------------------------------------*/
static void phy_rx_handler(const DaliFrame *frame, void *ctx)
{
    (void)ctx;
    dali_sched_notify_rx(frame);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------*/

DaliError dali_sched_init(const DaliSchedOps *ops)
{
    if (ops == NULL || ops->tx == NULL ||
        ops->set_rx_callback == NULL || ops->get_tick_ms == NULL) {
        return DALI_ERR_INVALID;
    }

    s_ops         = *ops;
    s_q_head      = 0u;
    s_q_tail      = 0u;
    s_q_count     = 0u;
    s_state       = SCHED_IDLE;
    s_send_count  = 0u;
    s_reply_received = false;
    s_active_is_sequence = false;
    s_active_sequence_step = 0u;
    s_sequence_has_reply = false;
    memset(&s_active_sequence, 0, sizeof(s_active_sequence));
    memset(&s_sequence_last_reply, 0, sizeof(s_sequence_last_reply));
    s_event_cb        = NULL;
    s_event_cb_ctx    = NULL;
    s_trace_cb        = NULL;
    s_trace_cb_ctx    = NULL;
    s_last_tx_us      = 0u;
    s_have_last_tx_us = false;
    s_tx_guard_started_us = 0u;
    s_tx_guard_started_ms = 0u;
    s_tx_guard_duration_us = 0u;
    s_send_twice_first_started_us = 0u;
    s_send_twice_first_started_ms = 0u;
    s_initialized = true;

    s_ops.set_rx_callback(phy_rx_handler, NULL);
    return DALI_OK;
}

DaliError dali_sched_enqueue(const DaliTransaction *txn)
{
    if (!s_initialized || txn == NULL) {
        return DALI_ERR_INVALID;
    }

    SchedQueueEntry entry = {
        .kind = SCHED_QUEUE_TRANSACTION,
        .item.txn = *txn,
    };
    return queue_push(&entry);
}

DaliError dali_sched_enqueue_sequence(const DaliSequence *seq)
{
    if (!s_initialized || !sequence_valid(seq)) {
        return DALI_ERR_INVALID;
    }

    SchedQueueEntry entry = {
        .kind = SCHED_QUEUE_SEQUENCE,
        .item.sequence = *seq,
    };
    return queue_push(&entry);
}

void dali_sched_run(void)
{
    if (!s_initialized) {
        return;
    }

next:
    switch (s_state) {

        case SCHED_IDLE: {
            SchedQueueEntry entry;
            bool got_item;
            SCHED_ENTER_CRITICAL();
            got_item = queue_pop_locked(&entry);
            SCHED_EXIT_CRITICAL();
            if (!got_item) {
                return;
            }
            if (entry.kind == SCHED_QUEUE_SEQUENCE) {
                s_active_sequence = entry.item.sequence;
                s_active_is_sequence = true;
                s_active_sequence_step = 0u;
                s_sequence_has_reply = false;
                memset(&s_sequence_last_reply, 0, sizeof(s_sequence_last_reply));
                sched_load_sequence_step(0u);
            } else {
                s_active = entry.item.txn;
                s_active_is_sequence = false;
            }
            s_send_count = 0u;
            s_send_twice_first_started_us = 0u;
            s_send_twice_first_started_ms = 0u;
            s_reply_received = false;
            s_state = SCHED_TX;
            goto next;
        }

        case SCHED_TX: {
            uint32_t now_us = sched_now_us();
            if (s_active.send_twice && s_send_count == 1u &&
                sched_send_twice_window_missed(now_us, true)) {
                ESP_LOGE(TAG, "send-twice window missed");
                if (sched_finish_active_step(DALI_ERR_TIMING, NULL)) {
                    s_state = SCHED_TX;
                } else {
                    s_state = SCHED_IDLE;
                }
                goto next;
            }
            if (sched_tx_guard_active(now_us)) {
                return;
            }

            if (s_active.send_twice && s_send_count == 0u) {
                /* Bracket the physical pair conservatively: the first anchor is
                 * before the blocking PHY call; the final check is after it. */
                s_send_twice_first_started_us = now_us;
                s_send_twice_first_started_ms = s_ops.get_tick_ms();
            }

            DaliError err = s_ops.tx(&s_active.frame);
            if (err != DALI_OK) {
                ESP_LOGE(TAG, "phy_tx failed: %d", (int)err);
                /* An error may follow a partial waveform. Delay the next attempt
                 * when the PHY cannot say how much of the frame reached the bus. */
                sched_arm_tx_gap(sched_now_us(), DALI_FORWARD_INTERFRAME_US);
                if (sched_finish_active_step(err, NULL)) {
                    s_state = SCHED_TX;
                } else {
                    s_state = SCHED_IDLE;
                }
                goto next;
            }
            uint32_t tx_done_us = sched_now_us();
            s_last_tx_us = tx_done_us;
            s_have_last_tx_us = true;
            sched_trace(DALI_SCHED_TRACE_TX, &s_active.frame, tx_done_us);
            sched_arm_tx_gap(tx_done_us, DALI_FORWARD_INTERFRAME_US);
            s_send_count++;
            if (s_active.send_twice && s_send_count == 2u &&
                sched_send_twice_window_missed(tx_done_us, false)) {
                ESP_LOGE(TAG, "send-twice window crossed during phy_tx");
                if (sched_finish_active_step(DALI_ERR_TIMING, NULL)) {
                    s_state = SCHED_TX;
                } else {
                    s_state = SCHED_IDLE;
                }
                goto next;
            }
            s_state_entered_ms = s_ops.get_tick_ms();
            s_state = SCHED_WAIT_SETTLE;
            return;   /* wait for settle period before proceeding */
        }

        case SCHED_WAIT_SETTLE: {
            if (elapsed_ms(s_state_entered_ms) < DALI_SETTLE_MS) {
                return;   /* not enough time elapsed yet */
            }
            /* Send-twice: if first send just completed, do the second */
            if (s_active.send_twice && s_send_count == 1u) {
                s_state = SCHED_TX;
                goto next;
            }
            if (s_active.needs_reply) {
                s_reply_received = false;
                s_state_entered_ms = s_ops.get_tick_ms();
                s_state = SCHED_WAIT_REPLY;
                return;
            }
            /* No reply needed — transaction complete */
            if (sched_finish_active_step(DALI_OK, NULL)) {
                s_state = SCHED_TX;
            } else {
                s_state = SCHED_IDLE;
            }
            goto next;
        }

        case SCHED_WAIT_REPLY: {
            if (s_reply_received) {
                DaliFrame reply = s_reply_frame;
                s_reply_received = false;
                if (sched_finish_active_step(DALI_OK, &reply)) {
                    s_state = SCHED_TX;
                } else {
                    s_state = SCHED_IDLE;
                }
                goto next;
            }
            if (elapsed_ms(s_state_entered_ms) >= DALI_REPLY_TIMEOUT_MS) {
                g_dali_stats.reply_timeouts++;
                if (s_active.retries_left > 0u) {
                    s_active.retries_left--;
                    g_dali_stats.tx_retries++;
                    s_send_count = 0u;
                    s_send_twice_first_started_us = 0u;
                    s_send_twice_first_started_ms = 0u;
                    s_state = SCHED_TX;
                    goto next;
                }
                if (sched_finish_active_step(DALI_ERR_TIMEOUT, NULL)) {
                    s_state = SCHED_TX;
                } else {
                    s_state = SCHED_IDLE;
                }
                goto next;
            }
            return;   /* still waiting for reply */
        }

        default:
            return;
    }
}

void dali_sched_notify_rx(const DaliFrame *frame)
{
    if (!s_initialized || frame == NULL) {
        return;
    }

    sched_trace(DALI_SCHED_TRACE_RX, frame, sched_now_us());

    if (s_state == SCHED_WAIT_REPLY &&
        s_active.needs_reply &&
        !s_reply_received &&
        frame->bit_length == DALI_BACKWARD_FRAME_BITS) {
        s_reply_frame    = *frame;
        s_reply_received = true;
        return;
    }

    sched_route_unsolicited_or_ignore(frame);
}

DaliError dali_sched_set_event_callback(DaliSchedEventCb cb, void *cb_ctx)
{
    if (!s_initialized) {
        return DALI_ERR_INVALID;
    }
    s_event_cb     = cb;
    s_event_cb_ctx = cb != NULL ? cb_ctx : NULL;
    return DALI_OK;
}

DaliError dali_sched_set_trace_callback(DaliSchedTraceCb cb, void *cb_ctx)
{
    if (!s_initialized) {
        return DALI_ERR_INVALID;
    }
    s_trace_cb     = cb;
    s_trace_cb_ctx = cb != NULL ? cb_ctx : NULL;
    return DALI_OK;
}

DaliError dali_sched_reset(void)
{
    SCHED_ENTER_CRITICAL();
    s_q_head  = 0u;
    s_q_tail  = 0u;
    s_q_count = 0u;
    memset(&s_active, 0, sizeof(s_active));
    memset(&s_active_sequence, 0, sizeof(s_active_sequence));
    memset(&s_sequence_last_reply, 0, sizeof(s_sequence_last_reply));
    memset(&s_reply_frame, 0, sizeof(s_reply_frame));
    s_state            = SCHED_IDLE;
    s_send_count       = 0u;
    s_send_twice_first_started_us = 0u;
    s_send_twice_first_started_ms = 0u;
    s_state_entered_ms = 0u;
    s_reply_received   = false;
    s_active_is_sequence = false;
    s_active_sequence_step = 0u;
    s_sequence_has_reply = false;
    s_last_tx_us       = 0u;
    s_have_last_tx_us  = false;
    /* Preserve the physical-bus TX guard across a logical scheduler reset. */
    SCHED_EXIT_CRITICAL();
    return DALI_OK;
}

DaliSchedState dali_sched_state(void)
{
    return s_state;
}

/* ---------------------------------------------------------------------------
 * On-device convenience initialiser
 * --------------------------------------------------------------------------*/
#ifndef DALI_HOST_BUILD
static uint32_t device_get_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t device_get_time_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

DaliError dali_sched_init_device(void)
{
    static const DaliSchedOps ops = {
        .tx              = dali_phy_tx,
        .set_rx_callback = dali_phy_set_rx_callback,
        .get_tick_ms     = device_get_tick_ms,
        .get_time_us     = device_get_time_us,
    };
    return dali_sched_init(&ops);
}
#endif
