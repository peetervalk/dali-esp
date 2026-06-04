#pragma once

/*
 * dali_scheduler.h — bus arbitration and transaction management
 *
 * Responsibilities:
 *   - Transaction queue (DALI_CMD_QUEUE_SIZE entries)
 *   - Send-twice enforcement (within DALI_SEND_TWICE_WINDOW_MS)
 *   - Reply timeout and retry (up to DALI_MAX_RETRIES)
 *   - TX/RX handoff with DaliPhy
 *
 * All functions run in task context — never called from ISR.
 */

#include "dali_frame.h"

/* ---------------------------------------------------------------------------
 * Scheduler transaction state machine
 * --------------------------------------------------------------------------*/
typedef enum {
    SCHED_IDLE         = 0,
    SCHED_TX           = 1,
    SCHED_WAIT_SETTLE  = 2,
    SCHED_WAIT_REPLY   = 3,
    SCHED_DONE         = 4,
} DaliSchedState;

/* ---------------------------------------------------------------------------
 * Transaction descriptor
 * --------------------------------------------------------------------------*/
typedef struct {
    DaliFrame  frame;
    bool       needs_reply;   /* expect a backward frame after TX        */
    bool       send_twice;    /* configuration command — must send twice  */
    uint8_t    retries_left;
} DaliTransaction;

/* ---------------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------------*/
DaliError dali_sched_init(void);
DaliError dali_sched_enqueue(const DaliTransaction *tx);
void      dali_sched_run(void);    /* call from DALI task loop              */
DaliError dali_sched_reset(void);
