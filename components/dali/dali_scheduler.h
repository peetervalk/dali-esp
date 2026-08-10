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

typedef void (*DaliSequenceCompletionCb)(DaliError result,
                                         uint8_t failed_step,
                                         const DaliFrame *last_reply,
                                         void *cb_ctx);

typedef struct {
    DaliSequenceStep        steps[DALI_SEQUENCE_MAX_STEPS];
    uint8_t                 step_count;
    DaliSequenceCompletionCb on_complete;
    void                   *cb_ctx;
} DaliSequence;

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
 * Advance the state machine.  Call periodically from the DALI task loop.
 * Runs as far as possible in a single call — blocks only during phy_tx().
 */
void dali_sched_run(void);

/*
 * Notify the scheduler that a bus frame arrived.
 * Called by the PHY RX callback; also callable directly in host tests.
 */
void dali_sched_notify_rx(const DaliFrame *frame);

/*
 * Register a raw unsolicited-event callback.
 * Passing NULL disables event routing.
 */
DaliError dali_sched_set_event_callback(DaliSchedEventCb cb, void *cb_ctx);

/*
 * Register a task-context frame trace callback.
 * Passing NULL disables trace routing.
 */
DaliError dali_sched_set_trace_callback(DaliSchedTraceCb cb, void *cb_ctx);

/* Reset scheduler: clear queue and return to IDLE; preserve the active TX gap. */
DaliError dali_sched_reset(void);

/* Return current state (for diagnostics/tests). */
DaliSchedState dali_sched_state(void);

#ifndef DALI_HOST_BUILD
/*
 * Convenience initialiser for on-device use.
 * Wires the DALI PHY plus esp_timer millisecond and microsecond clocks.
 */
DaliError dali_sched_init_device(void);
#endif
