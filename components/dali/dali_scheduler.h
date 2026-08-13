#pragma once

/*
 * dali_scheduler.h — DALI transaction timing and queue management
 *
 * Responsibilities:
 *   - Transaction queue (DALI_CMD_QUEUE_SIZE entries)
 *   - Fixed-size transaction sequences for DTR setup and dependent commands
 *   - Send-twice expansion into adjacent forward frames
 *   - Minimum spacing between locally transmitted forward frames
 *   - Send-twice 100 ms deadline validation
 *   - Reply timeout (DALI_REPLY_TIMEOUT_MS) and retry (retries_left)
 *   - TX/RX handoff with DaliPhy via injected ops
 *   - Optional task-context trace callbacks for diagnostics
 *
 * All functions run in task context — never called from ISR.
 * DALI-2 priority/backoff, collision detection, and multi-master arbitration
 * are not implemented here.
 *
 * Testability:
 *   PHY calls and time source are injected via DaliSchedOps so the scheduler
 *   can be unit-tested on the host with a mock PHY and a controllable clock.
 */

#include "dali_frame.h"
#include "dali_phy.h"   /* for DaliPhyRxCallback */

#define DALI_SEQUENCE_MAX_STEPS      7u
#define DALI_SEQUENCE_NO_FAILED_STEP 0xFFu

/* ---------------------------------------------------------------------------
 * Scheduler state machine states
 * --------------------------------------------------------------------------*/
typedef enum {
    SCHED_IDLE        = 0,
    SCHED_TX          = 1,
    SCHED_WAIT_SETTLE = 2,
    SCHED_WAIT_REPLY  = 3,
} DaliSchedState;

/* ---------------------------------------------------------------------------
 * Completion callback — invoked (from task context) when a transaction
 * finishes, successfully or with an error.
 * reply is non-NULL only when needs_reply is set and a frame was received.
 * --------------------------------------------------------------------------*/
typedef void (*DaliSchedCompletionCb)(DaliError result,
                                      const DaliFrame *reply,
                                      void *cb_ctx);

/*
 * Called by the scheduler owner task after a requested reset has cancelled all
 * active and queued work. The callback is the reset barrier: no transaction
 * accepted before the request can execute after it is called. Queue admission
 * remains blocked for the duration of the callback, so it is also the safe
 * place for the application to reset the PHY.
 */
typedef void (*DaliSchedResetCompletionCb)(void *cb_ctx);

/* Results used for reset cancellation and an externally invalidated reply. */
#define DALI_SCHED_RESET_ERROR      DALI_ERR_CANCELLED
#define DALI_SCHED_INTERVENED_ERROR DALI_ERR_INTERVENED

/*
 * Unsolicited RX event callback — invoked from task context for raw DALI-2
 * event candidates that are not solicited backward replies.
 */
typedef void (*DaliSchedEventCb)(const DaliFrame *frame, void *cb_ctx);

typedef enum {
    DALI_SCHED_TRACE_TX = 0,
    DALI_SCHED_TRACE_RX = 1,
} DaliSchedTraceDirection;

typedef struct {
    DaliSchedTraceDirection direction;
    DaliFrame               frame;
    uint32_t                timestamp_us;
    uint32_t                since_tx_us;
    bool                    has_since_tx;
} DaliSchedTraceEvent;

/*
 * Per-frame trace callback. Invoked from task context after TX completes or
 * when RX frames reach the scheduler.
 */
typedef void (*DaliSchedTraceCb)(const DaliSchedTraceEvent *event, void *cb_ctx);

/* ---------------------------------------------------------------------------
 * Transaction descriptor
 * --------------------------------------------------------------------------*/
typedef struct {
    DaliFrame             frame;
    bool                  needs_reply;   /* expect a backward frame after TX   */
    bool                  send_twice;    /* must transmit the frame twice      */
    uint8_t               retries_left;  /* retry budget; 0 = no retries       */
    DaliSchedCompletionCb on_complete;   /* optional; NULL = fire-and-forget   */
    void                 *cb_ctx;        /* forwarded to on_complete           */
} DaliTransaction;

typedef struct {
    DaliFrame frame;
    bool      needs_reply;
    bool      send_twice;
    uint8_t   retries_left;
} DaliSequenceStep;

/*
 * Outcome of one sequence, including a backward frame per reply-bearing step.
 * `replies[i]` is meaningful only when bit i of `reply_mask` is set. Steps after
 * a failure never run, so they are never present in the mask. failed_step is
 * also DALI_SEQUENCE_NO_FAILED_STEP for reset cancellation, which is not
 * attributable to a sequence step; steps_run still reports attempted steps.
 *
 * The pointer handed to the completion callback refers to scheduler-owned
 * storage that is reused by the next sequence; copy anything needed later.
 */
typedef struct {
    DaliError result;       /* DALI_OK, or the error that ended the sequence  */
    uint8_t   failed_step;  /* DALI_SEQUENCE_NO_FAILED_STEP when result is OK */
    uint8_t   steps_run;    /* steps attempted, including a failing one       */
    uint8_t   reply_mask;   /* bit i set = replies[i] holds a backward frame  */
    DaliFrame replies[DALI_SEQUENCE_MAX_STEPS];
} DaliSequenceResult;

typedef void (*DaliSequenceCompletionCb)(const DaliSequenceResult *result,
                                         void *cb_ctx);

typedef struct {
    DaliSequenceStep        steps[DALI_SEQUENCE_MAX_STEPS];
    uint8_t                 step_count;
    DaliSequenceCompletionCb on_complete;
    void                   *cb_ctx;
} DaliSequence;

/* ---------------------------------------------------------------------------
 * Queue admission diagnostics.
 *
 * depth/capacity are instantaneous; the counters are cumulative since
 * dali_sched_init() or the last dali_sched_reset_queue_stats(). high_water is
 * the largest depth reached, so a value at capacity means admission was one
 * submission away from failing even if rejected_full is still zero.
 *
 * rejected_full counts DALI_ERR_QUEUE_FULL and rejected_busy counts
 * DALI_ERR_BUSY (submission during a pending reset barrier). Both are dropped
 * work: the scheduler never retries a rejected submission, so a non-zero value
 * means some caller's command did not reach the bus.
 * --------------------------------------------------------------------------*/
typedef struct {
    uint8_t  depth;         /* entries currently queued                       */
    uint8_t  capacity;      /* DALI_CMD_QUEUE_SIZE                            */
    uint8_t  high_water;    /* largest depth observed                         */
    uint32_t admitted;      /* submissions accepted                           */
    uint32_t rejected_full; /* submissions refused with DALI_ERR_QUEUE_FULL   */
    uint32_t rejected_busy; /* submissions refused with DALI_ERR_BUSY (reset) */
} DaliSchedQueueStats;

/* ---------------------------------------------------------------------------
 * Injected ops — PHY interface + time source.
 * Provide real implementations on device; provide mocks in host tests.
 * --------------------------------------------------------------------------*/
typedef struct {
    DaliError (*tx)(const DaliFrame *frame);
    void      (*set_rx_callback)(DaliPhyRxCallback cb, void *ctx);
    uint32_t  (*get_tick_ms)(void);
    uint32_t  (*get_time_us)(void); /* optional; device uses this for µs guards */
} DaliSchedOps;

/* ---------------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------------*/

/* Initialise the scheduler with the given ops.  Must be called first. */
DaliError dali_sched_init(const DaliSchedOps *ops);

/* Enqueue a transaction.  Thread-safe between tasks (not ISR-safe). */
DaliError dali_sched_enqueue(const DaliTransaction *txn);

/*
 * Enqueue a fixed sequence. The scheduler copies the sequence and runs all
 * steps contiguously before popping the next queue entry. If a step fails,
 * later steps are skipped and failed_step is the zero-based step index. On
 * success, failed_step is DALI_SEQUENCE_NO_FAILED_STEP.
 */
DaliError dali_sched_enqueue_sequence(const DaliSequence *seq);

/*
 * Copy the backward frame captured for `step`. Returns false when the step did
 * not run or produced no reply.
 */
bool dali_sequence_result_reply(const DaliSequenceResult *result,
                                uint8_t step,
                                DaliFrame *out);

/* Copy the reply from the highest-numbered step that produced one. */
bool dali_sequence_result_last_reply(const DaliSequenceResult *result,
                                     DaliFrame *out);

/*
 * Advance the state machine.  Call periodically from the DALI task loop.
 * Runs as far as possible in a single call — blocks only during phy_tx().
 */
void dali_sched_run(void);

/*
 * Notify the scheduler that a bus frame arrived.
 * Called by the PHY RX callback; also callable directly in host tests.
 */
void dali_sched_notify_rx(const DaliFrame *frame);

/* ---------------------------------------------------------------------------
 * Event and trace subscribers
 *
 * Both streams fan out to several subscribers, because more than one consumer
 * legitimately wants them at once: the ESPHome integration routes unsolicited
 * events into its Part 103 dispatch table while a diagnostic shell session is
 * capturing the same frames, and neither can be asked to give the stream up so
 * the other can have it.
 *
 * Ordering is registration order, and every subscriber sees every frame; a
 * subscriber that consumes a frame does not hide it from the next one.
 *
 * A subscriber runs in the scheduler owner task, under the same contract the
 * single callback always had: no blocking, no allocation, no re-entry into the
 * scheduler.
 *
 * Lifetime: register before the DALI task starts and leave registered. A
 * subscriber removed while that task is running can still be entered once more,
 * because the fan-out may already have read the slot, so its ctx must stay
 * valid for at least one scheduler pass after removal. Removal exists for host
 * tests and for teardown paths that have already quiesced the task; a session
 * that comes and goes should stay registered and gate on its own state instead.
 * --------------------------------------------------------------------------*/

#define DALI_SCHED_MAX_EVENT_SUBSCRIBERS 3u
#define DALI_SCHED_MAX_TRACE_SUBSCRIBERS 3u

/*
 * Add a subscriber. The same (cb, ctx) pair registered twice is accepted once
 * and reported as DALI_OK, so a front end that cannot cheaply tell whether it
 * has already registered does not have to track it.
 *
 * Returns DALI_ERR_FULL when every slot is taken, DALI_ERR_INVALID for a NULL
 * callback or before dali_sched_init().
 */
DaliError dali_sched_add_event_subscriber(DaliSchedEventCb cb, void *cb_ctx);
DaliError dali_sched_add_trace_subscriber(DaliSchedTraceCb cb, void *cb_ctx);

/* Remove one subscriber. Removing one that is not registered is DALI_OK. */
DaliError dali_sched_remove_event_subscriber(DaliSchedEventCb cb, void *cb_ctx);
DaliError dali_sched_remove_trace_subscriber(DaliSchedTraceCb cb, void *cb_ctx);

/* How many subscribers are currently registered, for diagnostics and tests. */
uint8_t dali_sched_event_subscriber_count(void);
uint8_t dali_sched_trace_subscriber_count(void);

/*
 * Register the single "primary" callback.
 *
 * These predate the fan-out and are kept because most callers genuinely have
 * exactly one consumer and should not have to think about removal. Each owns
 * one reserved subscriber slot: calling it again replaces what the previous
 * call installed rather than adding a second subscriber, and passing NULL
 * releases the slot. Subscribers added with the functions above are untouched
 * either way.
 */
DaliError dali_sched_set_event_callback(DaliSchedEventCb cb, void *cb_ctx);
DaliError dali_sched_set_trace_callback(DaliSchedTraceCb cb, void *cb_ctx);

/*
 * Request an owner-task reset.
 *
 * This call is safe from another task and does not synchronously mutate active
 * scheduler state. The next dali_sched_run() cancels the active transaction
 * and every queued transaction with DALI_SCHED_RESET_ERROR, then calls
 * completion_cb from the scheduler owner task. Sequence results are filled
 * before their callbacks run. While reset is pending, new queue submissions
 * fail with DALI_ERR_BUSY.
 *
 * A successful return means the request was accepted, not that reset is
 * complete. completion_cb is the completion barrier. A second request while
 * one is pending returns DALI_ERR_BUSY.
 */
DaliError dali_sched_request_reset(DaliSchedResetCompletionCb completion_cb,
                                   void *cb_ctx);

/* True from reset request acceptance until its completion callback returns. */
bool dali_sched_reset_pending(void);

/*
 * Compatibility wrapper for dali_sched_request_reset(NULL, NULL).
 * Reset is deferred; callers needing a barrier must use
 * dali_sched_request_reset().
 */
DaliError dali_sched_reset(void);

/* Return current state (for diagnostics/tests). */
DaliSchedState dali_sched_state(void);

/*
 * True only when no transaction is active or queued and no reset barrier is
 * pending. Intended for an exclusive workflow to wait until previously
 * admitted work has drained before it starts enqueueing.
 */
bool dali_sched_is_quiescent(void);

/*
 * Copy the current queue admission diagnostics. Safe from any task; the sample
 * is taken under the scheduler's critical section, so depth and the counters
 * are mutually consistent. Returns DALI_ERR_INVALID for a NULL argument or
 * before dali_sched_init().
 */
DaliError dali_sched_queue_stats(DaliSchedQueueStats *out);

/* Clear high_water and the cumulative counters; depth/capacity are unaffected. */
void dali_sched_reset_queue_stats(void);

#ifndef DALI_HOST_BUILD
/*
 * Convenience initialiser for on-device use.
 * Wires the DALI PHY plus esp_timer millisecond and microsecond clocks.
 */
DaliError dali_sched_init_device(void);
#endif
