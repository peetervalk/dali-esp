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

/* ---------------------------------------------------------------------------
 * Module state
 * --------------------------------------------------------------------------*/
static DaliSchedOps    s_ops;
static bool            s_initialized;

/* Queue — circular buffer of transactions */
static DaliTransaction s_queue[DALI_CMD_QUEUE_SIZE];
static uint8_t         s_q_head;    /* consumer index (dali_sched_run)    */
static uint8_t         s_q_tail;    /* producer index (dali_sched_enqueue)*/
static uint8_t         s_q_count;   /* number of items currently queued   */

/* Active transaction state */
static DaliSchedState  s_state;
static DaliTransaction s_active;          /* copy of the current transaction */
static uint8_t         s_send_count;      /* 1 = first TX done, 2 = both done */
static uint32_t        s_state_entered_ms;

/* Reply notification — written by notify_rx, read by dali_sched_run */
static volatile bool   s_reply_received;
static DaliFrame       s_reply_frame;

/* Unsolicited raw-event routing */
static DaliSchedEventCb s_event_cb;
static void            *s_event_cb_ctx;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------*/

static uint32_t elapsed_ms(uint32_t since_ms)
{
    return s_ops.get_tick_ms() - since_ms;   /* wraps safely with uint32_t */
}

static void sched_complete(DaliError result, const DaliFrame *reply)
{
    if (s_active.on_complete != NULL) {
        s_active.on_complete(result, reply, s_active.cb_ctx);
    }
}

static bool sched_is_unsolicited_event_frame(const DaliFrame *frame)
{
    return frame->bit_length == 24u;
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

/* Dequeue one item into s_active.  Caller must hold critical section. */
static bool queue_pop_locked(void)
{
    if (s_q_count == 0u) {
        return false;
    }
    s_active = s_queue[s_q_head];
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
    s_event_cb        = NULL;
    s_event_cb_ctx    = NULL;
    s_initialized = true;

    s_ops.set_rx_callback(phy_rx_handler, NULL);
    return DALI_OK;
}

DaliError dali_sched_enqueue(const DaliTransaction *txn)
{
    if (!s_initialized || txn == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliError result;
    SCHED_ENTER_CRITICAL();
    if (s_q_count >= DALI_CMD_QUEUE_SIZE) {
        result = DALI_ERR_QUEUE_FULL;
    } else {
        s_queue[s_q_tail] = *txn;
        s_q_tail  = (uint8_t)((s_q_tail + 1u) & (DALI_CMD_QUEUE_SIZE - 1u));
        s_q_count++;
        result = DALI_OK;
    }
    SCHED_EXIT_CRITICAL();
    return result;
}

void dali_sched_run(void)
{
    if (!s_initialized) {
        return;
    }

next:
    switch (s_state) {

        case SCHED_IDLE: {
            bool got_item;
            SCHED_ENTER_CRITICAL();
            got_item = queue_pop_locked();
            SCHED_EXIT_CRITICAL();
            if (!got_item) {
                return;
            }
            s_send_count = 0u;
            s_reply_received = false;
            s_state = SCHED_TX;
            goto next;
        }

        case SCHED_TX: {
            DaliError err = s_ops.tx(&s_active.frame);
            if (err != DALI_OK) {
                ESP_LOGE(TAG, "phy_tx failed: %d", (int)err);
                sched_complete(err, NULL);
                s_state = SCHED_IDLE;
                goto next;
            }
            s_send_count++;
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
            sched_complete(DALI_OK, NULL);
            s_state = SCHED_IDLE;
            goto next;
        }

        case SCHED_WAIT_REPLY: {
            if (s_reply_received) {
                sched_complete(DALI_OK, &s_reply_frame);
                s_reply_received = false;
                s_state = SCHED_IDLE;
                goto next;
            }
            if (elapsed_ms(s_state_entered_ms) >= DALI_REPLY_TIMEOUT_MS) {
                g_dali_stats.reply_timeouts++;
                if (s_active.retries_left > 0u) {
                    s_active.retries_left--;
                    g_dali_stats.tx_retries++;
                    s_send_count = 0u;
                    s_state = SCHED_TX;
                    goto next;
                }
                sched_complete(DALI_ERR_TIMEOUT, NULL);
                s_state = SCHED_IDLE;
                goto next;
            }
            return;   /* still waiting for reply */
        }

        case SCHED_DONE:
        default:
            return;
    }
}

void dali_sched_notify_rx(const DaliFrame *frame)
{
    if (!s_initialized || frame == NULL) {
        return;
    }

    if (s_state == SCHED_WAIT_REPLY &&
        s_active.needs_reply &&
        !s_reply_received &&
        frame->bit_length == 8u &&
        elapsed_ms(s_state_entered_ms) < DALI_REPLY_TIMEOUT_MS) {
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

DaliError dali_sched_reset(void)
{
    SCHED_ENTER_CRITICAL();
    s_q_head  = 0u;
    s_q_tail  = 0u;
    s_q_count = 0u;
    SCHED_EXIT_CRITICAL();

    s_state          = SCHED_IDLE;
    s_send_count     = 0u;
    s_reply_received = false;
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

DaliError dali_sched_init_device(void)
{
    static const DaliSchedOps ops = {
        .tx              = dali_phy_tx,
        .set_rx_callback = dali_phy_set_rx_callback,
        .get_tick_ms     = device_get_tick_ms,
    };
    return dali_sched_init(&ops);
}
#endif
