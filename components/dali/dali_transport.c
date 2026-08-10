#include "dali_transport.h"

#include <string.h>

bool dali_transport_valid(const DaliTransport *transport)
{
    return transport != NULL && transport->transact != NULL;
}

bool dali_transport_supports_atomic_sequence(const DaliTransport *transport)
{
    return transport != NULL && transport->transact_sequence != NULL;
}

uint32_t dali_transport_sequence_timeout_ms(const DaliSequence *seq)
{
    uint32_t ms = DALI_TRANSPORT_SEQUENCE_QUEUE_BUDGET_MS;
    if (seq == NULL) {
        return ms;
    }

    uint8_t steps = seq->step_count > DALI_SEQUENCE_MAX_STEPS
                  ? DALI_SEQUENCE_MAX_STEPS
                  : seq->step_count;
    for (uint8_t i = 0u; i < steps; i++) {
        uint32_t attempts = 1u + (uint32_t)seq->steps[i].retries_left;
        ms += attempts * DALI_TRANSPORT_SEQUENCE_STEP_BUDGET_MS;
    }
    return ms;
}

static bool sequence_runnable(const DaliSequence *seq)
{
    if (seq == NULL ||
        seq->step_count == 0u ||
        seq->step_count > DALI_SEQUENCE_MAX_STEPS) {
        return false;
    }

    for (uint8_t i = 0u; i < seq->step_count; i++) {
        uint8_t bits = seq->steps[i].frame.bit_length;
        if (bits == 0u || bits > DALI_MAX_FRAME_BITS) {
            return false;
        }
    }
    return true;
}

static void result_init(DaliSequenceResult *result, DaliError error)
{
    memset(result, 0, sizeof(*result));
    result->result      = error;
    result->failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
}

/* Issue the steps individually. Correct frames, but no atomicity. */
static DaliError run_sequence_stepwise(const DaliTransport *transport,
                                       const DaliSequence  *seq,
                                       DaliSequenceResult  *result)
{
    for (uint8_t i = 0u; i < seq->step_count; i++) {
        const DaliSequenceStep *step = &seq->steps[i];
        DaliFrame reply = {0u, 0u};

        DaliError err = transport->transact(&step->frame,
                                            step->needs_reply,
                                            step->retries_left,
                                            step->send_twice,
                                            step->needs_reply ? &reply : NULL,
                                            transport->ctx);
        result->steps_run = (uint8_t)(i + 1u);

        if (err != DALI_OK) {
            result->result      = err;
            result->failed_step = i;
            return err;
        }

        if (step->needs_reply) {
            result->replies[i] = reply;
            result->reply_mask |= (uint8_t)(1u << i);
        }
    }

    return DALI_OK;
}

DaliError dali_transport_run_sequence(const DaliTransport *transport,
                                      const DaliSequence  *seq,
                                      DaliSequenceResult  *result_out)
{
    DaliSequenceResult result;
    result_init(&result, DALI_ERR_INVALID);

    /* Seed the caller's struct before anything can fail or delegate, so it is
     * never left holding whatever the stack had. */
    if (result_out != NULL) {
        *result_out = result;
    }

    if (!dali_transport_valid(transport) || !sequence_runnable(seq)) {
        return DALI_ERR_INVALID;
    }

    if (transport->transact_sequence != NULL) {
        return transport->transact_sequence(seq, result_out, transport->ctx);
    }

    result.result = DALI_OK;
    DaliError err = run_sequence_stepwise(transport, seq, &result);
    if (result_out != NULL) {
        *result_out = result;
    }
    return err;
}
