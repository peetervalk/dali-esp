#pragma once

/*
 * dali_transport.h — how reusable protocol code reaches the bus
 *
 * Discovery, commissioning, memory access, and input polling all need to send
 * frames without knowing whether they are driven by the ESPHome scan task, the
 * native CLI, or a host test. They share this one abstraction.
 *
 * Two levels are available:
 *
 *   transact           one frame; other traffic may run before the next call
 *   transact_sequence  a whole DaliSequence with no other local work interleaved
 *
 * Only the scheduler can provide real atomicity, so transact_sequence is
 * optional: a transport that cannot offer it leaves the pointer NULL, and
 * dali_transport_run_sequence() falls back to issuing the steps one at a time.
 * The fallback produces the same frames in the same order but gives up the
 * local atomicity guarantee, so a caller that depends on a group staying
 * together must use dali_transport_run_sequence_atomic(). Another physical
 * DALI master can still transmit between frames; this abstraction only controls
 * traffic admitted through the local scheduler.
 *
 * No hardware dependencies.
 */

#include "dali_frame.h"
#include "dali_scheduler.h"

/*
 * Send one frame and, when needs_reply is set, return the backward frame.
 * reply_out may be NULL when the caller does not want the reply.
 */
typedef DaliError (*DaliTransactionFn)(const DaliFrame *frame,
                                       bool             needs_reply,
                                       uint8_t          retries_left,
                                       bool             send_twice,
                                       DaliFrame       *reply_out,
                                       void            *ctx);

/*
 * Run every step of seq with no other locally scheduled traffic in between,
 * and report the per-step outcome. result_out may be NULL when only the overall
 * error matters; an implementation that receives a non-NULL result_out must
 * populate it before returning, on success and on failure alike.
 */
typedef DaliError (*DaliSequenceTransactionFn)(const DaliSequence *seq,
                                               DaliSequenceResult *result_out,
                                               void               *ctx);

/*
 * Block for at least ms milliseconds before the caller's next frame.
 *
 * Some protocol steps must let the bus or the gear settle — the post-RANDOMISE
 * wait while every control gear generates its 24-bit random address is the one
 * that exists today. A settle cannot be expressed as a DaliSequenceStep, which
 * carries a frame and no duration, and the shared modules must not call an RTOS
 * directly: that is what kept dali_commissioning.c dependent on FreeRTOS. The
 * environment supplies the wait instead.
 */
typedef void (*DaliDelayFn)(uint32_t ms, void *ctx);

typedef struct {
    DaliTransactionFn         transact;
    DaliSequenceTransactionFn transact_sequence; /* NULL = no atomic grouping */
    void                     *ctx;
    /*
     * Optional safety-unwind path. It must ignore front-end/user cancellation
     * while preserving ordinary scheduler, PHY, timing, and bus errors.
     * Shared workflows use it only for commands that restore protocol state.
     */
    DaliTransactionFn         transact_cleanup;
    /*
     * Optional settle wait. Workflows that need one reject a transport without
     * it before transmitting anything, rather than silently skipping the wait.
     */
    DaliDelayFn               delay_ms;
} DaliTransport;

/* True when the transport can at least send single frames. */
bool dali_transport_valid(const DaliTransport *transport);

/*
 * True when a sequence handed to dali_transport_run_sequence() runs with no
 * other locally scheduled transaction interleaved. False means the fallback
 * will be used.
 */
bool dali_transport_supports_atomic_sequence(const DaliTransport *transport);

/*
 * True when the transport can perform a settle wait. A workflow whose protocol
 * correctness depends on a wait must check this before it transmits.
 */
bool dali_transport_supports_delay(const DaliTransport *transport);

/*
 * Wait at least ms milliseconds. Returns DALI_ERR_INVALID when the transport
 * supplies no wait, so a missing settle fails loudly instead of vanishing.
 */
DaliError dali_transport_delay_ms(const DaliTransport *transport, uint32_t ms);

/*
 * Send one safety-unwind frame. Prefer transact_cleanup when supplied and
 * otherwise fall back to the ordinary transaction function.
 */
DaliError dali_transport_transact_cleanup(const DaliTransport *transport,
                                          const DaliFrame *frame,
                                          bool needs_reply,
                                          uint8_t retries_left,
                                          bool send_twice,
                                          DaliFrame *reply_out);

/*
 * Run a sequence with local atomicity when the transport supports it. On the
 * fallback path the steps are issued individually and other local work may
 * interleave.
 *
 * Returns the overall result: DALI_OK when every step succeeded, otherwise the
 * error that ended it. result_out, when given, is written on every path.
 */
DaliError dali_transport_run_sequence(const DaliTransport *transport,
                                      const DaliSequence  *seq,
                                      DaliSequenceResult  *result_out);

/*
 * Run only when the transport provides locally atomic sequence capability. A
 * frame-only or otherwise incomplete transport is rejected with
 * DALI_ERR_INVALID before any traffic; result_out is initialized on every path.
 * Use the ordinary runner only when split-frame fallback is explicitly
 * acceptable.
 */
DaliError dali_transport_run_sequence_atomic(const DaliTransport *transport,
                                             const DaliSequence  *seq,
                                             DaliSequenceResult  *result_out);

/* Per-step and fixed parts of the blocking-wait budget below. */
#define DALI_TRANSPORT_SEQUENCE_QUEUE_BUDGET_MS 200u
#define DALI_TRANSPORT_SEQUENCE_STEP_BUDGET_MS  120u

/*
 * How long a caller that blocks on a sequence should be prepared to wait.
 * Covers each step's transmits, settle gap, inter-frame guard, and reply
 * timeout once per attempt, plus headroom for queue latency. A single-frame
 * timeout is far too short for a multi-step sequence, so blocking transports
 * size their wait with this rather than a fixed constant.
 */
uint32_t dali_transport_sequence_timeout_ms(const DaliSequence *seq);
