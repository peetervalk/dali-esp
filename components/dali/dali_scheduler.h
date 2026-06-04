#pragma once

/*
 * dali_scheduler.h — DALI bus arbitration and transaction management
 *
 * Responsibilities:
 *   - Transaction queue (DALI_CMD_QUEUE_SIZE entries)
 *   - Send-twice enforcement (DALI_SEND_TWICE_WINDOW_MS)
 *   - Reply timeout (DALI_REPLY_TIMEOUT_MS) and retry (retries_left)
 *   - TX/RX handoff with DaliPhy via injected ops
 *
 * All functions run in task context — never called from ISR.
 *
 * Testability:
 *   PHY calls and time source are injected via DaliSchedOps so the scheduler
 *   can be unit-tested on the host with a mock PHY and a controllable clock.
 */

#include "dali_frame.h"
#include "dali_phy.h"   /* for DaliPhyRxCallback */

/* ---------------------------------------------------------------------------
 * Scheduler state machine states
 * --------------------------------------------------------------------------*/
typedef enum {
    SCHED_IDLE        = 0,
    SCHED_TX          = 1,
    SCHED_WAIT_SETTLE = 2,
    SCHED_WAIT_REPLY  = 3,
    SCHED_DONE        = 4,
} DaliSchedState;

/* ---------------------------------------------------------------------------
 * Completion callback — invoked (from task context) when a transaction
 * finishes, successfully or with an error.
 * reply is non-NULL only when needs_reply is set and a frame was received.
 * --------------------------------------------------------------------------*/
typedef void (*DaliSchedCompletionCb)(DaliError result,
                                      const DaliFrame *reply,
                                      void *cb_ctx);

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

/* ---------------------------------------------------------------------------
 * Injected ops — PHY interface + time source.
 * Provide real implementations on device; provide mocks in host tests.
 * --------------------------------------------------------------------------*/
typedef struct {
    DaliError (*tx)(const DaliFrame *frame);
    void      (*set_rx_callback)(DaliPhyRxCallback cb, void *ctx);
    uint32_t  (*get_tick_ms)(void);
} DaliSchedOps;

/* ---------------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------------*/

/* Initialise the scheduler with the given ops.  Must be called first. */
DaliError dali_sched_init(const DaliSchedOps *ops);

/* Enqueue a transaction.  Thread-safe between tasks (not ISR-safe). */
DaliError dali_sched_enqueue(const DaliTransaction *txn);

/*
 * Advance the state machine.  Call periodically from the DALI task loop.
 * Runs as far as possible in a single call — blocks only during phy_tx().
 */
void dali_sched_run(void);

/*
 * Notify the scheduler that a backward frame arrived.
 * Called by the PHY RX callback; also callable directly in host tests.
 */
void dali_sched_notify_rx(const DaliFrame *frame);

/* Reset scheduler: clear queue and return to IDLE. */
DaliError dali_sched_reset(void);

/* Return current state (for diagnostics/tests). */
DaliSchedState dali_sched_state(void);

#ifndef DALI_HOST_BUILD
/*
 * Convenience initialiser for on-device use.
 * Wires dali_phy_tx, dali_phy_set_rx_callback, and esp_timer milliseconds.
 */
DaliError dali_sched_init_device(void);
#endif
