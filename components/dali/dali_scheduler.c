#include "dali_scheduler.h"

/* Stub — to be implemented in Phase 8 */

DaliError dali_sched_init(void)
{
    return DALI_OK;
}

DaliError dali_sched_enqueue(const DaliTransaction *tx)
{
    (void)tx;
    return DALI_ERR_QUEUE_FULL; /* not yet implemented */
}

void dali_sched_run(void)
{
    /* TODO: process transaction queue */
}

DaliError dali_sched_reset(void)
{
    return DALI_OK;
}
