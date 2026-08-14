#include "dali_shell.h"
#include "dali_cli.h"
#include "dali_phy.h"
#include "dali_scheduler.h"
#include "dali_protocol.h"
#include "dali_control.h"
#include "dali_input_device.h"
#include "dali_input_config.h"
#include "dali_input_poll.h"
#include "dali_event.h"
#include "dali_discovery.h"
#include "dali_transport.h"
#include "dali_commissioning.h"
#include "dali_memory.h"
#include "dali_gear_dt6.h"
#include "dali_gear_dt8.h"
#include "dali_lunatone.h"
#include "dali_steinel.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DALI_HOST_BUILD
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#endif

static volatile bool s_trace_enabled;

/* ---------------------------------------------------------------------------
 * Session
 *
 * One session at a time; see the header for why. s_attached is what makes a
 * second attach fail, and what makes a workflow that outlives its front end
 * stop writing into a sink that has gone away.
 * --------------------------------------------------------------------------*/

static DaliShellSession s_session;
static bool             s_attached;

/*
 * Deferred output ring, for text produced on a task other than the one holding
 * the session. See defer_foreign_task_output in the header for why a socket
 * sink must never be called from the scheduler owner task.
 *
 * Fixed size and fixed line length: this exists to protect bus timing, so it
 * must not allocate and must not grow under load. A full ring drops the newest
 * line and counts it.
 */
#define SHELL_DEFERRED_LINES   16u
#define SHELL_DEFERRED_LINE_MAX 128u

static char     s_deferred[SHELL_DEFERRED_LINES][SHELL_DEFERRED_LINE_MAX];
static uint8_t  s_deferred_head;
static uint8_t  s_deferred_count;
static uint32_t s_deferred_dropped;

#ifndef DALI_HOST_BUILD
static portMUX_TYPE s_deferred_mux = portMUX_INITIALIZER_UNLOCKED;
#define SHELL_DEFERRED_ENTER() taskENTER_CRITICAL(&s_deferred_mux)
#define SHELL_DEFERRED_EXIT()  taskEXIT_CRITICAL(&s_deferred_mux)
/* The task that called dali_shell_attach(); output from any other task is
 * foreign and is deferred rather than written straight through. */
static TaskHandle_t s_session_task;
#define SHELL_ON_SESSION_TASK() (xTaskGetCurrentTaskHandle() == s_session_task)
#else
#define SHELL_DEFERRED_ENTER()  do {} while (0)
#define SHELL_DEFERRED_EXIT()   do {} while (0)
#define SHELL_ON_SESSION_TASK() true
#endif

/*
 * Line assembly for dali_shell_feed_byte(). Module scope rather than function
 * statics so that attaching clears it: a session that dropped mid-line would
 * otherwise leave its unterminated text to be prepended to the first command
 * the next operator types.
 */
static char    s_line[DALI_SHELL_LINE_MAX];
static uint8_t s_line_len;
static bool    s_line_overflowed;
/* Set after a CR, so the LF of a CRLF pair can be swallowed rather than read as
 * a second, empty line. */
static bool    s_line_last_was_cr;

static void shell_line_reset(void)
{
    s_line_len         = 0u;
    s_line_overflowed  = false;
    s_line_last_was_cr = false;
}

static void shell_deferred_reset(void)
{
    SHELL_DEFERRED_ENTER();
    s_deferred_head    = 0u;
    s_deferred_count   = 0u;
    s_deferred_dropped = 0u;
    SHELL_DEFERRED_EXIT();
}

static void shell_deferred_push(const char *text)
{
    SHELL_DEFERRED_ENTER();
    if (s_deferred_count >= SHELL_DEFERRED_LINES) {
        s_deferred_dropped++;
        SHELL_DEFERRED_EXIT();
        return;
    }
    uint8_t slot = (uint8_t)((s_deferred_head + s_deferred_count) % SHELL_DEFERRED_LINES);
    /* Truncation here costs the tail of one trace line, which is preferable to
     * either allocating or holding the critical section any longer. The
     * terminator is restored for the same reason shell_printf() restores it: a
     * queued line that loses its "\r\n" is flushed into the stream ahead of a
     * prompt and desynchronises a client reading up to one. */
    size_t len = strlen(text);
    if (len >= SHELL_DEFERRED_LINE_MAX) {
        len = SHELL_DEFERRED_LINE_MAX - 3u;
        memcpy(s_deferred[slot], text, len);
        s_deferred[slot][len++] = '\r';
        s_deferred[slot][len++] = '\n';
    } else {
        memcpy(s_deferred[slot], text, len);
    }
    s_deferred[slot][len] = '\0';
    s_deferred_count++;
    SHELL_DEFERRED_EXIT();
}

/* Pop one line into caller storage; false when the ring is empty. */
static bool shell_deferred_pop(char *out, size_t cap)
{
    bool popped = false;
    SHELL_DEFERRED_ENTER();
    if (s_deferred_count > 0u) {
        const char *src = s_deferred[s_deferred_head];
        size_t len = strlen(src);
        if (len >= cap) {
            len = cap - 1u;
        }
        memcpy(out, src, len);
        out[len] = '\0';
        s_deferred_head = (uint8_t)((s_deferred_head + 1u) % SHELL_DEFERRED_LINES);
        s_deferred_count--;
        popped = true;
    }
    SHELL_DEFERRED_EXIT();
    return popped;
}

/*
 * Every byte the shell emits goes through here.
 *
 * The serial CLI this grew from called printf() directly, which is why a
 * network front end could not exist: its output went to the UART regardless of
 * who asked for it. Routing through the session's sink is what lets the same
 * verb print to a socket, to stdout, or into a test buffer without knowing
 * which.
 *
 * Output produced while nothing is attached is discarded rather than dropped
 * onto the UART, so a workflow finishing after its operator disconnected stays
 * silent instead of interleaving with the ESPHome logger.
 */
static void shell_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static void shell_sink_write(void *ctx, const char *text);

static void shell_printf(const char *fmt, ...)
{
    if (!s_attached || s_session.out.write == NULL) {
        return;
    }

    /* Same fixed chunk dali_cli_printf() uses. */
    char    buf[DALI_CLI_FORMAT_MAX];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    /*
     * A line longer than the buffer is clipped, and what it loses first is its
     * own "\r\n". That is not a cosmetic loss: the prompt is the only text this
     * shell writes without a terminator before it, which is what lets a client
     * read a reply up to the prompt and know it is complete. A clipped line
     * leaves the prompt written onto its end, and every such client waits for a
     * prompt that has already gone past. Restoring the terminator costs the
     * last two characters of a line that was being cut anyway.
     */
    if ((size_t)written >= sizeof(buf)) {
        buf[sizeof(buf) - 3u] = '\r';
        buf[sizeof(buf) - 2u] = '\n';
        buf[sizeof(buf) - 1u] = '\0';
    }
    shell_sink_write(NULL, buf);
}

/*
 * The sink the shared dali_cli_* formatters print through, and the one place
 * every byte of shell output passes.
 *
 * Output raised on a foreign task is queued rather than written: on this build
 * that means `trace on` lines from the scheduler owner task, which must not be
 * allowed to block on a socket. See defer_foreign_task_output in the header.
 */
static void shell_sink_write(void *ctx, const char *text)
{
    (void)ctx;
    if (!s_attached || s_session.out.write == NULL || text == NULL) {
        return;
    }
    if (s_session.defer_foreign_task_output && !SHELL_ON_SESSION_TASK()) {
        shell_deferred_push(text);
        return;
    }
    s_session.out.write(s_session.out.ctx, text);
}

/*
 * True when the front end has gone away mid-workflow. Long walks poll this
 * between steps so a discover with forty addresses left stops holding the bus
 * for a reader that has disconnected.
 */
static bool shell_aborted(void)
{
    if (!s_attached) {
        return true;
    }
    if (s_session.aborted == NULL) {
        return false;
    }
    return s_session.aborted(s_session.abort_ctx);
}

/* Claim the bus for a named long-running workflow, when the integration cares. */
static bool shell_bus_claim(const char *what)
{
    if (s_session.hooks.bus_claim == NULL) {
        return true;
    }
    return s_session.hooks.bus_claim(s_session.hooks.ctx, what);
}

static void shell_bus_release(void)
{
    if (s_session.hooks.bus_release != NULL) {
        s_session.hooks.bus_release(s_session.hooks.ctx);
    }
}

/* Report a verb the session policy does not permit, in one wording. */
static bool shell_policy_allows(uint8_t flag, const char *verb)
{
    if ((s_session.policy & flag) != 0u) {
        return true;
    }
    shell_printf("%s: refused by session policy\r\n", verb);
    return false;
}

/* ---------------------------------------------------------------------------
 * Synchronous scheduler helper — device builds only
 *
 * Enqueues one transaction and blocks the calling task via FreeRTOS task
 * notification until the completion callback fires (or 200 ms elapses).
 * The DALI processing task must be calling dali_sched_run() concurrently.
 * --------------------------------------------------------------------------*/
#ifndef DALI_HOST_BUILD

#define SHELL_SYNC_SLOT_COUNT 4u
#define SHELL_SYNC_WAIT_MS  200u
#define SHELL_RESET_WAIT_MS 1000u
#define SHELL_IDENTIFY_CYCLES 5u
#define SHELL_IDENTIFY_STEP_MS 1000u
#define SHELL_FIND_SWITCH_DEFAULT_SECONDS 30u
#define SHELL_FIND_SWITCH_MAX_SECONDS 300u
#define SHELL_SWITCH_MAPPING_MAX 32u
#define SHELL_INPUT_CACHE_MAX 16u
#define SHELL_SENSOR_VALUE_CACHE_MAX 64u
#define SHELL_CAPTURE_MAX 128u

typedef struct {
    TaskHandle_t waiting_task;
    DaliError    result;
    DaliFrame    reply;
    bool         has_reply;
    DaliSequenceResult sequence;
    bool         has_sequence;
    bool         in_use;
    bool         complete;
} DiagSyncCtx;

typedef struct {
    bool           valid;
    uint8_t        order;
    DaliInputEvent event;
    uint32_t       first_seen_us;
    uint32_t       seen_count;
} DiagSwitchMapping;

typedef struct {
    bool                     valid;
    DaliDiscoveryInputDevice input;
} DiagInputCacheEntry;

typedef enum {
    SHELL_CAPTURE_TX = 0,
    SHELL_CAPTURE_RX,
    SHELL_CAPTURE_EVENT,
} DiagCaptureKind;

typedef struct {
    bool              valid;
    DiagCaptureKind   kind;
    DaliFrame         frame;
    DaliInputEvent    event;
    bool              has_event;
    uint32_t          timestamp_us;
    uint32_t          since_tx_us;
    bool              has_since_tx;
} DiagCaptureRecord;

typedef struct {
    bool      valid;
    uint8_t   address;
    uint8_t   instance;
    bool      has_resolution;
    uint8_t   resolution;
    uint8_t   expected_bytes;
    DaliError result;
    uint32_t  timestamp_us;
    uint32_t  value;
    uint8_t   byte_count;
    bool      complete;
    DaliError byte_errors[4];
} DiagSensorValueCacheEntry;

/*
 * The sink handed to the shared dali_cli_* formatters, so the device and the
 * host tests exercise the same formatting code. It forwards to whichever front
 * end currently holds the session rather than to stdout.
 */
static const DaliCliOut s_out = { .write = shell_sink_write, .ctx = NULL };

static DiagSyncCtx s_diag_sync_slots[SHELL_SYNC_SLOT_COUNT];
static portMUX_TYPE s_diag_sync_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_diag_state_mux = portMUX_INITIALIZER_UNLOCKED;
static DaliFrame s_last_rx_frame;
static uint32_t  s_last_rx_timestamp_us;
static uint32_t  s_last_rx_since_tx_us;
static bool      s_last_rx_has_since_tx;
static bool      s_has_last_rx_frame;
static DaliDiscoveryInventory s_inventory;
/*
 * Working copy for every verb that builds or edits an inventory before
 * committing it to s_inventory.
 *
 * It must not be a stack local: the struct is nearly 5 KB against the shell
 * task's 8 KB stack, and a walk nests the discovery and transport frames on top
 * of it. As an automatic it overflowed the task and panicked the chip the
 * instant `discover` was typed — the ESPHome scan task keeps the same struct off
 * its own stack for the same reason.
 *
 * One buffer serves all of them because dali_shell_attach() admits a single
 * session and the session task runs one verb at a time, so no two of these
 * workflows are ever in flight together. Each user fills it completely before
 * reading it — dali_discovery_scan() resets it, shell_inventory_snapshot()
 * overwrites it — so nothing carries across commands.
 */
static DaliDiscoveryInventory s_inventory_scratch;
static DaliInputEventQueue s_event_queue;
static DiagSwitchMapping s_switch_mappings[SHELL_SWITCH_MAPPING_MAX];
static uint8_t s_switch_mapping_count;
static DiagInputCacheEntry s_input_cache[SHELL_INPUT_CACHE_MAX];
static DiagSensorValueCacheEntry s_sensor_value_cache[SHELL_SENSOR_VALUE_CACHE_MAX];
static DiagCaptureRecord s_capture[SHELL_CAPTURE_MAX];
static DiagCaptureRecord s_capture_export[SHELL_CAPTURE_MAX];
static uint8_t s_capture_head;
static uint8_t s_capture_count;
static uint32_t s_capture_dropped;
static bool s_capture_enabled;

static DiagSyncCtx *shell_sync_alloc_slot(TaskHandle_t waiting_task)
{
    DiagSyncCtx *slot = NULL;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    for (uint8_t i = 0u; i < SHELL_SYNC_SLOT_COUNT; i++) {
        if (!s_diag_sync_slots[i].in_use) {
            s_diag_sync_slots[i] = (DiagSyncCtx){
                .waiting_task = waiting_task,
                .result       = DALI_ERR_TIMEOUT,
                .reply        = {0u, 0u},
                .has_reply    = false,
                .sequence     = { .failed_step = DALI_SEQUENCE_NO_FAILED_STEP },
                .has_sequence = false,
                .in_use       = true,
                .complete     = false,
            };
            slot = &s_diag_sync_slots[i];
            break;
        }
    }
    taskEXIT_CRITICAL(&s_diag_sync_mux);

    return slot;
}

static void shell_sync_release_slot(DiagSyncCtx *ctx)
{
    if (ctx == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_sync_mux);
    ctx->waiting_task = NULL;
    ctx->in_use       = false;
    ctx->complete     = false;
    taskEXIT_CRITICAL(&s_diag_sync_mux);
}

static void shell_sync_cb(DaliError result, const DaliFrame *reply, void *cb_ctx)
{
    DiagSyncCtx *ctx = (DiagSyncCtx *)cb_ctx;
    if (ctx == NULL) {
        return;
    }

    TaskHandle_t notify_task = NULL;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    ctx->result       = result;
    ctx->has_reply    = (reply != NULL);
    ctx->has_sequence = false;
    if (reply != NULL) {
        ctx->reply = *reply;
    }
    ctx->complete = true;
    if (ctx->waiting_task != NULL) {
        notify_task = ctx->waiting_task;
    } else {
        ctx->in_use = false;
    }
    taskEXIT_CRITICAL(&s_diag_sync_mux);

    if (notify_task != NULL) {
        xTaskNotifyGive(notify_task);
    }
}

static void shell_sequence_sync_cb(const DaliSequenceResult *result, void *cb_ctx)
{
    DiagSyncCtx *ctx = (DiagSyncCtx *)cb_ctx;
    if (ctx == NULL || result == NULL) {
        return;
    }

    TaskHandle_t notify_task = NULL;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    ctx->result       = result->result;
    ctx->sequence     = *result;
    ctx->has_sequence = true;
    ctx->has_reply    = dali_sequence_result_last_reply(result, &ctx->reply);
    ctx->complete = true;
    if (ctx->waiting_task != NULL) {
        notify_task = ctx->waiting_task;
    } else {
        ctx->in_use = false;
    }
    taskEXIT_CRITICAL(&s_diag_sync_mux);

    if (notify_task != NULL) {
        xTaskNotifyGive(notify_task);
    }
}

static bool shell_sync_complete(DiagSyncCtx *ctx)
{
    bool complete;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    complete = ctx->complete;
    taskEXIT_CRITICAL(&s_diag_sync_mux);

    return complete;
}

/* Runs on the DALI scheduler owner task inside the reset admission barrier. */
static void shell_reset_owner_cb(void *cb_ctx)
{
    DaliError result = dali_phy_reset();
    shell_sync_cb(result, NULL, cb_ctx);
}

static DaliError shell_reset_sync(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0u);

    DiagSyncCtx *ctx = shell_sync_alloc_slot(current_task);
    if (ctx == NULL) {
        return DALI_ERR_BUSY;
    }

    DaliError err = dali_sched_request_reset(shell_reset_owner_cb, ctx);
    if (err != DALI_OK) {
        shell_sync_release_slot(ctx);
        return err;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(SHELL_RESET_WAIT_MS);
    TickType_t start_tick = xTaskGetTickCount();
    while (!shell_sync_complete(ctx)) {
        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed >= wait_ticks) {
            break;
        }
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks - elapsed);
    }

    DaliError result = DALI_ERR_TIMEOUT;
    bool completed;
    taskENTER_CRITICAL(&s_diag_sync_mux);
    completed = ctx->complete;
    if (completed) {
        result = ctx->result;
        ctx->waiting_task = NULL;
        ctx->in_use = false;
        ctx->complete = false;
    } else {
        /* A late owner callback releases the slot after seeing no waiter. */
        ctx->waiting_task = NULL;
    }
    taskEXIT_CRITICAL(&s_diag_sync_mux);

    if (completed) {
        /* The owner callback notifies just before returning; wait for the
         * scheduler to lower the admission barrier as the final reset fence. */
        while (dali_sched_reset_pending()) {
            vTaskDelay(1u);
        }
    }
    return result;
}

/*
 * Enqueue frame, wait up to SHELL_SYNC_WAIT_MS for completion.
 * retries_left is the scheduler retry budget after the first attempt.
 * reply_out may be NULL when the reply data is not needed.
 */
static DaliError shell_sched_sync(const DaliFrame *frame, bool needs_reply,
                                  uint8_t retries_left, bool send_twice,
                                  DaliFrame *reply_out)
{
    if (frame == NULL) {
        return DALI_ERR_INVALID;
    }
    /*
     * The abort check lives at this level because it is the one blocking
     * primitive every verb reaches the bus through. A walk that shared code
     * owns — dali_discovery_scan() runs all 64 addresses itself — then stops at
     * its next frame without that module needing to know a front end can
     * vanish, and a ten-second identify blink stops on its next step.
     */
    if (shell_aborted()) {
        return DALI_ERR_CANCELLED;
    }

    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0u);

    DiagSyncCtx *ctx = shell_sync_alloc_slot(current_task);
    if (ctx == NULL) {
        return DALI_ERR_BUSY;
    }

    DaliTransaction txn = {
        .frame        = *frame,
        .needs_reply  = needs_reply,
        .send_twice   = send_twice,
        .retries_left = retries_left,
        .on_complete  = shell_sync_cb,
        .cb_ctx       = ctx,
    };
    DaliError err = dali_sched_enqueue(&txn);
    if (err != DALI_OK) {
        shell_sync_release_slot(ctx);
        return err;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(SHELL_SYNC_WAIT_MS);
    TickType_t start_tick = xTaskGetTickCount();
    while (!shell_sync_complete(ctx)) {
        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed >= wait_ticks) {
            break;
        }
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks - elapsed);
    }

    DaliError result = DALI_ERR_TIMEOUT;
    DaliFrame reply = {0u, 0u};
    bool has_reply = false;
    bool completed = false;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    completed = ctx->complete;
    if (completed) {
        result    = ctx->result;
        has_reply = ctx->has_reply;
        reply     = ctx->reply;
        ctx->waiting_task = NULL;
        ctx->in_use       = false;
        ctx->complete     = false;
    } else {
        ctx->waiting_task = NULL;
    }
    taskEXIT_CRITICAL(&s_diag_sync_mux);

    if (completed && reply_out != NULL && has_reply) {
        *reply_out = reply;
    }
    return result;
}

/* Fill an outcome that never reached a completion callback. */
static void shell_sequence_result_init(DaliSequenceResult *result_out,
                                      DaliError result)
{
    if (result_out == NULL) {
        return;
    }

    *result_out = (DaliSequenceResult){
        .result      = result,
        .failed_step = DALI_SEQUENCE_NO_FAILED_STEP,
    };
}

/*
 * Enqueue a sequence and wait for it. result_out receives the per-step outcome,
 * including any backward frame each step produced; pass NULL when only the
 * overall error matters. result_out is written on every path, so callers may
 * read it whatever this returns.
 */
static DaliError shell_sched_sequence_sync(DaliSequence *seq,
                                          DaliSequenceResult *result_out)
{
    shell_sequence_result_init(result_out, DALI_ERR_INVALID);

    if (seq == NULL) {
        return DALI_ERR_INVALID;
    }
    /* Same abort contract as shell_sched_sync(); see the note there. */
    if (shell_aborted()) {
        shell_sequence_result_init(result_out, DALI_ERR_CANCELLED);
        return DALI_ERR_CANCELLED;
    }

    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0u);

    DiagSyncCtx *ctx = shell_sync_alloc_slot(current_task);
    if (ctx == NULL) {
        shell_sequence_result_init(result_out, DALI_ERR_BUSY);
        return DALI_ERR_BUSY;
    }

    seq->on_complete = shell_sequence_sync_cb;
    seq->cb_ctx = ctx;

    DaliError err = dali_sched_enqueue_sequence(seq);
    if (err != DALI_OK) {
        shell_sync_release_slot(ctx);
        shell_sequence_result_init(result_out, err);
        return err;
    }

    /* A multi-step sequence can outlast the single-frame wait several times
     * over, so size the budget from the sequence itself. */
    TickType_t wait_ticks = pdMS_TO_TICKS(dali_transport_sequence_timeout_ms(seq));
    TickType_t start_tick = xTaskGetTickCount();
    while (!shell_sync_complete(ctx)) {
        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed >= wait_ticks) {
            break;
        }
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks - elapsed);
    }

    DaliError result = DALI_ERR_TIMEOUT;
    DaliSequenceResult sequence = { .failed_step = DALI_SEQUENCE_NO_FAILED_STEP };
    bool has_sequence = false;
    bool completed = false;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    completed = ctx->complete;
    if (completed) {
        result       = ctx->result;
        sequence     = ctx->sequence;
        has_sequence = ctx->has_sequence;
        ctx->waiting_task = NULL;
        ctx->in_use       = false;
        ctx->complete     = false;
    } else {
        ctx->waiting_task = NULL;
    }
    taskEXIT_CRITICAL(&s_diag_sync_mux);

    if (result_out != NULL) {
        /* A wait that timed out never reached the completion callback, so carry
         * the wait result on an otherwise empty outcome. */
        if (!has_sequence) {
            sequence.result = result;
        }
        *result_out = sequence;
    }
    return result;
}

static DaliError shell_discovery_transact(const DaliFrame *frame,
                                         bool needs_reply,
                                         uint8_t retries_left,
                                         bool send_twice,
                                         DaliFrame *reply_out,
                                         void *ctx)
{
    (void)ctx;
    return shell_sched_sync(frame,
                           needs_reply,
                           retries_left,
                           send_twice,
                           reply_out);
}

/* Atomic-group half of the transport: shared protocol code that needs a DTR
 * setup and its consuming command to stay together gets that here too, not just
 * in the ESPHome scan task. */
static DaliError shell_discovery_sequence_transact(const DaliSequence *seq,
                                                  DaliSequenceResult *result_out,
                                                  void *ctx)
{
    (void)ctx;
    if (seq == NULL) {
        return DALI_ERR_INVALID;
    }

    /* shell_sched_sequence_sync installs its own completion callback. */
    DaliSequence local = *seq;
    return shell_sched_sequence_sync(&local, result_out);
}

/*
 * The blocking transport every device front end shares. A host test attaches a
 * scripted transport instead, which is what lets the long workflows below be
 * exercised without a bus.
 */
const DaliTransport *dali_shell_device_transport(void)
{
    static const DaliTransport transport = {
        .transact          = shell_discovery_transact,
        .transact_sequence = shell_discovery_sequence_transact,
        .ctx               = NULL,
    };
    return &transport;
}

/* Every workflow reaches the bus through whatever the attached session bound,
 * never through a transport chosen here. */
static DaliDiscoveryTransport shell_discovery_transport(void)
{
    return s_session.transport;
}

static int shell_frame_hex_width(const DaliFrame *frame)
{
    return (int)((frame->bit_length + 3u) / 4u);
}

static void shell_print_frame(const char *prefix, const DaliFrame *frame)
{
    dali_cli_print_frame(&s_out, prefix, frame);
}

static void shell_store_last_rx(const DaliSchedTraceEvent *event)
{
    if (event == NULL || event->direction != DALI_SCHED_TRACE_RX) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    s_last_rx_frame        = event->frame;
    s_last_rx_timestamp_us = event->timestamp_us;
    s_last_rx_since_tx_us  = event->since_tx_us;
    s_last_rx_has_since_tx = event->has_since_tx;
    s_has_last_rx_frame    = true;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void shell_inventory_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    (void)dali_discovery_inventory_reset(&s_inventory);
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void shell_last_rx_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    s_last_rx_frame        = (DaliFrame){0u, 0u};
    s_last_rx_timestamp_us = 0u;
    s_last_rx_since_tx_us  = 0u;
    s_last_rx_has_since_tx = false;
    s_has_last_rx_frame    = false;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void shell_inventory_replace(const DaliDiscoveryInventory *inventory)
{
    if (inventory == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    s_inventory = *inventory;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static bool shell_inventory_snapshot(DaliDiscoveryInventory *out)
{
    if (out == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    *out = s_inventory;
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return out->valid;
}

static void shell_events_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    (void)dali_event_queue_init(&s_event_queue);
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static bool shell_event_pop(DaliInputEventRecord *out)
{
    bool popped;

    taskENTER_CRITICAL(&s_diag_state_mux);
    popped = dali_event_queue_pop(&s_event_queue, out);
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return popped;
}

static uint32_t shell_event_queue_dropped_snapshot(void)
{
    uint32_t dropped;

    taskENTER_CRITICAL(&s_diag_state_mux);
    dropped = dali_event_queue_dropped(&s_event_queue);
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return dropped;
}

static uint8_t shell_event_queue_count_snapshot(void)
{
    uint8_t count;

    taskENTER_CRITICAL(&s_diag_state_mux);
    count = dali_event_queue_count(&s_event_queue);
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return count;
}

static void shell_switch_mappings_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    memset(s_switch_mappings, 0, sizeof(s_switch_mappings));
    s_switch_mapping_count = 0u;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void shell_input_cache_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    memset(s_input_cache, 0, sizeof(s_input_cache));
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void shell_sensor_value_cache_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    memset(s_sensor_value_cache, 0, sizeof(s_sensor_value_cache));
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void shell_sensor_value_cache_store(uint8_t addr,
                                          const DaliInputInstanceInfo *info,
                                          uint8_t expected_bytes,
                                          DaliError result,
                                          const DaliInputPollResult *poll)
{
    if (info == NULL || addr >= DALI_SHORT_ADDRESS_COUNT ||
        info->instance >= DALI_INPUT_MAX_INSTANCES) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    uint8_t slot = SHELL_SENSOR_VALUE_CACHE_MAX;
    for (uint8_t i = 0u; i < SHELL_SENSOR_VALUE_CACHE_MAX; i++) {
        if (s_sensor_value_cache[i].valid &&
            s_sensor_value_cache[i].address == addr &&
            s_sensor_value_cache[i].instance == info->instance) {
            slot = i;
            break;
        }
        if (!s_sensor_value_cache[i].valid && slot == SHELL_SENSOR_VALUE_CACHE_MAX) {
            slot = i;
        }
    }
    if (slot == SHELL_SENSOR_VALUE_CACHE_MAX) {
        slot = (uint8_t)(((uint16_t)addr + info->instance) %
                         SHELL_SENSOR_VALUE_CACHE_MAX);
    }

    DiagSensorValueCacheEntry *entry = &s_sensor_value_cache[slot];
    *entry = (DiagSensorValueCacheEntry){
        .valid          = true,
        .address        = addr,
        .instance       = info->instance,
        .has_resolution = info->has_resolution,
        .resolution     = info->resolution,
        .expected_bytes = expected_bytes,
        .result         = result,
        .timestamp_us   = (uint32_t)esp_timer_get_time(),
        .value          = poll != NULL ? poll->value.value : 0u,
        .byte_count     = poll != NULL ? poll->value.byte_count : 0u,
        .complete       = poll != NULL ? poll->value.complete : false,
    };
    for (uint8_t i = 0u; i < 4u; i++) {
        entry->byte_errors[i] = poll != NULL ? poll->byte_errors[i] : DALI_ERR_INVALID;
    }
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static bool shell_sensor_value_cache_lookup(uint8_t addr,
                                           uint8_t instance,
                                           DiagSensorValueCacheEntry *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT ||
        instance >= DALI_INPUT_MAX_INSTANCES) {
        return false;
    }

    bool found = false;
    taskENTER_CRITICAL(&s_diag_state_mux);
    for (uint8_t i = 0u; i < SHELL_SENSOR_VALUE_CACHE_MAX; i++) {
        if (s_sensor_value_cache[i].valid &&
            s_sensor_value_cache[i].address == addr &&
            s_sensor_value_cache[i].instance == instance) {
            *out = s_sensor_value_cache[i];
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return found;
}

static void shell_input_cache_store(const DaliDiscoveryInputDevice *input)
{
    if (input == NULL || input->device.address >= DALI_SHORT_ADDRESS_COUNT) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    uint8_t slot = SHELL_INPUT_CACHE_MAX;
    for (uint8_t i = 0u; i < SHELL_INPUT_CACHE_MAX; i++) {
        if (s_input_cache[i].valid &&
            s_input_cache[i].input.device.address == input->device.address) {
            slot = i;
            break;
        }
        if (!s_input_cache[i].valid && slot == SHELL_INPUT_CACHE_MAX) {
            slot = i;
        }
    }
    if (slot == SHELL_INPUT_CACHE_MAX) {
        slot = (uint8_t)(input->device.address % SHELL_INPUT_CACHE_MAX);
    }
    s_input_cache[slot] = (DiagInputCacheEntry){
        .valid = true,
        .input = *input,
    };
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static bool shell_input_cache_lookup(uint8_t addr, DaliDiscoveryInputDevice *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return false;
    }

    bool found = false;

    taskENTER_CRITICAL(&s_diag_state_mux);
    for (uint8_t i = 0u; i < SHELL_INPUT_CACHE_MAX; i++) {
        if (s_input_cache[i].valid &&
            s_input_cache[i].input.device.address == addr) {
            *out = s_input_cache[i].input;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return found;
}

static bool shell_event_is_switch_candidate(const DaliInputEvent *event)
{
    if (dali_event_is_switch_mapping_candidate(event)) {
        return true;
    }
    if (event == NULL ||
        event->frame_kind != DALI_EVENT_FRAME_INPUT_24BIT ||
        event->source.scheme != DALI_EVENT_SOURCE_DEVICE_INSTANCE ||
        !event->source.has_device_address ||
        !event->source.has_instance ||
        event->event_information != DALI_DT301_EVENT_DOUBLE_PRESS) {
        return false;
    }

    /* Device/Instance events omit instance type. Resolve it from the most
     * recent input-device discovery rather than guessing from the event value. */
    DaliDiscoveryInputDevice input;
    if (!shell_input_cache_lookup(event->source.device_address, &input) ||
        !input.device.has_instance_count ||
        event->source.instance >= input.device.instance_count ||
        event->source.instance >= DALI_INPUT_MAX_INSTANCES) {
        return false;
    }

    uint8_t instance = event->source.instance;
    const DaliInputInstanceInfo *info = &input.device.instances[instance];
    return input.instance_type_errors[instance] == DALI_OK &&
           info->has_type &&
           info->type == DALI_INPUT_INSTANCE_TYPE_PUSH_BUTTON;
}

static bool shell_event_same_source(const DaliInputEvent *a, const DaliInputEvent *b)
{
    return a != NULL && b != NULL &&
           a->frame_kind == b->frame_kind &&
           a->raw.bit_length == b->raw.bit_length &&
           a->raw.data == b->raw.data;
}

static bool shell_record_switch_mapping(const DaliInputEventRecord *record)
{
    if (record == NULL) {
        return false;
    }

    bool recorded = false;

    taskENTER_CRITICAL(&s_diag_state_mux);
    for (uint8_t i = 0u; i < s_switch_mapping_count; i++) {
        if (shell_event_same_source(&s_switch_mappings[i].event, &record->event)) {
            s_switch_mappings[i].seen_count++;
            taskEXIT_CRITICAL(&s_diag_state_mux);
            return false;
        }
    }

    if (s_switch_mapping_count < SHELL_SWITCH_MAPPING_MAX) {
        DiagSwitchMapping *mapping = &s_switch_mappings[s_switch_mapping_count];
        *mapping = (DiagSwitchMapping){
            .valid         = true,
            .order         = (uint8_t)(s_switch_mapping_count + 1u),
            .event         = record->event,
            .first_seen_us = record->timestamp_us,
            .seen_count    = 1u,
        };
        s_switch_mapping_count++;
        recorded = true;
    }
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return recorded;
}

static uint8_t shell_switch_mappings_snapshot(DiagSwitchMapping *out,
                                             uint8_t capacity)
{
    uint8_t total;
    uint8_t count;

    taskENTER_CRITICAL(&s_diag_state_mux);
    total = s_switch_mapping_count;
    count = total;
    if (out != NULL && count > 0u) {
        if (count > capacity) {
            count = capacity;
        }
        memcpy(out, s_switch_mappings, sizeof(out[0]) * count);
    }
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return total;
}

static const char *shell_capture_kind_name(DiagCaptureKind kind)
{
    switch (kind) {
        case SHELL_CAPTURE_TX:
            return "tx";
        case SHELL_CAPTURE_RX:
            return "rx";
        case SHELL_CAPTURE_EVENT:
            return "event";
        default:
            return "unknown";
    }
}

static const char *shell_sched_state_name(DaliSchedState state)
{
    switch (state) {
        case SCHED_IDLE:
            return "idle";
        case SCHED_TX:
            return "tx";
        case SCHED_WAIT_SETTLE:
            return "wait-settle";
        case SCHED_WAIT_REPLY:
            return "wait-reply";
        default:
            return "unknown";
    }
}

static void shell_capture_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_head = 0u;
    s_capture_count = 0u;
    s_capture_dropped = 0u;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void shell_capture_set_enabled(bool enabled)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    s_capture_enabled = enabled;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void shell_capture_push(const DiagCaptureRecord *record)
{
    if (record == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    if (!s_capture_enabled) {
        taskEXIT_CRITICAL(&s_diag_state_mux);
        return;
    }

    uint8_t index;
    if (s_capture_count < SHELL_CAPTURE_MAX) {
        index = (uint8_t)((s_capture_head + s_capture_count) % SHELL_CAPTURE_MAX);
        s_capture_count++;
    } else {
        index = s_capture_head;
        s_capture_head = (uint8_t)((s_capture_head + 1u) % SHELL_CAPTURE_MAX);
        s_capture_dropped++;
    }
    s_capture[index] = *record;
    s_capture[index].valid = true;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void shell_capture_push_trace(const DaliSchedTraceEvent *event)
{
    if (event == NULL) {
        return;
    }

    DiagCaptureRecord record = {
        .valid        = true,
        .kind         = event->direction == DALI_SCHED_TRACE_TX
                      ? SHELL_CAPTURE_TX
                      : SHELL_CAPTURE_RX,
        .frame        = event->frame,
        .timestamp_us = event->timestamp_us,
        .since_tx_us  = event->since_tx_us,
        .has_since_tx = event->has_since_tx,
    };
    shell_capture_push(&record);
}

static void shell_capture_push_event(const DaliInputEventRecord *event_record)
{
    if (event_record == NULL) {
        return;
    }

    DiagCaptureRecord record = {
        .valid        = true,
        .kind         = SHELL_CAPTURE_EVENT,
        .frame        = event_record->event.raw,
        .event        = event_record->event,
        .has_event    = true,
        .timestamp_us = event_record->timestamp_us,
    };
    shell_capture_push(&record);
}

static uint8_t shell_capture_snapshot(DiagCaptureRecord *out,
                                     uint8_t capacity,
                                     uint32_t *dropped_out,
                                     bool *enabled_out)
{
    uint8_t count;

    taskENTER_CRITICAL(&s_diag_state_mux);
    count = s_capture_count;
    if (out != NULL && capacity > 0u) {
        uint8_t copy_count = count > capacity ? capacity : count;
        for (uint8_t i = 0u; i < copy_count; i++) {
            uint8_t index = (uint8_t)((s_capture_head + i) % SHELL_CAPTURE_MAX);
            out[i] = s_capture[index];
        }
    }
    if (dropped_out != NULL) {
        *dropped_out = s_capture_dropped;
    }
    if (enabled_out != NULL) {
        *enabled_out = s_capture_enabled;
    }
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return count;
}

static void shell_print_event_json_fields(const DaliInputEvent *event)
{
    if (event == NULL) {
        return;
    }

    shell_printf(", \"frame_kind\": \"%s\"",
           dali_event_frame_kind_name(event->frame_kind));

    if (event->frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT) {
        shell_printf(", \"address_byte\": %u, \"address_byte_hex\": \"0x%02X\"",
               (unsigned)event->address_byte,
               (unsigned)event->address_byte);
        shell_printf(", \"address_kind\": \"%s\"",
               dali_event_address_kind_name(event->address_kind));
        if (event->address_kind == DALI_EVENT_ADDRESS_SHORT ||
            event->address_kind == DALI_EVENT_ADDRESS_GROUP) {
            shell_printf(", \"address\": %u", (unsigned)event->address);
        }
        shell_printf(", \"selector\": %s",
               event->address_selector ? "true" : "false");
        shell_printf(", \"action_code\": %u", (unsigned)event->legacy_data);
        shell_printf(", \"action_hex\": \"0x%02X\"",
               (unsigned)event->legacy_data);
        shell_printf(", \"action\": \"%s\"", dali_event_action_name(event));
        return;
    }

    shell_printf(", \"source_scheme\": \"%s\"",
           dali_event_source_scheme_name(event->source.scheme));
    if (event->source.has_device_address) {
        shell_printf(", \"device_address\": %u",
               (unsigned)event->source.device_address);
    }
    if (event->source.has_device_group) {
        shell_printf(", \"device_group\": %u",
               (unsigned)event->source.device_group);
    }
    if (event->source.has_instance) {
        shell_printf(", \"instance\": %u", (unsigned)event->source.instance);
    }
    if (event->source.has_instance_group) {
        shell_printf(", \"instance_group\": %u",
               (unsigned)event->source.instance_group);
    }
    if (event->source.has_instance_type) {
        shell_printf(", \"instance_type\": %u",
               (unsigned)event->source.instance_type);
    }

    if (event->frame_kind == DALI_EVENT_FRAME_INPUT_24BIT) {
        shell_printf(", \"event_information\": %u",
               (unsigned)event->event_information);
        shell_printf(", \"event_information_hex\": \"0x%03X\"",
               (unsigned)event->event_information);
    }
    shell_printf(", \"action\": \"%s\"", dali_event_action_name(event));
}

static void shell_print_capture_json(void)
{
    uint32_t dropped = 0u;
    bool enabled = false;
    uint8_t count = shell_capture_snapshot(s_capture_export,
                                          SHELL_CAPTURE_MAX,
                                          &dropped,
                                          &enabled);

    shell_printf("  \"capture\": {\r\n");
    shell_printf("    \"enabled\": %s,\r\n", enabled ? "true" : "false");
    shell_printf("    \"count\": %u,\r\n", (unsigned)count);
    shell_printf("    \"dropped\": %" PRIu32 ",\r\n", dropped);
    shell_printf("    \"records\": [\r\n");
    for (uint8_t i = 0u; i < count; i++) {
        const DiagCaptureRecord *record = &s_capture_export[i];
        if (i > 0u) {
            shell_printf(",\r\n");
        }
        shell_printf("      { \"kind\": \"%s\", \"timestamp_us\": %" PRIu32,
               shell_capture_kind_name(record->kind),
               record->timestamp_us);
        shell_printf(", \"raw\": \"0x%0*" PRIX32 "\"",
               shell_frame_hex_width(&record->frame),
               record->frame.data);
        shell_printf(", \"raw_bits\": %u", (unsigned)record->frame.bit_length);
        if (record->has_since_tx) {
            shell_printf(", \"since_tx_us\": %" PRIu32, record->since_tx_us);
        }
        if (record->has_event) {
            shell_print_event_json_fields(&record->event);
        }
        shell_printf(" }");
    }
    shell_printf("\r\n    ]\r\n");
    shell_printf("  }");
}

static void shell_print_event_record(const char *prefix,
                                    const DaliInputEventRecord *record)
{
    if (record == NULL) {
        return;
    }

    const DaliInputEvent *event = &record->event;
    shell_printf("%sraw=0x%0*" PRIX32 " frame=%s",
           prefix,
           shell_frame_hex_width(&event->raw),
           event->raw.data,
           dali_event_frame_kind_name(event->frame_kind));

    if (event->frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT) {
        shell_printf(" addr_byte=0x%02X kind=%s",
               (unsigned)event->address_byte,
               dali_event_address_kind_name(event->address_kind));
        if (event->address_kind == DALI_EVENT_ADDRESS_SHORT ||
            event->address_kind == DALI_EVENT_ADDRESS_GROUP) {
            shell_printf(" addr=%u", (unsigned)event->address);
        }
        shell_printf(" selector=%u", event->address_selector ? 1u : 0u);
        shell_printf(" action=0x%02X %s time=%" PRIu32 "us\r\n",
               (unsigned)event->legacy_data,
               dali_event_action_name(event),
               record->timestamp_us);
        return;
    }

    shell_printf(" source=%s",
           dali_event_source_scheme_name(event->source.scheme));
    if (event->source.has_device_address) {
        shell_printf(" device_address=%u", (unsigned)event->source.device_address);
    }
    if (event->source.has_device_group) {
        shell_printf(" device_group=%u", (unsigned)event->source.device_group);
    }
    if (event->source.has_instance) {
        shell_printf(" instance=%u", (unsigned)event->source.instance);
    }
    if (event->source.has_instance_group) {
        shell_printf(" instance_group=%u", (unsigned)event->source.instance_group);
    }
    if (event->source.has_instance_type) {
        shell_printf(" instance_type=%u", (unsigned)event->source.instance_type);
    }

    if (event->frame_kind == DALI_EVENT_FRAME_INPUT_24BIT) {
        shell_printf(" event_information=0x%03X %s",
               (unsigned)event->event_information,
               dali_event_action_name(event));
    } else {
        shell_printf(" %s", dali_event_action_name(event));
    }
    shell_printf(" time=%" PRIu32 "us\r\n", record->timestamp_us);
}

/*
 * Scheduler subscribers. Registered once by dali_shell_init() and never
 * removed, so both run whether or not a front end holds the session; with
 * nothing attached they simply fill caches that the next attach clears.
 */
void dali_shell_on_event(const DaliFrame *frame, void *ctx)
{
    (void)ctx;

    DaliInputEventRecord record = {
        .timestamp_us = (uint32_t)esp_timer_get_time(),
    };
    if (dali_event_parse_frame(frame, &record.event) != DALI_OK) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    (void)dali_event_queue_push(&s_event_queue, &record);
    taskEXIT_CRITICAL(&s_diag_state_mux);

    shell_capture_push_event(&record);
}

void dali_shell_on_trace(const DaliSchedTraceEvent *event, void *ctx)
{
    (void)ctx;

    shell_store_last_rx(event);
    shell_capture_push_trace(event);

    if (!s_trace_enabled || event == NULL) {
        return;
    }

    const DaliFrame *frame = &event->frame;
    unsigned bits = (unsigned)frame->bit_length;

    if (event->direction == DALI_SCHED_TRACE_TX) {
        shell_printf("[BUS] TX 0x%0*" PRIX32 " (%u-bit)\r\n",
               shell_frame_hex_width(frame),
               frame->data,
               bits);
    } else if (event->has_since_tx) {
        uint32_t tenths_ms = (event->since_tx_us + 50u) / 100u;
        shell_printf("[BUS] RX 0x%0*" PRIX32 " (%u-bit, %" PRIu32 ".%" PRIu32 " ms after TX)\r\n",
               shell_frame_hex_width(frame),
               frame->data,
               bits,
               tenths_ms / 10u,
               tenths_ms % 10u);
    } else {
        shell_printf("[BUS] RX 0x%0*" PRIX32 " (%u-bit)\r\n",
               shell_frame_hex_width(frame),
               frame->data,
               bits);
    }
    fflush(stdout);
}

#endif /* !DALI_HOST_BUILD */

/* ---------------------------------------------------------------------------
 * Command handlers
 *
 * Every handler drives the bus through the blocking scheduler helpers above, so
 * the whole section is device-only. The portable half of the CLI — tokenising,
 * the verb table, argument validation, and response formatting — is in
 * dali_cli.c, which the host suite builds and exercises directly.
 * --------------------------------------------------------------------------*/
#ifndef DALI_HOST_BUILD

static void cmd_stats(void)
{
#ifndef DALI_HOST_BUILD
    uint32_t capture_dropped = 0u;
    bool capture_enabled = false;
    uint8_t capture_count =
        shell_capture_snapshot(NULL, 0u, &capture_dropped, &capture_enabled);

    shell_printf("RX overflow:      %" PRIu32 "\r\n", g_dali_stats.rx_overflow);
    shell_printf("TX retries:       %" PRIu32 "\r\n", g_dali_stats.tx_retries);
    shell_printf("Malformed frames: %" PRIu32 "\r\n", g_dali_stats.malformed_frames);
    shell_printf("Reply timeouts:   %" PRIu32 "\r\n", g_dali_stats.reply_timeouts);
    shell_printf("RX ignored:       %" PRIu32 "\r\n", g_dali_stats.rx_ignored_outside_reply);
    shell_printf("RX events routed: %" PRIu32 "\r\n", g_dali_stats.unsolicited_events_routed);
    shell_printf("Raw malformed:    %" PRIu32 "\r\n", g_dali_stats.raw_malformed);
    shell_printf("ISR overruns:     %" PRIu32 "\r\n", g_dali_stats.isr_overruns);
    shell_printf("Bus idle fails:   %" PRIu32 "\r\n", g_dali_stats.bus_idle_failures);
    shell_printf("RX TX echo drop:  %" PRIu32 "\r\n", g_dali_stats.rx_self_echo_suppressed);
    shell_printf("RX settle drop:   %" PRIu32 "\r\n", g_dali_stats.rx_settle_suppressed);
    shell_printf("RX glitch drop:   %" PRIu32 "\r\n", g_dali_stats.rx_glitch_drops);
    shell_printf("Event queued:     %u\r\n", (unsigned)shell_event_queue_count_snapshot());
    shell_printf("Event dropped:    %" PRIu32 "\r\n", shell_event_queue_dropped_snapshot());
    shell_printf("Capture:          %s, %u queued, %" PRIu32 " dropped\r\n",
           capture_enabled ? "on" : "off",
           (unsigned)capture_count,
           capture_dropped);

    DaliSchedQueueStats q;
    if (dali_sched_queue_stats(&q) == DALI_OK) {
        shell_printf("Queue depth:      %u/%u (high-water %u)\r\n",
               (unsigned)q.depth, (unsigned)q.capacity, (unsigned)q.high_water);
        shell_printf("Queue admitted:   %" PRIu32 "\r\n", q.admitted);
        shell_printf("Queue dropped:    %" PRIu32 " full, %" PRIu32 " busy\r\n",
               q.rejected_full, q.rejected_busy);
    }
#endif
}

/*
 * A rejected submission is never retried by the scheduler, so `full`/`busy` are
 * commands that did not reach the bus. high-water at capacity means admission
 * came within one submission of failing even while both counters read zero.
 */
static void cmd_queue(const DaliCliTokens *t)
{
    if (t->count == 2u) {
        if (!dali_cli_has_subcommand(dali_cli_command_for_id(DALI_CLI_CMD_QUEUE),
                                     t->tok[1])) {
            dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_QUEUE));
            return;
        }
        dali_sched_reset_queue_stats();
    }

    DaliSchedQueueStats q;
    if (dali_sched_queue_stats(&q) != DALI_OK) {
        dali_cli_print_error(&s_out, "queue", DALI_ERR_INVALID);
        return;
    }
    shell_printf("depth %u/%u  high-water %u  admitted %" PRIu32
           "  dropped %" PRIu32 " full / %" PRIu32 " busy\r\n",
           (unsigned)q.depth, (unsigned)q.capacity, (unsigned)q.high_water,
           q.admitted, q.rejected_full, q.rejected_busy);
}

static void cmd_trace(const DaliCliTokens *t)
{
    if (!dali_cli_has_subcommand(dali_cli_command_for_id(DALI_CLI_CMD_TRACE),
                                 t->tok[1])) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_TRACE));
        return;
    }

    if (strcmp(t->tok[1], "on") == 0) {
        s_trace_enabled = true;
        shell_printf("trace on\r\n");
    } else if (strcmp(t->tok[1], "off") == 0) {
        s_trace_enabled = false;
        shell_printf("trace off\r\n");
    } else {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_TRACE));
    }
}

static void cmd_reset(void)
{
#ifndef DALI_HOST_BUILD
    DaliError err = shell_reset_sync();
    if (err != DALI_OK) {
        shell_printf("reset: ERR %d\r\n", (int)err);
        return;
    }

    shell_last_rx_reset();
    shell_inventory_reset();
    shell_events_reset();
    shell_switch_mappings_reset();
    shell_input_cache_reset();
    shell_sensor_value_cache_reset();
    shell_capture_reset();
    shell_printf("reset OK\r\n");
#else
    if (dali_sched_reset() == DALI_OK) {
        dali_sched_run();
        (void)dali_phy_reset();
    }
#endif
}

static void cmd_read(void)
{
#ifndef DALI_HOST_BUILD
    DaliFrame frame;
    uint32_t timestamp_us;
    uint32_t since_tx_us;
    bool has_since_tx;
    bool has_frame;

    taskENTER_CRITICAL(&s_diag_state_mux);
    frame        = s_last_rx_frame;
    timestamp_us = s_last_rx_timestamp_us;
    since_tx_us  = s_last_rx_since_tx_us;
    has_since_tx = s_last_rx_has_since_tx;
    has_frame    = s_has_last_rx_frame;
    taskEXIT_CRITICAL(&s_diag_state_mux);

    if (!has_frame) {
        shell_printf("read: no RX frame captured\r\n");
        return;
    }

    shell_print_frame("read: ", &frame);
    shell_printf("  timestamp: %" PRIu32 " us\r\n", timestamp_us);
    if (has_since_tx) {
        uint32_t tenths_ms = (since_tx_us + 50u) / 100u;
        shell_printf("  after TX:  %" PRIu32 ".%" PRIu32 " ms\r\n",
               tenths_ms / 10u,
               tenths_ms % 10u);
    }
#endif
}

static char shell_rxdebug_bucket(uint32_t interval_us)
{
    const uint32_t half_min = (DALI_HALF_BIT_US * 3u) / 4u;
    const uint32_t half_max = (DALI_HALF_BIT_US * 5u) / 4u;
    const uint32_t full_min = (DALI_BIT_US * 3u) / 4u;
    const uint32_t full_max = (DALI_BIT_US * 5u) / 4u;

    if (interval_us >= half_min && interval_us <= half_max) {
        return 'H';
    }
    if (interval_us >= full_min && interval_us <= full_max) {
        return 'F';
    }
    if (interval_us < half_min) {
        return '<';
    }
    if (interval_us > full_max) {
        return '>';
    }
    return '?';
}

static void cmd_rxdebug(void)
{
#ifndef DALI_HOST_BUILD
    DaliPhyRxDebugSnapshot snapshot;
    DaliError err = dali_phy_get_rx_debug(&snapshot);
    if (err != DALI_OK) {
        shell_printf("rxdebug: ERR %d\r\n", (int)err);
        return;
    }
    if (!snapshot.valid) {
        shell_printf("rxdebug: no malformed RX snapshot\r\n");
        return;
    }

    shell_printf("rxdebug: err=%d, intervals=%u, edges=%u\r\n",
           (int)snapshot.error,
           (unsigned)snapshot.interval_count,
           (unsigned)snapshot.edge_count);

    shell_printf("  edge levels:");
    for (uint8_t i = 0u; i < snapshot.edge_count; i++) {
        shell_printf(" %u", (unsigned)snapshot.edge_levels[i]);
    }
    shell_printf("\r\n");

    shell_printf("  intervals us (H=half, F=full):");
    for (uint8_t i = 0u; i < snapshot.interval_count; i++) {
        if ((i % 8u) == 0u) {
            shell_printf("\r\n    ");
        }
        shell_printf("%" PRIu32 "%c ", snapshot.intervals_us[i],
               shell_rxdebug_bucket(snapshot.intervals_us[i]));
    }
    shell_printf("\r\n");
#endif
}

static DaliError shell_send_no_reply(const DaliFrame *frame, bool send_twice)
{
    return shell_sched_sync(frame, false, 0u, send_twice, NULL);
}

static void shell_print_tx_result(const char *name, DaliError err)
{
    dali_cli_print_tx_result(&s_out, name, err);
}

/* A target that is not a single short address may be answered by several
 * devices at once, or acted on by several at once. Say so before the traffic. */
static void shell_warn_multi_target(const char *name, DaliTarget target, bool is_query)
{
    if (target.type == DALI_ADDR_SHORT) {
        return;
    }
    if (is_query) {
        shell_printf("%s: group/broadcast replies may collide on a real bus\r\n", name);
    } else {
        shell_printf("%s: group/broadcast target may affect multiple devices\r\n", name);
    }
}

static void cmd_level(const DaliCliTokens *t)
{
    DaliTarget target;
    DaliCliLevel level;

    if (!dali_cli_parse_target(t->tok[1], &target) ||
        !dali_cli_parse_level(t->tok[2], &level)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_LEVEL));
        return;
    }

    DaliFrame frame;
    /* MASK is not a level, so it never goes through the DAPC level builder. */
    DaliError err = level.is_mask
                  ? dali_control_build_dapc_mask(target, &frame)
                  : dali_control_build_dapc(target, level.level, &frame);
    if (err == DALI_OK) {
        err = shell_send_no_reply(&frame, false);
    }
    shell_print_tx_result(level.is_mask ? "mask" : "level", err);
}

static void cmd_mask(const DaliCliTokens *t)
{
    DaliTarget target;
    if (!dali_cli_parse_target(t->tok[1], &target)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_MASK));
        return;
    }

    DaliFrame frame;
    DaliError err = dali_control_build_dapc_mask(target, &frame);
    if (err == DALI_OK) {
        err = shell_send_no_reply(&frame, false);
    }
    shell_print_tx_result("mask", err);
}

typedef DaliError (*DiagTargetFrameBuilder)(DaliTarget target, DaliFrame *out);

static void cmd_target_frame(const DaliCliTokens *t,
                             DaliCliCommandId       id,
                             DiagTargetFrameBuilder builder)
{
    const DaliCliCommandSpec *spec = dali_cli_command_for_id(id);
    DaliTarget target;

    if (builder == NULL || !dali_cli_parse_target(t->tok[1], &target)) {
        dali_cli_print_usage(&s_out, spec);
        return;
    }

    DaliFrame frame;
    DaliError err = builder(target, &frame);
    if (err == DALI_OK) {
        err = shell_send_no_reply(&frame, false);
    }
    shell_print_tx_result(spec->name, err);
}

static void cmd_scene(const DaliCliTokens *t)
{
    DaliTarget target;
    uint8_t scene;

    if (!dali_cli_parse_target(t->tok[1], &target) ||
        !dali_cli_parse_u8(t->tok[2], DALI_MAX_SCENE, &scene)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_SCENE));
        return;
    }

    DaliFrame frame;
    DaliError err = dali_control_build_go_to_scene(target, scene, &frame);
    if (err == DALI_OK) {
        err = shell_send_no_reply(&frame, false);
    }
    shell_print_tx_result("scene", err);
}

/*
 * raw sends one frame; raw2 sends the same frame twice inside the 100 ms
 * send-twice window. Two manually typed raw commands cannot meet that deadline,
 * so a send-twice command entered that way is not the command the standard
 * describes — the scheduler's own expansion is the only way to get it right.
 */
static void cmd_raw(const DaliCliTokens *t, bool send_twice)
{
    DaliCliCommandId id = send_twice ? DALI_CLI_CMD_RAW2 : DALI_CLI_CMD_RAW;
    bool wait_reply = false;

    if (!send_twice && t->count == 4u) {
        if (strcmp(t->tok[3], "wait") != 0) {
            g_dali_stats.raw_malformed++;
            dali_cli_print_usage(&s_out, dali_cli_command_for_id(id));
            return;
        }
        wait_reply = true;
    }

    DaliFrame frame;
    if (!dali_cli_parse_raw_frame(t->tok[1], t->tok[2], &frame)) {
        g_dali_stats.raw_malformed++;
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(id));
        return;
    }

    DaliFrame reply = {0u, 0u};
    DaliError err = shell_sched_sync(&frame, wait_reply, 0u, send_twice,
                                    wait_reply ? &reply : NULL);
    if (wait_reply) {
        if (err == DALI_OK) {
            dali_cli_print_frame(&s_out, "RX: ", &reply);
        } else if (err == DALI_ERR_TIMEOUT) {
            shell_printf("RX: timeout\r\n");
        } else {
            shell_printf("TX/RX: ERR %d\r\n", (int)err);
        }
        return;
    }

    shell_print_tx_result("TX", err);
}

static void cmd_dtr(const DaliCliTokens *t)
{
    uint8_t reg;
    uint8_t value;

    if (!dali_cli_parse_u8(t->tok[1], (unsigned)DALI_DTR2, &reg) ||
        !dali_cli_parse_u8(t->tok[2], 255u, &value)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_DTR));
        return;
    }

    DaliFrame frame;
    DaliError err = dali_control_build_dtr((DaliDtrRegister)reg, value, &frame);
    if (err == DALI_OK) {
        err = shell_send_no_reply(&frame, false);
    }
    shell_print_tx_result("dtr", err);
}

static uint8_t shell_command_reply_retries_left(DaliCommandId id)
{
    return dali_command_response_retry_safe(id) ? 1u : 0u;
}

static DaliError shell_query_status(DaliTarget target, DaliFrame *reply)
{
    DaliFrame frame;
    DaliError err = dali_control_build_query_status(target, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return shell_sched_sync(
        &frame,
        true,
        shell_command_reply_retries_left(DALI_CMD_QUERY_STATUS),
        false,
        reply);
}

static DaliError shell_query_u8(DaliTarget target,
                               DaliCommandId id,
                               uint8_t param,
                               uint8_t *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliFrame reply = {0u, 0u};
    DaliError err = dali_control_build_query(target, id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }

    err = shell_sched_sync(&frame,
                          true,
                          shell_command_reply_retries_left(id),
                          false,
                          &reply);
    if (err != DALI_OK) {
        return err;
    }
    if (reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_MALFORMED;
    }

    *out = (uint8_t)(reply.data & 0xFFu);
    return DALI_OK;
}

static void cmd_status(const DaliCliTokens *t)
{
    DaliTarget target;
    DaliFrame reply = {0u, 0u};

    if (!dali_cli_parse_target(t->tok[1], &target)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_STATUS));
        return;
    }
    shell_warn_multi_target("status", target, true);

    DaliError err = shell_query_status(target, &reply);
    if (err == DALI_OK) {
        dali_cli_print_response(&s_out, "status", DALI_RESP_STATUS, &reply);
    } else {
        dali_cli_print_error(&s_out, "status", err);
    }
}

static void cmd_query(const DaliCliTokens *t)
{
    const DaliCliCommandSpec *usage = dali_cli_command_for_id(DALI_CLI_CMD_QUERY);
    DaliTarget target;

    if (!dali_cli_parse_target(t->tok[1], &target)) {
        dali_cli_print_usage(&s_out, usage);
        return;
    }

    /* `query <target>` on its own is the status shorthand. */
    if (t->count == 2u) {
        cmd_status(t);
        return;
    }

    const DaliCliGearCommand *spec = dali_cli_query_find(t->tok[2]);
    if (spec == NULL) {
        shell_printf("query: unknown query '%s'\r\n", t->tok[2]);
        shell_printf("use 'list query'\r\n");
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (t->count != 4u ||
            !dali_cli_parse_u8(t->tok[3], spec->max_param, &param)) {
            shell_printf("usage: query " DALI_CLI_TARGET_ARG " %s <0-%u>\r\n",
                   spec->name, (unsigned)spec->max_param);
            return;
        }
    } else if (t->count != 3u) {
        shell_printf("usage: query " DALI_CLI_TARGET_ARG " %s\r\n", spec->name);
        return;
    }

    shell_warn_multi_target("query", target, true);

    const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
    DaliFrame frame;
    DaliFrame reply = {0u, 0u};
    DaliError err = dali_control_build_query(target, spec->id, param, &frame);
    if (err == DALI_OK) {
        err = shell_sched_sync(&frame,
                              true,
                              shell_command_reply_retries_left(spec->id),
                              false,
                              &reply);
    }
    if (err == DALI_OK) {
        dali_cli_print_response(&s_out, spec->name,
                                cmd != NULL ? cmd->response_kind : DALI_RESP_UINT8,
                                &reply);
    } else {
        dali_cli_print_error(&s_out, spec->name, err);
    }
}

static void cmd_special(const DaliCliTokens *t)
{
    const DaliCliGearCommand *spec = dali_cli_special_find(t->tok[1]);
    if (spec == NULL) {
        shell_printf("special: unknown command '%s'\r\n", t->tok[1]);
        shell_printf("use 'list special'\r\n");
        return;
    }

    /*
     * The verb is permitted while nine of its names are not: sent alone the
     * addressing primitives are at best inert and at worst destroy the
     * addressing of every device on the bus, and RANDOMISE cannot be undone.
     * A session without DALI_SHELL_ALLOW_COMMISSION reaches them only through
     * `commission`, which sequences and checks them.
     */
    if (dali_cli_special_is_commissioning(spec->id) &&
        !shell_policy_allows(DALI_SHELL_ALLOW_COMMISSION, spec->name)) {
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (t->count != 3u ||
            !dali_cli_parse_u8(t->tok[2], spec->max_param, &param)) {
            shell_printf("usage: special %s <0-%u>\r\n",
                   spec->name, (unsigned)spec->max_param);
            return;
        }
    } else if (t->count != 2u) {
        shell_printf("usage: special %s\r\n", spec->name);
        return;
    }

    const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
    if (cmd == NULL || cmd->frame_kind != DALI_CMD_FRAME_SPECIAL) {
        dali_cli_print_error(&s_out, spec->name, DALI_ERR_INVALID);
        return;
    }

    DaliFrame frame;
    DaliFrame reply = {0u, 0u};
    bool needs_reply = cmd->response_kind != DALI_RESP_NONE;
    DaliError err = dali_build_special(spec->id, param, &frame);
    if (err == DALI_OK) {
        err = shell_sched_sync(&frame,
                              needs_reply,
                              needs_reply
                                  ? shell_command_reply_retries_left(spec->id)
                                  : 0u,
                              cmd->send_twice,
                              needs_reply ? &reply : NULL);
    }

    if (err == DALI_OK && needs_reply) {
        dali_cli_print_response(&s_out, spec->name, cmd->response_kind, &reply);
    } else if (err == DALI_OK) {
        shell_printf("%s: OK\r\n", spec->name);
    } else {
        dali_cli_print_error(&s_out, spec->name, err);
    }
}

static void cmd_config(const DaliCliTokens *t)
{
    const DaliCliCommandSpec *usage = dali_cli_command_for_id(DALI_CLI_CMD_CONFIG);
    DaliTarget target;

    if (!dali_cli_parse_target(t->tok[1], &target)) {
        dali_cli_print_usage(&s_out, usage);
        return;
    }

    const DaliCliGearCommand *spec = dali_cli_config_find(t->tok[2]);
    if (spec == NULL) {
        shell_printf("config: unknown config '%s'\r\n", t->tok[2]);
        shell_printf("use 'list config'\r\n");
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (t->count != 4u ||
            !dali_cli_parse_u8(t->tok[3], spec->max_param, &param)) {
            shell_printf("usage: config " DALI_CLI_TARGET_ARG " %s <0-%u>\r\n",
                   spec->name, (unsigned)spec->max_param);
            return;
        }
    } else if (t->count != 3u) {
        shell_printf("usage: config " DALI_CLI_TARGET_ARG " %s\r\n", spec->name);
        return;
    }

    shell_warn_multi_target("config", target, false);
    if (spec->uses_dtr0) {
        shell_printf("config: using current DTR0 value; use config-dtr0 to set it atomically\r\n");
    }

    DaliFrame frame;
    DaliError err = dali_control_build_config(target, spec->id, param, &frame);
    const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
    if (err == DALI_OK && cmd != NULL) {
        err = shell_send_no_reply(&frame, cmd->send_twice);
    }
    shell_print_tx_result(spec->name, err);
}

/*
 * Report a sequence outcome, naming the step that ended it. Which step failed
 * is the difference between "nothing happened" and "the DTR was written but the
 * command that consumes it was not", so it is never folded into a bare error.
 */
static void shell_print_sequence_result(const char *name,
                                       DaliError err,
                                       const DaliSequenceResult *result)
{
    if (err == DALI_OK) {
        shell_printf("%s: OK\r\n", name);
        return;
    }
    if (result != NULL && result->failed_step != DALI_SEQUENCE_NO_FAILED_STEP) {
        shell_printf("%s: ERR %d at sequence step %u\r\n",
               name, (int)err, (unsigned)result->failed_step);
        return;
    }
    dali_cli_print_error(&s_out, name, err);
}

static void cmd_config_dtr0(const DaliCliTokens *t)
{
    const DaliCliCommandSpec *usage = dali_cli_command_for_id(DALI_CLI_CMD_CONFIG_DTR0);
    DaliTarget target;

    if (!dali_cli_parse_target(t->tok[1], &target)) {
        dali_cli_print_usage(&s_out, usage);
        return;
    }

    const DaliCliGearCommand *spec = dali_cli_config_find(t->tok[2]);
    if (spec == NULL) {
        shell_printf("config-dtr0: unknown config '%s'\r\n", t->tok[2]);
        shell_printf("use 'list config'\r\n");
        return;
    }
    if (!spec->uses_dtr0 || !dali_control_config_uses_dtr0(spec->id)) {
        shell_printf("config-dtr0: '%s' does not consume DTR0\r\n", spec->name);
        return;
    }

    uint8_t dtr0_value = 0u;
    if (!dali_cli_parse_u8(t->tok[3], 255u, &dtr0_value)) {
        dali_cli_print_usage(&s_out, usage);
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (t->count != 5u ||
            !dali_cli_parse_u8(t->tok[4], spec->max_param, &param)) {
            shell_printf("usage: config-dtr0 " DALI_CLI_TARGET_ARG " %s <0-255> <0-%u>\r\n",
                   spec->name, (unsigned)spec->max_param);
            return;
        }
    } else if (t->count != 4u) {
        shell_printf("usage: config-dtr0 " DALI_CLI_TARGET_ARG " %s <0-255>\r\n",
               spec->name);
        return;
    }

    shell_warn_multi_target("config-dtr0", target, false);

    const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
    DaliFrame dtr_frame;
    DaliFrame config_frame;
    DaliError err = dali_control_build_dtr(DALI_DTR0, dtr0_value, &dtr_frame);
    if (err == DALI_OK) {
        err = dali_control_build_config(target, spec->id, param, &config_frame);
    }
    if (err == DALI_OK && cmd == NULL) {
        err = DALI_ERR_INVALID;
    }
    if (err != DALI_OK) {
        shell_print_tx_result(spec->name, err);
        return;
    }

    DaliSequence seq = {
        .steps = {
            { .frame = dtr_frame },
            { .frame = config_frame, .send_twice = cmd->send_twice },
        },
        .step_count = 2u,
    };

    DaliSequenceResult seq_result;
    err = shell_sched_sequence_sync(&seq, &seq_result);
    shell_print_sequence_result(spec->name, err, &seq_result);
}

static void shell_print_input_instance(const DaliInputInstanceInfo *info)
{
    if (info == NULL) {
        return;
    }

    shell_printf("  %2u: type=%u %s, usable=%s, source=%s",
           (unsigned)info->instance,
           (unsigned)info->type,
           dali_input_role_name(info->role),
           dali_input_usable_name(info->usable),
           dali_input_role_source_name(info->role_source));

    if (info->has_enabled) {
        shell_printf(", enabled=%s", info->enabled ? "yes" : "no");
    }
    if (info->has_resolution) {
        shell_printf(", resolution=%u", (unsigned)info->resolution);
    }
    if (info->has_status) {
        shell_printf(", status=0x%02X", (unsigned)info->status);
    }
    if (info->has_error) {
        shell_printf(", error=%s", dali_is_yes(info->error) ? "yes" : "no");
    }
    shell_printf("\r\n");
}

static void cmd_instances(const DaliCliTokens *t)
{
    uint8_t addr;

    if (!dali_cli_parse_short_addr(t->tok[1], &addr)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_INSTANCES));
        return;
    }

    DaliDiscoveryTransport transport = shell_discovery_transport();
    DaliDiscoveryInputDevice input;
    DaliError err = dali_discovery_query_input_device(&transport, addr, &input);
    if (err == DALI_ERR_TIMEOUT) {
        shell_printf("instances: timeout querying device %u\r\n", (unsigned)addr);
        return;
    }
    if (err != DALI_OK) {
        shell_printf("instances: ERR %d\r\n", (int)err);
        return;
    }
    shell_input_cache_store(&input);

    DaliDiscoveryInventory *inventory = &s_inventory_scratch;
    if (shell_inventory_snapshot(inventory)) {
        if (dali_discovery_inventory_update_input_device(inventory, &input) == DALI_OK) {
            shell_inventory_replace(inventory);
        }
    }

    uint8_t count = dali_discovery_input_visible_instance_count(&input);
    shell_printf("Input device %u:\r\n", (unsigned)addr);
    if (input.device.instance_count > DALI_INPUT_MAX_INSTANCES) {
        shell_printf("  instances: %u (showing first %u)\r\n",
               (unsigned)input.device.instance_count,
               (unsigned)DALI_INPUT_MAX_INSTANCES);
    } else {
        shell_printf("  instances: %u\r\n", (unsigned)input.device.instance_count);
    }

    for (uint8_t instance = 0u; instance < count; instance++) {
        DaliError type_err = input.instance_type_errors[instance];
        const DaliInputInstanceInfo *info = &input.device.instances[instance];
        if (type_err == DALI_ERR_TIMEOUT) {
            shell_printf("  %2u: type timeout\r\n", (unsigned)instance);
            continue;
        }
        if (type_err != DALI_OK) {
            shell_printf("  %2u: type ERR %d\r\n", (unsigned)instance, (int)type_err);
            continue;
        }

        shell_print_input_instance(info);
    }
}

static void shell_print_poll_value(const DaliInputPollResult *poll)
{
    if (poll == NULL) {
        return;
    }

    int width = (int)(poll->value.byte_count * 2u);
    if (width <= 0) {
        width = 2;
    }

    shell_printf(" raw=0x%0*" PRIX32 " bytes=%u",
           width,
           poll->value.value,
           (unsigned)poll->value.byte_count);
}

static void shell_sensor_poll_instance(uint8_t addr,
                                      const DaliInputInstanceInfo *info)
{
    if (info == NULL) {
        return;
    }

    uint8_t expected_bytes = info->has_resolution
                           ? dali_input_poll_bytes_for_resolution(info->resolution)
                           : 1u;
    DaliDiscoveryTransport transport = shell_discovery_transport();
    DaliInputPollResult poll;
    DaliError err = dali_input_poll_value(&transport,
                                          addr,
                                          info->instance,
                                          expected_bytes,
                                          &poll);
    shell_sensor_value_cache_store(addr, info, expected_bytes, err, &poll);

    shell_printf("  %2u: type=%u %s",
           (unsigned)info->instance,
           (unsigned)info->type,
           dali_input_role_name(info->role));

    if (info->has_enabled) {
        shell_printf(", enabled=%s", info->enabled ? "yes" : "no");
    }
    if (info->has_resolution) {
        shell_printf(", resolution=%u", (unsigned)info->resolution);
    } else {
        shell_printf(", resolution=unknown");
    }

    if (err == DALI_OK) {
        shell_print_poll_value(&poll);
        shell_printf("\r\n");
    } else if (err == DALI_ERR_TIMEOUT) {
        shell_printf(" poll=timeout\r\n");
    } else {
        shell_printf(" poll=ERR %d\r\n", (int)err);
    }
}

static void cmd_sensor(const DaliCliTokens *t)
{
    uint8_t addr;
    uint8_t selected_instance = 0u;
    bool has_selected_instance = t->count == 4u;

    if (!dali_cli_has_subcommand(dali_cli_command_for_id(DALI_CLI_CMD_SENSOR),
                                 t->tok[1]) ||
        !dali_cli_parse_short_addr(t->tok[2], &addr) ||
        (has_selected_instance &&
         !dali_cli_parse_instance(t->tok[3], &selected_instance))) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_SENSOR));
        return;
    }

    DaliDiscoveryTransport transport = shell_discovery_transport();
    DaliDiscoveryInputDevice input;
    DaliError err = dali_discovery_query_input_device(&transport, addr, &input);
    if (err == DALI_ERR_TIMEOUT) {
        shell_printf("sensor poll: timeout querying device %u\r\n", (unsigned)addr);
        return;
    }
    if (err != DALI_OK) {
        shell_printf("sensor poll: ERR %d\r\n", (int)err);
        return;
    }
    shell_input_cache_store(&input);

    DaliDiscoveryInventory *inventory = &s_inventory_scratch;
    if (shell_inventory_snapshot(inventory)) {
        if (dali_discovery_inventory_update_input_device(inventory, &input) == DALI_OK) {
            shell_inventory_replace(inventory);
        }
    }

    uint8_t count = dali_discovery_input_visible_instance_count(&input);
    shell_printf("Sensor poll %u:\r\n", (unsigned)addr);

    if (has_selected_instance) {
        if (selected_instance >= count) {
            shell_printf("  instance %u not visible; discovered count=%u\r\n",
                   (unsigned)selected_instance,
                   (unsigned)input.device.instance_count);
            return;
        }
        shell_sensor_poll_instance(addr, &input.device.instances[selected_instance]);
        return;
    }

    for (uint8_t instance = 0u; instance < count; instance++) {
        DaliError type_err = input.instance_type_errors[instance];
        if (type_err == DALI_ERR_TIMEOUT) {
            shell_printf("  %2u: type timeout\r\n", (unsigned)instance);
            continue;
        }
        if (type_err != DALI_OK) {
            shell_printf("  %2u: type ERR %d\r\n", (unsigned)instance, (int)type_err);
            continue;
        }
        shell_sensor_poll_instance(addr, &input.device.instances[instance]);
    }
}

static void cmd_events(void)
{
#ifndef DALI_HOST_BUILD
    DaliInputEventRecord record;
    uint8_t count = 0u;

    while (shell_event_pop(&record)) {
        shell_print_event_record("event: ", &record);
        count++;
    }

    if (count == 0u) {
        shell_printf("events: none queued\r\n");
    }
    shell_printf("events: drained=%u dropped=%" PRIu32 "\r\n",
           (unsigned)count,
           shell_event_queue_dropped_snapshot());
#endif
}

static void cmd_capture(const DaliCliTokens *t)
{
    const char *action = t->tok[1];

    if (!dali_cli_has_subcommand(dali_cli_command_for_id(DALI_CLI_CMD_CAPTURE),
                                 action)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_CAPTURE));
        return;
    }

    if (strcmp(action, "start") == 0) {
        shell_capture_set_enabled(true);
        shell_printf("capture: started\r\n");
    } else if (strcmp(action, "stop") == 0) {
        shell_capture_set_enabled(false);
        shell_printf("capture: stopped\r\n");
    } else if (strcmp(action, "clear") == 0) {
        shell_capture_reset();
        shell_printf("capture: cleared\r\n");
    } else if (strcmp(action, "status") == 0) {
        uint32_t dropped = 0u;
        bool enabled = false;
        uint8_t count = shell_capture_snapshot(NULL, 0u, &dropped, &enabled);
        shell_printf("capture: %s, count=%u, dropped=%" PRIu32 "\r\n",
               enabled ? "on" : "off",
               (unsigned)count,
               dropped);
    } else if (strcmp(action, "export") == 0) {
        shell_printf("{\r\n");
        shell_print_capture_json();
        shell_printf("\r\n}\r\n");
    } else {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_CAPTURE));
    }
}

static void cmd_find(const DaliCliTokens *t)
{
    uint32_t seconds = SHELL_FIND_SWITCH_DEFAULT_SECONDS;

    if (!dali_cli_has_subcommand(dali_cli_command_for_id(DALI_CLI_CMD_FIND),
                                 t->tok[1]) ||
        (t->count == 3u &&
         !dali_cli_parse_u32(t->tok[2], SHELL_FIND_SWITCH_MAX_SECONDS, &seconds)) ||
        seconds == 0u) {
        shell_printf("usage: find switches [1-%u seconds]\r\n",
               (unsigned)SHELL_FIND_SWITCH_MAX_SECONDS);
        return;
    }

    shell_events_reset();
    shell_switch_mappings_reset();

    shell_printf("find switches: listening for %u seconds; double-press DALI-2 switches or trigger legacy coupler actions.\r\n",
           (unsigned)seconds);
    shell_printf("Run 'discover' first so Device/Instance switch types can be resolved safely.\r\n");

    TickType_t start = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(seconds * 1000u);
    DaliInputEventRecord record;

    while ((xTaskGetTickCount() - start) < duration) {
        while (shell_event_pop(&record)) {
            shell_print_event_record("event: ", &record);
            if (shell_event_is_switch_candidate(&record.event)) {
                bool recorded = shell_record_switch_mapping(&record);
                if (recorded) {
                    uint8_t order = shell_switch_mappings_snapshot(NULL, 0u);
                    shell_printf("  mapped switch %u\r\n", (unsigned)order);
                }
            }
        }
        /* This verb sends nothing, so the transport's abort check never runs;
         * without this a disconnected session would hold the task for the full
         * listening window. */
        if (shell_aborted()) {
            shell_printf("find switches: aborted\r\n");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50u));
    }

    while (shell_event_pop(&record)) {
        shell_print_event_record("event: ", &record);
        if (shell_event_is_switch_candidate(&record.event)) {
            bool recorded = shell_record_switch_mapping(&record);
            if (recorded) {
                uint8_t order = shell_switch_mappings_snapshot(NULL, 0u);
                shell_printf("  mapped switch %u\r\n", (unsigned)order);
            }
        }
    }

    uint8_t mapped = shell_switch_mappings_snapshot(NULL, 0u);
    shell_printf("find switches: mapped=%u dropped=%" PRIu32 "\r\n",
           (unsigned)mapped,
           shell_event_queue_dropped_snapshot());
}

typedef struct {
    bool detailed;
} DiagDiscoveryPrintCtx;

static void shell_discovery_found_cb(uint8_t addr,
                                    const DaliDiscoveryDeviceInfo *device,
                                    void *ctx)
{
    DiagDiscoveryPrintCtx *print_ctx = (DiagDiscoveryPrintCtx *)ctx;
    if (device == NULL || (!device->has_status && !device->has_input_device)) {
        return;
    }

    if (print_ctx != NULL && print_ctx->detailed) {
        if (device->has_status) {
            const char *kind = device->has_device_type
                ? dali_discovery_device_type_name(device->device_type)
                : "unknown-type";
            shell_printf("%02u: present, %s, status=0x%02X",
                   (unsigned)addr, kind, (unsigned)device->status);
            if (device->has_version) {
                shell_printf(", v%u", (unsigned)(device->version / 2u));
            }
            if (device->has_actual_level) {
                shell_printf(", level=%u", (unsigned)device->actual_level);
            }
            if (device->has_groups && device->groups != 0u) {
                shell_printf(", groups=[");
                bool first = true;
                for (uint8_t g = 0u; g < 16u; g++) {
                    if (device->groups & (1u << g)) {
                        if (!first) {
                            shell_printf(",");
                        }
                        shell_printf("%u", (unsigned)g);
                        first = false;
                    }
                }
                shell_printf("]");
            }
            if (device->has_input_device) {
                shell_printf(", input-device(%u instances)", (unsigned)device->instance_count);
            }
            shell_printf("\r\n");
        } else {
            shell_printf("%02u: input-device, %u instance(s)\r\n",
                   (unsigned)addr,
                   (unsigned)(device->has_instance_count ? device->instance_count : 0u));
        }
    } else {
        if (device->has_status) {
            shell_printf("Device %2u: present (status=0x%02X)\r\n",
                   (unsigned)addr,
                   (unsigned)device->status);
        } else {
            shell_printf("Device %2u: input-device (%u instance(s))\r\n",
                   (unsigned)addr,
                   (unsigned)(device->has_instance_count ? device->instance_count : 0u));
        }
    }
}

static uint8_t shell_discover_bus(bool detailed)
{
    uint8_t found = 0u;
    DaliDiscoveryInventory *inventory = &s_inventory_scratch;
    DaliDiscoveryTransport transport = shell_discovery_transport();
    DiagDiscoveryPrintCtx print_ctx = {
        .detailed = detailed,
    };

    /* Held for the whole walk, so the integration's own polling does not
     * interleave queries into it. */
    if (!shell_bus_claim("scan")) {
        shell_printf("scan: bus busy\r\n");
        return 0u;
    }

    /*
     * A backward frame that arrives after its reply window has closed is the one
     * bus fault a scan cannot see: the transaction reports a plain timeout, and
     * an address that answered a shade too late is indistinguishable from an
     * empty one. Report the count so under-reporting a populated bus reads as a
     * timing problem rather than as missing hardware.
     */
    uint32_t late_replies_before = g_dali_stats.rx_ignored_outside_reply;

    shell_printf("Scanning short addresses 0-%u...\r\n", (unsigned)DALI_MAX_SHORT_ADDRESS);
    DaliError err = dali_discovery_scan(inventory,
                                        &transport,
                                        shell_discovery_found_cb,
                                        &print_ctx,
                                        &found);
    if (err != DALI_OK) {
        shell_inventory_reset();
        shell_bus_release();
        /* A cancelled walk is the front end going away, not a bus fault. */
        if (err == DALI_ERR_CANCELLED) {
            shell_printf("Scan aborted\r\n");
        } else {
            shell_printf("Scan ERR %d\r\n", (int)err);
        }
        return 0u;
    }

    /* Enumerate instances for any detected input devices. */
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *entry =
            dali_discovery_inventory_get(inventory, addr);
        if (entry == NULL || !entry->present || !entry->has_input_device) {
            continue;
        }
        DaliDiscoveryInputDevice input;
        if (dali_discovery_query_input_device(&transport, addr, &input) == DALI_OK) {
            (void)dali_discovery_inventory_update_input_device(inventory, &input);
            shell_input_cache_store(&input);
            if (detailed) {
                uint8_t count = dali_discovery_input_visible_instance_count(&input);
                shell_printf("  %02u: %u input instance(s) enumerated\r\n",
                       (unsigned)addr, (unsigned)count);
            }
        }
    }

    shell_inventory_replace(inventory);
    shell_bus_release();

    /* Tell the integration its cached view of the bus is stale before saying
     * the scan is done, so an operator reading the next line is looking at a
     * surface that has already been told to catch up. */
    if (s_session.hooks.inventory_changed != NULL) {
        s_session.hooks.inventory_changed(s_session.hooks.ctx, inventory);
    }

    uint32_t late_replies =
        g_dali_stats.rx_ignored_outside_reply - late_replies_before;
    shell_printf("Scan complete: %u device(s) found.\r\n", (unsigned)found);
    if (late_replies > 0u) {
        /* Not a failure on its own: the retry hold-off exists to cover exactly
         * this, and a bus whose gear answers a shade late reports a steady count
         * on every scan. It earns a line because it is the one condition that
         * can drop a present device from the list above without any other trace.
         *
         * Two calls rather than one sentence: as a single line this ran past
         * DALI_CLI_FORMAT_MAX and was clipped of its own terminator, which left
         * the prompt written onto the end of it and hung every client that
         * frames replies on the prompt. shell_printf() now restores a clipped
         * terminator, but keeping each line inside the buffer is what stops the
         * text being cut in the first place. */
        shell_printf("  note: %" PRIu32 " backward frame(s) answered after the "
                     "%u ms reply window.\r\n",
                     late_replies, (unsigned)DALI_REPLY_TIMEOUT_MS);
        shell_printf("  Marginal gear timing; retries cover it. If a known "
                     "device is missing above, re-run.\r\n");
    }
    return found;
}

static void cmd_scan(void)
{
#ifndef DALI_HOST_BUILD
    (void)shell_discover_bus(false);
#endif
}

static void cmd_discover(void)
{
#ifndef DALI_HOST_BUILD
    (void)shell_discover_bus(true);
#endif
}

static void cmd_inventory(void)
{
#ifndef DALI_HOST_BUILD
    uint8_t found = 0u;
    DaliDiscoveryInventory *inventory = &s_inventory_scratch;

    if (!shell_inventory_snapshot(inventory)) {
        shell_printf("inventory: empty; run discover first\r\n");
        return;
    }
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *entry =
            dali_discovery_inventory_get(inventory, addr);
        if (entry != NULL && entry->present &&
            (entry->has_status || entry->has_input_device)) {
            if (entry->has_status) {
                const char *kind = entry->has_device_type
                    ? dali_discovery_device_type_name(entry->device_type)
                    : "unknown-type";
                shell_printf("%02u: %s, status=0x%02X", (unsigned)addr, kind, (unsigned)entry->status);
            } else {
                shell_printf("%02u: input-device", (unsigned)addr);
            }
            if (entry->has_version) {
                shell_printf(", v%u", (unsigned)(entry->version / 2u));
            }
            if (entry->has_actual_level) {
                shell_printf(", level=%u", (unsigned)entry->actual_level);
            }
            if (entry->has_groups) {
                shell_printf(", groups=[");
                bool first_group = true;
                for (uint8_t g = 0u; g < 16u; g++) {
                    if (entry->groups & (1u << g)) {
                        if (!first_group) {
                            shell_printf(",");
                        }
                        shell_printf("%u", (unsigned)g);
                        first_group = false;
                    }
                }
                shell_printf("]");
            }
            if (entry->has_input_device) {
                shell_printf(", input-device(%u instances)", (unsigned)entry->instance_count);
            }
            shell_printf("\r\n");
            found++;
        }
    }

    shell_printf("Inventory: %u device(s)\r\n", (unsigned)found);
#endif
}

static void shell_commission_progress_cb(const DaliCommissioningEvent *event,
                                        void *ctx)
{
#ifndef DALI_HOST_BUILD
    (void)ctx;
    if (event == NULL) {
        return;
    }

    switch (event->kind) {
        case DALI_COMMISSIONING_EVENT_INITIALISED:
            shell_printf("commission: initialise unaddressed\r\n");
            break;

        case DALI_COMMISSIONING_EVENT_RANDOMISED:
            shell_printf("commission: randomize\r\n");
            break;

        case DALI_COMMISSIONING_EVENT_SEARCH_FOUND:
            shell_printf("commission: found random=0x%06" PRIX32
                   " -> short %u\r\n",
                   event->random_address,
                   (unsigned)event->short_address);
            break;

        case DALI_COMMISSIONING_EVENT_ASSIGNED:
            shell_printf("commission: assigned short %u"
                   " (count=%u)\r\n",
                   (unsigned)event->short_address,
                   (unsigned)event->assigned_count);
            break;

        case DALI_COMMISSIONING_EVENT_NO_MORE_DEVICES:
            shell_printf("commission: no more unaddressed devices\r\n");
            break;

        case DALI_COMMISSIONING_EVENT_ADDRESS_SPACE_FULL:
            shell_printf("commission: no free short addresses\r\n");
            break;

        case DALI_COMMISSIONING_EVENT_TERMINATED:
            shell_printf("commission: terminate\r\n");
            break;

        default:
            break;
    }
#else
    (void)event;
    (void)ctx;
#endif
}

static void cmd_commission(const DaliCliTokens *t)
{
    uint8_t first_address = 0u;
    uint8_t max_devices = 0u;

    if (!dali_cli_has_subcommand(dali_cli_command_for_id(DALI_CLI_CMD_COMMISSION),
                                 t->tok[1]) ||
        (t->count >= 3u &&
         !dali_cli_parse_u8(t->tok[2], DALI_MAX_SHORT_ADDRESS, &first_address)) ||
        (t->count == 4u &&
         !dali_cli_parse_u8(t->tok[3], DALI_SHORT_ADDRESS_COUNT, &max_devices))) {
        shell_printf("usage: commission unaddressed [0-%u] [0-%u]\r\n",
               (unsigned)DALI_MAX_SHORT_ADDRESS,
               (unsigned)DALI_SHORT_ADDRESS_COUNT);
        return;
    }

    DaliDiscoveryTransport transport = shell_discovery_transport();
    DaliDiscoveryInventory *inventory = &s_inventory_scratch;
    uint8_t found = 0u;

    /*
     * Claimed across the pre-scan and the addressing walk together, not once
     * per phase: the used-address mask the walk assigns from is only valid as
     * long as nothing else has touched the bus since the scan that produced it.
     */
    if (!shell_bus_claim("commission")) {
        shell_printf("commission: bus busy\r\n");
        return;
    }

    shell_printf("commission: pre-scan occupied short addresses\r\n");
    DaliError err = dali_discovery_scan(inventory,
                                        &transport,
                                        NULL,
                                        NULL,
                                        &found);
    if (err != DALI_OK) {
        shell_inventory_reset();
        shell_bus_release();
        shell_printf("commission: pre-scan ERR %d\r\n", (int)err);
        return;
    }
    shell_inventory_replace(inventory);
    shell_printf("commission: occupied=%u\r\n", (unsigned)found);

    DaliCommissioningOptions options = {
        .first_short_address = first_address,
        .max_devices = max_devices,
        .used_address_mask =
            dali_commissioning_used_mask_from_inventory(inventory),
        .query_short_address = true,
    };
    DaliCommissioningResult result;
    err = dali_commissioning_commission_unaddressed(
        &transport,
        &options,
        &result,
        shell_commission_progress_cb,
        NULL);

    shell_bus_release();

    /* Commissioning changes which short addresses exist, so the integration's
     * cached view is stale on both the success and the partial-failure path. */
    if (s_session.hooks.inventory_changed != NULL) {
        s_session.hooks.inventory_changed(s_session.hooks.ctx, inventory);
    }

    if (err != DALI_OK) {
        shell_printf("commission: ERR %d after %u assignment(s)\r\n",
               (int)err,
               (unsigned)result.assigned_count);
        return;
    }

    shell_printf("commission: complete assigned=%u",
           (unsigned)result.assigned_count);
    if (result.no_more_devices) {
        shell_printf(" no-more-devices=yes");
    }
    if (result.address_space_full) {
        shell_printf(" address-space-full=yes");
    }
    shell_printf("\r\n");

    for (uint8_t i = 0u; i < result.assigned_count; i++) {
        const DaliCommissioningAssignment *assignment =
            &result.assignments[i];
        shell_printf("  short %u <= random 0x%06" PRIX32,
               (unsigned)assignment->short_address,
               assignment->random_address);
        if (assignment->has_query_short) {
            shell_printf(" query=0x%02X",
                   (unsigned)assignment->query_short_raw);
        }
        shell_printf("\r\n");
    }

    if (result.assigned_count > 0u) {
        shell_printf("commission: verifying with post-scan\r\n");
        found = 0u;
        err = dali_discovery_scan(inventory,
                                  &transport,
                                  NULL,
                                  NULL,
                                  &found);
        if (err != DALI_OK) {
            shell_inventory_reset();
            shell_printf("commission: post-scan ERR %d\r\n", (int)err);
            return;
        }
        shell_inventory_replace(inventory);
        shell_printf("commission: post-scan found=%u\r\n", (unsigned)found);
    }
}

/*
 * `export config` — the integration's YAML, not the bus's state.
 *
 * Delegated to the front end (see DaliShellHooks.export_config): the answer is
 * "what is this device configured as", which the shell does not know and the
 * serial console's firmware does not have.
 */
static void cmd_export_config(void)
{
    DaliDiscoveryInventory *inventory = &s_inventory_scratch;

    if (s_session.hooks.export_config == NULL) {
        shell_printf("export config: this firmware is not configured by YAML; "
                     "nothing to export\r\n");
        return;
    }

    /*
     * The input cache goes across as a lookup rather than a copy: it is up to
     * SHELL_INPUT_CACHE_MAX full instance records, the hook reads a handful of
     * addresses out of it, and shell_input_cache_lookup() already takes the
     * critical section that makes a read safe against the DALI task.
     */
    s_session.hooks.export_config(s_session.hooks.ctx,
                                  &s_out,
                                  shell_inventory_snapshot(inventory) ? inventory
                                                                      : NULL,
                                  shell_input_cache_lookup);
}

static void cmd_export_inventory(void)
{
    DaliDiscoveryInventory *inventory = &s_inventory_scratch;
    bool has_inventory;
    static DiagSwitchMapping mappings[SHELL_SWITCH_MAPPING_MAX];
    uint8_t mapping_count;

    has_inventory = shell_inventory_snapshot(inventory);
    mapping_count = shell_switch_mappings_snapshot(mappings, SHELL_SWITCH_MAPPING_MAX);

    shell_printf("{\r\n");
    shell_printf("  \"schema_version\": 1,\r\n");
    shell_printf("  \"devices\": [\r\n");
    bool first_device = true;
    if (has_inventory) {
        for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
            const DaliDiscoveryDeviceInfo *entry =
                dali_discovery_inventory_get(inventory, addr);
            if (entry == NULL || !entry->present) {
                continue;
            }

            if (!first_device) {
                shell_printf(",\r\n");
            }
            first_device = false;

            DaliDiscoveryInputDevice cached_input;
            bool has_input_cache = shell_input_cache_lookup(addr, &cached_input);

            shell_printf("    { \"address\": %u, \"present\": true",
                   (unsigned)addr);
            if (entry->has_status) {
                shell_printf(", \"status\": %u, \"status_hex\": \"0x%02X\"",
                       (unsigned)entry->status,
                       (unsigned)entry->status);
            }
            if (entry->has_device_type) {
                shell_printf(", \"device_type\": %u, \"device_type_name\": \"%s\"",
                       (unsigned)entry->device_type,
                       dali_discovery_device_type_name(entry->device_type));
            }
            if (entry->has_version) {
                shell_printf(", \"version\": %u", (unsigned)entry->version);
            }
            if (entry->has_actual_level) {
                shell_printf(", \"actual_level\": %u", (unsigned)entry->actual_level);
            }
            if (entry->has_groups) {
                shell_printf(", \"groups\": [");
                bool first_group = true;
                for (uint8_t g = 0u; g < 16u; g++) {
                    if (entry->groups & (1u << g)) {
                        if (!first_group) {
                            shell_printf(", ");
                        }
                        shell_printf("%u", (unsigned)g);
                        first_group = false;
                    }
                }
                shell_printf("]");
            }
            if (entry->has_control_gear && entry->has_input_device) {
                shell_printf(", \"kind\": \"hybrid\"");
                if (entry->has_instance_count) {
                    shell_printf(", \"instance_count\": %u",
                           (unsigned)entry->instance_count);
                }
            } else if (entry->has_input_device) {
                shell_printf(", \"kind\": \"input_device\"");
                if (entry->has_instance_count) {
                    shell_printf(", \"instance_count\": %u",
                           (unsigned)entry->instance_count);
                }
            } else if (entry->has_control_gear) {
                shell_printf(", \"kind\": \"control_gear\"");
            } else {
                shell_printf(", \"kind\": \"unknown\"");
            }

            if (has_input_cache) {
                uint8_t count =
                    dali_discovery_input_visible_instance_count(&cached_input);
                shell_printf(", \"instances\": [");
                for (uint8_t instance = 0u; instance < count; instance++) {
                    const DaliInputInstanceInfo *info =
                        &cached_input.device.instances[instance];
                    DaliError type_err = cached_input.instance_type_errors[instance];

                    if (instance > 0u) {
                        shell_printf(", ");
                    }

                    shell_printf("{ \"instance\": %u", (unsigned)instance);
                    if (type_err == DALI_OK && info->has_type) {
                        shell_printf(", \"type\": %u", (unsigned)info->type);
                        shell_printf(", \"type_name\": \"%s\"",
                               dali_input_type_name(info->type));
                        shell_printf(", \"role\": \"%s\"",
                               dali_input_role_name(info->role));
                        shell_printf(", \"usable\": \"%s\"",
                               dali_input_usable_name(info->usable));
                        shell_printf(", \"source\": \"%s\"",
                               dali_input_role_source_name(info->role_source));
                        if (info->has_enabled) {
                            shell_printf(", \"enabled\": %s",
                                   info->enabled ? "true" : "false");
                        }
                        if (info->has_resolution) {
                            shell_printf(", \"resolution\": %u",
                                   (unsigned)info->resolution);
                        }
                        if (info->has_status) {
                            shell_printf(", \"status\": %u, \"status_hex\": \"0x%02X\"",
                                   (unsigned)info->status,
                                   (unsigned)info->status);
                        }
                        if (info->has_error) {
                            shell_printf(", \"error\": %s",
                                   dali_is_yes(info->error) ? "true" : "false");
                        }
                    } else {
                        shell_printf(", \"query_error\": %d", (int)type_err);
                    }

                    DiagSensorValueCacheEntry cached_value;
                    if (shell_sensor_value_cache_lookup(addr,
                                                       instance,
                                                       &cached_value)) {
                        int value_width = (int)(cached_value.byte_count * 2u);
                        if (value_width <= 0) {
                            value_width = 2;
                        }
                        shell_printf(", \"latest_value\": {");
                        shell_printf(" \"result\": %d", (int)cached_value.result);
                        shell_printf(", \"timestamp_us\": %" PRIu32,
                               cached_value.timestamp_us);
                        shell_printf(", \"expected_bytes\": %u",
                               (unsigned)cached_value.expected_bytes);
                        shell_printf(", \"byte_count\": %u",
                               (unsigned)cached_value.byte_count);
                        shell_printf(", \"complete\": %s",
                               cached_value.complete ? "true" : "false");
                        shell_printf(", \"raw\": \"0x%0*" PRIX32 "\"",
                               value_width,
                               cached_value.value);
                        shell_printf(", \"raw_value\": %" PRIu32,
                               cached_value.value);
                        if (cached_value.has_resolution) {
                            shell_printf(", \"resolution\": %u",
                                   (unsigned)cached_value.resolution);
                        }
                        shell_printf(", \"byte_errors\": [%d, %d, %d, %d]",
                               (int)cached_value.byte_errors[0],
                               (int)cached_value.byte_errors[1],
                               (int)cached_value.byte_errors[2],
                               (int)cached_value.byte_errors[3]);
                        shell_printf(" }");
                    }
                    shell_printf(" }");
                }
                shell_printf("]");
            }
            shell_printf(" }");
        }
    }
    shell_printf("\r\n  ],\r\n");

    shell_printf("  \"switches\": [\r\n");
    for (uint8_t i = 0u; i < mapping_count; i++) {
        const DiagSwitchMapping *mapping = &mappings[i];
        const DaliInputEvent *event = &mapping->event;
        if (!mapping->valid) {
            continue;
        }

        if (i > 0u) {
            shell_printf(",\r\n");
        }

        shell_printf("    { \"order\": %u", (unsigned)mapping->order);
        shell_print_event_json_fields(event);
        shell_printf(", \"raw\": \"0x%0*" PRIX32 "\"",
               shell_frame_hex_width(&event->raw),
               event->raw.data);
        shell_printf(", \"raw_bits\": %u", (unsigned)event->raw.bit_length);
        shell_printf(", \"first_seen_us\": %" PRIu32, mapping->first_seen_us);
        shell_printf(", \"seen_count\": %" PRIu32, mapping->seen_count);
        shell_printf(" }");
    }
    shell_printf("\r\n  ],\r\n");
    shell_print_capture_json();
    shell_printf("\r\n");
    shell_printf("}\r\n");
}

static void cmd_export(const DaliCliTokens *t)
{
    if (!dali_cli_has_subcommand(dali_cli_command_for_id(DALI_CLI_CMD_EXPORT),
                                 t->tok[1])) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_EXPORT));
        return;
    }

    if (strcmp(t->tok[1], "config") == 0) {
        cmd_export_config();
    } else {
        cmd_export_inventory();
    }
}

static void cmd_bus(const DaliCliTokens *t)
{
    if (!dali_cli_has_subcommand(dali_cli_command_for_id(DALI_CLI_CMD_BUS),
                                 t->tok[1])) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_BUS));
        return;
    }

    uint8_t rx_level = 0u;
    DaliError rx_err = dali_phy_read_rx_level(&rx_level);
    shell_printf("bus check:\r\n");
    if (rx_err == DALI_OK) {
        shell_printf("  RX level: %u (%s)\r\n",
               (unsigned)rx_level,
               rx_level != 0u ? "idle-high candidate" : "active-low/stuck-low candidate");
    } else {
        shell_printf("  RX level: unavailable (ERR %d)\r\n", (int)rx_err);
    }
    shell_printf("  scheduler: %s\r\n",
           shell_sched_state_name(dali_sched_state()));
    shell_printf("  last RX: %s\r\n", s_has_last_rx_frame ? "yes" : "none");
    if (s_has_last_rx_frame) {
        shell_print_frame("    ", &s_last_rx_frame);
    }
    shell_printf("  event queue: %u queued, %" PRIu32 " dropped\r\n",
           (unsigned)shell_event_queue_count_snapshot(),
           shell_event_queue_dropped_snapshot());

    uint32_t capture_dropped = 0u;
    bool capture_enabled = false;
    uint8_t capture_count =
        shell_capture_snapshot(NULL, 0u, &capture_dropped, &capture_enabled);
    shell_printf("  capture: %s, %u records, %" PRIu32 " dropped\r\n",
           capture_enabled ? "on" : "off",
           (unsigned)capture_count,
           capture_dropped);
    shell_printf("  stats: malformed=%" PRIu32 ", timeouts=%" PRIu32
           ", ignored=%" PRIu32 ", bus_idle_failures=%" PRIu32 "\r\n",
           g_dali_stats.malformed_frames,
           g_dali_stats.reply_timeouts,
           g_dali_stats.rx_ignored_outside_reply,
           g_dali_stats.bus_idle_failures);
}

static void cmd_smoke(const DaliCliTokens *t)
{
    uint8_t addr;
    if (!dali_cli_parse_short_addr(t->tok[1], &addr)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_SMOKE));
        return;
    }

    DaliTarget target = {
        .type = DALI_ADDR_SHORT,
        .address = addr,
    };
    uint8_t pass = 0u;
    uint8_t fail = 0u;
    uint8_t skip = 0u;

    shell_printf("smoke %u: query-only diagnostic pass\r\n", (unsigned)addr);

    DaliFrame status_reply = {0u, 0u};
    DaliError err = shell_query_status(target, &status_reply);
    if (err == DALI_OK) {
        uint8_t status = (uint8_t)(status_reply.data & 0xFFu);
        shell_printf("  status: PASS 0x%02X\r\n", (unsigned)status);
        pass++;

        DaliDiscoveryInventory *inventory = &s_inventory_scratch;
        if (!shell_inventory_snapshot(inventory)) {
            (void)dali_discovery_inventory_reset(inventory);
            inventory->valid = true;
        }
        if (dali_discovery_inventory_store_status(inventory, addr, status) == DALI_OK) {
            shell_inventory_replace(inventory);
        }
    } else {
        shell_printf("  status: ERR %d\r\n", (int)err);
        fail++;
    }

    struct {
        const char   *name;
        DaliCommandId id;
    } queries[] = {
        { "version",      DALI_CMD_QUERY_VERSION_NUMBER },
        { "device-type",  DALI_CMD_QUERY_DEVICE_TYPE },
        { "actual-level", DALI_CMD_QUERY_ACTUAL_LEVEL },
    };

    for (uint8_t i = 0u; i < (uint8_t)(sizeof(queries) / sizeof(queries[0])); i++) {
        uint8_t value = 0u;
        err = shell_query_u8(target, queries[i].id, 0u, &value);
        if (err == DALI_OK) {
            shell_printf("  %s: PASS %u (0x%02X)\r\n",
                   queries[i].name,
                   (unsigned)value,
                   (unsigned)value);
            pass++;
        } else if (err == DALI_ERR_TIMEOUT) {
            shell_printf("  %s: timeout/skip\r\n", queries[i].name);
            skip++;
        } else {
            shell_printf("  %s: ERR %d\r\n", queries[i].name, (int)err);
            fail++;
        }
    }

    DaliDiscoveryTransport transport = shell_discovery_transport();
    DaliDiscoveryInputDevice input;
    err = dali_discovery_query_input_device(&transport, addr, &input);
    if (err == DALI_OK) {
        shell_printf("  input instances: PASS %u\r\n",
               (unsigned)input.device.instance_count);
        pass++;
        shell_input_cache_store(&input);

        DaliDiscoveryInventory *inventory = &s_inventory_scratch;
        if (!shell_inventory_snapshot(inventory)) {
            (void)dali_discovery_inventory_reset(inventory);
            inventory->valid = true;
        }
        if (dali_discovery_inventory_update_input_device(inventory, &input) == DALI_OK) {
            shell_inventory_replace(inventory);
        }

        uint8_t count = dali_discovery_input_visible_instance_count(&input);
        for (uint8_t instance = 0u; instance < count; instance++) {
            if (input.instance_type_errors[instance] == DALI_OK) {
                shell_sensor_poll_instance(addr, &input.device.instances[instance]);
            }
        }
    } else if (err == DALI_ERR_TIMEOUT) {
        shell_printf("  input instances: timeout/skip\r\n");
        skip++;
    } else {
        shell_printf("  input instances: ERR %d\r\n", (int)err);
        fail++;
    }

    shell_printf("smoke %u: pass=%u fail=%u skip=%u\r\n",
           (unsigned)addr,
           (unsigned)pass,
           (unsigned)fail,
           (unsigned)skip);
}

static void cmd_identify(const DaliCliTokens *t)
{
    uint8_t addr;
    DaliTarget target;
    DaliFrame max_frame;
    DaliFrame min_frame;

    if (!dali_cli_parse_short_addr(t->tok[1], &addr)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_IDENTIFY));
        return;
    }

    target = (DaliTarget){ .type = DALI_ADDR_SHORT, .address = addr };
    if (dali_control_build_recall_max(target, &max_frame) != DALI_OK ||
        dali_control_build_recall_min(target, &min_frame) != DALI_OK) {
        shell_printf("identify: invalid address\r\n");
        return;
    }

    shell_printf("Blinking addr %u between min and max for %u seconds.\r\n",
           (unsigned)addr,
           (unsigned)((SHELL_IDENTIFY_CYCLES * SHELL_IDENTIFY_STEP_MS * 2u) / 1000u));

    for (uint8_t i = 0u; i < SHELL_IDENTIFY_CYCLES; i++) {
        DaliError err = shell_send_no_reply(&max_frame, false);
        if (err != DALI_OK) {
            shell_printf("identify: max ERR %d\r\n", (int)err);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(SHELL_IDENTIFY_STEP_MS));

        err = shell_send_no_reply(&min_frame, false);
        if (err != DALI_OK) {
            shell_printf("identify: min ERR %d\r\n", (int)err);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(SHELL_IDENTIFY_STEP_MS));
    }

    shell_printf("identify: done\r\n");
}

/* ---------------------------------------------------------------------------
 * Named tables
 * --------------------------------------------------------------------------*/

static void cmd_list(const DaliCliTokens *t)
{
    DaliCliTableId table;
    if (!dali_cli_table_find(t->tok[1], &table)) {
        shell_printf("list: unknown table '%s'\r\n", t->tok[1]);
        dali_cli_print_table_names(&s_out);
        return;
    }
    dali_cli_print_table(&s_out, table);
}

/*
 * Emit every table dali_cli.h exposes as one JSON object.
 *
 * This exists for a client-side line editor. Completion, hints, and argument
 * checking in a wrapper are otherwise a second copy of these tables, written in
 * another language, that drifts the first time a verb gains an argument. Asking
 * the firmware what it accepts means the client is wrong only when the
 * firmware is.
 *
 * Printed as one line per entry rather than assembled in a buffer: the full
 * dump is several kilobytes, which no single fixed buffer here should hold.
 */
static void shell_json_escape_print(const char *text)
{
    /* The tables hold verb names, argument sketches, and summaries; only the
     * quote and backslash can occur in practice, but both must be escaped for
     * the output to parse at all. */
    for (const char *c = text; *c != '\0'; c++) {
        if (*c == '"' || *c == '\\') {
            shell_printf("\\%c", *c);
        } else {
            shell_printf("%c", *c);
        }
    }
}

static void shell_print_gear_table_json(const char *label,
                                        uint8_t count,
                                        const DaliCliGearCommand *(*at)(uint8_t))
{
    shell_printf("  \"%s\": [", label);
    for (uint8_t i = 0u; i < count; i++) {
        const DaliCliGearCommand *cmd = at(i);
        if (cmd == NULL) {
            continue;
        }
        shell_printf("%s\n    {\"name\": \"", i == 0u ? "" : ",");
        shell_json_escape_print(cmd->name);
        shell_printf("\", \"needs_param\": %s, \"max_param\": %u}",
                     cmd->needs_param ? "true" : "false",
                     (unsigned)cmd->max_param);
    }
    shell_printf("\n  ],\n");
}

static void cmd_schema(void)
{
    shell_printf("{\n  \"verbs\": [");

    uint8_t verb_count = dali_cli_command_count();
    for (uint8_t i = 0u; i < verb_count; i++) {
        const DaliCliCommandSpec *spec = dali_cli_command_at(i);
        if (spec == NULL) {
            continue;
        }
        shell_printf("%s\n    {\"name\": \"", i == 0u ? "" : ",");
        shell_json_escape_print(spec->name);
        shell_printf("\", \"args\": \"");
        shell_json_escape_print(spec->args);
        shell_printf("\", \"summary\": \"");
        shell_json_escape_print(spec->summary);
        shell_printf("\", \"min_args\": %u, \"max_args\": %u",
                     (unsigned)spec->min_args, (unsigned)spec->max_args);
        if (spec->subcommands != NULL) {
            shell_printf(", \"subcommands\": \"");
            shell_json_escape_print(spec->subcommands);
            shell_printf("\"");
        }
        shell_printf("}");
    }
    shell_printf("\n  ],\n");

    shell_print_gear_table_json("query", dali_cli_query_count(), dali_cli_query_at);
    shell_print_gear_table_json("special", dali_cli_special_count(), dali_cli_special_at);
    shell_print_gear_table_json("config", dali_cli_config_count(), dali_cli_config_at);

    /* DT6/DT8 and the instance tables carry different fields, so they are
     * emitted by name only: a completer needs the spelling, and anything more
     * detailed is already in `list <table>` for a human to read. */
    shell_printf("  \"dt6\": [");
    for (uint8_t i = 0u; i < dali_cli_dt6_count(); i++) {
        const DaliCliDtCommand *cmd = dali_cli_dt6_at(i);
        if (cmd == NULL) {
            continue;
        }
        shell_printf("%s\"", i == 0u ? "" : ", ");
        shell_json_escape_print(cmd->name);
        shell_printf("\"");
    }
    shell_printf("],\n  \"dt8\": [");
    for (uint8_t i = 0u; i < dali_cli_dt8_count(); i++) {
        const DaliCliDtCommand *cmd = dali_cli_dt8_at(i);
        if (cmd == NULL) {
            continue;
        }
        shell_printf("%s\"", i == 0u ? "" : ", ");
        shell_json_escape_print(cmd->name);
        shell_printf("\"");
    }
    shell_printf("],\n  \"iquery\": [");
    for (uint8_t i = 0u; i < dali_cli_iquery_count(); i++) {
        const DaliCliInstanceQuery *cmd = dali_cli_iquery_at(i);
        if (cmd == NULL) {
            continue;
        }
        shell_printf("%s\"", i == 0u ? "" : ", ");
        shell_json_escape_print(cmd->name);
        shell_printf("\"");
    }
    shell_printf("],\n  \"iconfig\": [");
    for (uint8_t i = 0u; i < dali_cli_iconfig_count(); i++) {
        const DaliCliInstanceConfig *cmd = dali_cli_iconfig_at(i);
        if (cmd == NULL) {
            continue;
        }
        shell_printf("%s\"", i == 0u ? "" : ", ");
        shell_json_escape_print(cmd->name);
        shell_printf("\"");
    }

    /* What this session will actually run, so a client can grey out what it
     * cannot use rather than letting the operator discover it by refusal. */
    shell_printf("],\n  \"policy\": {\"commission\": %s, \"reset\": %s}\n}\r\n",
                 (s_session.policy & DALI_SHELL_ALLOW_COMMISSION) != 0u ? "true" : "false",
                 (s_session.policy & DALI_SHELL_ALLOW_RESET) != 0u ? "true" : "false");
}

/* ---------------------------------------------------------------------------
 * Memory
 * --------------------------------------------------------------------------*/

#define SHELL_MEMREAD_MAX_BYTES 32u

static void cmd_memread(const DaliCliTokens *t)
{
    const DaliCliCommandSpec *usage = dali_cli_command_for_id(DALI_CLI_CMD_MEMREAD);
    uint8_t addr;
    uint8_t bank;
    uint8_t offset;
    uint8_t count = 1u;

    if (!dali_cli_parse_short_addr(t->tok[1], &addr) ||
        !dali_cli_parse_u8(t->tok[2], 255u, &bank) ||
        !dali_cli_parse_u8(t->tok[3], 255u, &offset) ||
        (t->count == 5u &&
         (!dali_cli_parse_u8(t->tok[4], SHELL_MEMREAD_MAX_BYTES, &count) || count == 0u))) {
        dali_cli_print_usage(&s_out, usage);
        shell_printf("       count 1-%u; the block must end at offset 255\r\n",
               (unsigned)SHELL_MEMREAD_MAX_BYTES);
        return;
    }
    if ((unsigned)offset + (unsigned)count > 256u) {
        shell_printf("memread: bank %u has no location 0x%02X\r\n",
               (unsigned)bank, (unsigned)((unsigned)offset + count - 1u));
        return;
    }

    uint8_t buf[SHELL_MEMREAD_MAX_BYTES];
    DaliDiscoveryTransport transport = shell_discovery_transport();
    DaliError err = dali_memory_read_bytes(&transport, addr, bank, offset, buf, count);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, "memread", err);
        return;
    }

    shell_printf("memread %u:\r\n", (unsigned)addr);
    for (uint8_t i = 0u; i < count; i += 8u) {
        uint8_t chunk = (uint8_t)(count - i);
        if (chunk > 8u) { chunk = 8u; }
        dali_cli_print_memory_block(&s_out, bank, (uint8_t)(offset + i), &buf[i], chunk);
    }
}

static void shell_print_hex_bytes(const char *label, const uint8_t *data, uint8_t count)
{
    shell_printf("  %s:", label);
    for (uint8_t i = 0u; i < count; i++) {
        shell_printf(" %02X", (unsigned)data[i]);
    }
    shell_printf("\r\n");
}

static void cmd_meminfo(const DaliCliTokens *t)
{
    uint8_t addr;
    if (!dali_cli_parse_short_addr(t->tok[1], &addr)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_MEMINFO));
        return;
    }

    DaliDiscoveryTransport transport = shell_discovery_transport();
    DaliMemoryBank0Identity identity;
    DaliError err = dali_memory_read_bank0_identity(&transport, addr, &identity);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, "meminfo", err);
        return;
    }

    shell_printf("meminfo %u (control gear, bank 0):\r\n", (unsigned)addr);
    shell_print_hex_bytes("GTIN          ", identity.gtin, DALI_MEMORY_BANK0_GTIN_LEN);
    shell_printf("  firmware      : %u.%u\r\n",
           (unsigned)identity.fw_major, (unsigned)identity.fw_minor);
    shell_print_hex_bytes("identification", identity.serial,
                         DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
    shell_printf("  hardware      : %u.%u\r\n",
           (unsigned)identity.hw_major, (unsigned)identity.hw_minor);
}

/*
 * Part 103 control devices use different DTR and memory opcodes than Part 102
 * control gear, so they get their own verb rather than a flag on memread. A
 * bank 0 write is refused by the builder: bank 0 is read-only.
 */
static void cmd_devmem(const DaliCliTokens *t)
{
    const DaliCliCommandSpec *usage = dali_cli_command_for_id(DALI_CLI_CMD_DEVMEM);
    bool is_write = strcmp(t->tok[1], "write") == 0;
    uint8_t addr;
    uint8_t bank;
    uint8_t offset;

    if (!dali_cli_has_subcommand(usage, t->tok[1]) ||
        !dali_cli_parse_short_addr(t->tok[2], &addr) ||
        !dali_cli_parse_u8(t->tok[3], 255u, &bank) ||
        !dali_cli_parse_u8(t->tok[4], 255u, &offset)) {
        dali_cli_print_usage(&s_out, usage);
        return;
    }

    DaliSequence seq;
    DaliSequenceResult result;
    DaliError err;

    if (is_write) {
        uint8_t value;
        if (t->count != 6u || !dali_cli_parse_u8(t->tok[5], 255u, &value)) {
            shell_printf("usage: devmem write <addr> <bank> <offset> <value>\r\n");
            return;
        }
        err = dali_memory_build_control_device_write_sequence(addr, bank, offset,
                                                             value, &seq);
        if (err != DALI_OK) {
            dali_cli_print_error(&s_out, "devmem write", err);
            return;
        }
        err = shell_sched_sequence_sync(&seq, &result);
        shell_print_sequence_result("devmem write", err, &result);
        if (err == DALI_OK) {
            shell_printf("devmem write: queued and transmitted; not read back\r\n");
        }
        return;
    }

    uint8_t count = 1u;
    if (t->count == 6u &&
        (!dali_cli_parse_u8(t->tok[5], DALI_MEMORY_MAX_SEQUENCE_READ_BYTES, &count) ||
         count == 0u)) {
        shell_printf("usage: devmem read <addr> <bank> <offset> [1-%u]\r\n",
               (unsigned)DALI_MEMORY_MAX_SEQUENCE_READ_BYTES);
        return;
    }
    if ((unsigned)offset + (unsigned)count > 256u) {
        shell_printf("devmem read: bank %u has no location 0x%02X\r\n",
               (unsigned)bank, (unsigned)((unsigned)offset + count - 1u));
        return;
    }

    err = dali_memory_build_control_device_read_sequence(addr, bank, offset,
                                                         count, &seq);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, "devmem read", err);
        return;
    }

    err = shell_sched_sequence_sync(&seq, &result);
    if (err != DALI_OK) {
        shell_print_sequence_result("devmem read", err, &result);
        return;
    }

    uint8_t buf[DALI_MEMORY_MAX_SEQUENCE_READ_BYTES];
    err = dali_memory_read_from_sequence(&result, count, buf);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, "devmem read", err);
        return;
    }

    shell_printf("devmem read %u:\r\n", (unsigned)addr);
    dali_cli_print_memory_block(&s_out, bank, offset, buf, count);
}

/*
 * dtrcheck — load a control-device DTR and read it straight back.
 *
 * A DTR load produces no backward frame, so nothing otherwise confirms that a
 * control device accepted one. That matters when a device's configuration
 * writes appear to do nothing: this separates "the DTR never took the value"
 * from "the command that consumes it was ignored". Both frames go out as one
 * sequence, so no other locally scheduled DTR write can land between them.
 */
static void cmd_dtrcheck(const DaliCliTokens *t)
{
    const DaliCliCommandSpec *usage = dali_cli_command_for_id(DALI_CLI_CMD_DTRCHECK);
    uint8_t addr;
    uint8_t reg;
    uint8_t value;

    if (!dali_cli_parse_short_addr(t->tok[1], &addr) ||
        !dali_cli_parse_u8(t->tok[2], 2u, &reg) ||
        !dali_cli_parse_u8(t->tok[3], 255u, &value)) {
        dali_cli_print_usage(&s_out, usage);
        return;
    }

    DaliSequence seq;
    DaliError err = dali_input_build_dtr_check_sequence(addr, (DaliDtrRegister)reg,
                                                        value, &seq);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, "dtrcheck", err);
        return;
    }

    DaliSequenceResult result;
    err = shell_sched_sequence_sync(&seq, &result);
    if (err != DALI_OK) {
        shell_print_sequence_result("dtrcheck", err, &result);
        return;
    }

    DaliFrame reply;
    if (!dali_sequence_result_last_reply(&result, &reply)) {
        shell_printf("dtrcheck: DTR%u no reply\r\n", (unsigned)reg);
        return;
    }

    uint8_t read_back = (uint8_t)(reply.data & 0xFFu);
    shell_printf("dtrcheck: DTR%u wrote %u (0x%02X), read %u (0x%02X) - %s\r\n",
           (unsigned)reg, (unsigned)value, (unsigned)value,
           (unsigned)read_back, (unsigned)read_back,
           read_back == value ? "match" : "MISMATCH");
}

/* ---------------------------------------------------------------------------
 * Device types 6 and 8
 *
 * Every command here goes out as one sequence: the DTR loads, ENABLE DEVICE
 * TYPE, and the command itself cannot be separated without the gear answering
 * a different question than the one asked.
 * --------------------------------------------------------------------------*/

static bool shell_parse_dt_dtr_bytes(const DaliCliTokens *t,
                                    uint8_t              first_token,
                                    uint8_t              dtr_count,
                                    uint8_t             *dtr)
{
    for (uint8_t i = 0u; i < dtr_count; i++) {
        if (!dali_cli_parse_u8(t->tok[first_token + i], 255u, &dtr[i])) {
            return false;
        }
    }
    return true;
}

static void shell_run_dt_command(const char             *verb,
                                uint8_t                 device_type,
                                const DaliCliDtCommand *spec,
                                uint8_t                 addr,
                                const uint8_t          *dtr)
{
    DaliFrame command = spec->build(addr);
    if (command.bit_length == 0u) {
        dali_cli_print_error(&s_out, spec->name, DALI_ERR_INVALID);
        return;
    }

    bool send_twice = spec->kind == DALI_CLI_DT_CONFIG;
    bool expects_reply = spec->kind == DALI_CLI_DT_QUERY;

    DaliSequence seq;
    DaliError err = device_type == 6u
        ? dali_dt6_build_command_sequence(command, send_twice, expects_reply,
                                          dtr, spec->dtr_count, &seq)
        : dali_dt8_build_command_sequence(command, send_twice, expects_reply,
                                          dtr, spec->dtr_count, &seq);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, spec->name, err);
        return;
    }

    DaliSequenceResult result;
    err = shell_sched_sequence_sync(&seq, &result);
    if (err != DALI_OK) {
        shell_print_sequence_result(spec->name, err, &result);
        return;
    }

    if (!expects_reply) {
        shell_printf("%s %s: OK\r\n", verb, spec->name);
        return;
    }

    DaliFrame reply;
    uint8_t step = (uint8_t)(spec->dtr_count + 1u);
    if (!dali_sequence_result_reply(&result, step, &reply)) {
        shell_printf("%s: no reply captured\r\n", spec->name);
        return;
    }
    dali_cli_print_response(&s_out, spec->name, spec->response_kind, &reply);
}

static void cmd_dt6(const DaliCliTokens *t)
{
    uint8_t addr;
    if (!dali_cli_parse_short_addr(t->tok[1], &addr)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_DT6));
        return;
    }

    const DaliCliDtCommand *spec = dali_cli_dt6_find(t->tok[2]);
    if (spec == NULL) {
        shell_printf("dt6: unknown command '%s'\r\n", t->tok[2]);
        shell_printf("use 'list dt6'\r\n");
        return;
    }

    uint8_t dtr[DALI_DT6_MAX_DTR_BYTES] = {0};
    if (t->count != (uint8_t)(3u + spec->dtr_count) ||
        !shell_parse_dt_dtr_bytes(t, 3u, spec->dtr_count, dtr)) {
        shell_printf("usage: dt6 <addr> %s", spec->name);
        for (uint8_t i = 0u; i < spec->dtr_count; i++) {
            shell_printf(" <0-255>");
        }
        shell_printf("\r\n");
        if (spec->dtr_help != NULL) {
            shell_printf("       %s\r\n", spec->dtr_help);
        }
        return;
    }

    shell_run_dt_command("dt6", 6u, spec, addr, dtr);
}

static void cmd_dt8(const DaliCliTokens *t)
{
    uint8_t addr;
    if (!dali_cli_parse_short_addr(t->tok[1], &addr)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_DT8));
        return;
    }

    const DaliCliDtCommand *spec = dali_cli_dt8_find(t->tok[2]);
    if (spec == NULL) {
        shell_printf("dt8: unknown command '%s'\r\n", t->tok[2]);
        shell_printf("use 'list dt8'\r\n");
        return;
    }

    if (spec->kind == DALI_CLI_DT_COLOUR16) {
        const DaliCliDt8Selector *sel = t->count == 4u
                                      ? dali_cli_dt8_selector_find(t->tok[3])
                                      : NULL;
        if (sel == NULL) {
            shell_printf("usage: dt8 <addr> colour <selector>\r\n");
            shell_printf("       use 'list selectors'\r\n");
            return;
        }

        DaliSequence seq;
        DaliError err = dali_dt8_build_colour_value_sequence(addr, sel->selector, &seq);
        if (err != DALI_OK) {
            dali_cli_print_error(&s_out, "colour", err);
            return;
        }

        DaliSequenceResult result;
        err = shell_sched_sequence_sync(&seq, &result);
        if (err != DALI_OK) {
            shell_print_sequence_result("colour", err, &result);
            return;
        }

        uint16_t value = 0u;
        err = dali_dt8_colour_value_from_sequence(&result, &value);
        if (err != DALI_OK) {
            dali_cli_print_error(&s_out, "colour", err);
            return;
        }

        shell_printf("colour %s: %u (0x%04X)", sel->name, (unsigned)value, (unsigned)value);
        if (sel->selector == DALI_DT8_VALUE_COLOUR_TEMP_TC) {
            shell_printf(" = %u K", (unsigned)dali_dt8_mirek_to_kelvin(value));
        }
        shell_printf("\r\n");
        return;
    }

    uint8_t dtr[DALI_DT8_MAX_DTR_BYTES] = {0};
    if (t->count != (uint8_t)(3u + spec->dtr_count) ||
        !shell_parse_dt_dtr_bytes(t, 3u, spec->dtr_count, dtr)) {
        shell_printf("usage: dt8 <addr> %s", spec->name);
        for (uint8_t i = 0u; i < spec->dtr_count; i++) {
            shell_printf(" <0-255>");
        }
        shell_printf("\r\n");
        if (spec->dtr_help != NULL) {
            shell_printf("       %s\r\n", spec->dtr_help);
        }
        return;
    }

    shell_run_dt_command("dt8", 8u, spec, addr, dtr);
}

/* ---------------------------------------------------------------------------
 * Part 103 instance query and configuration
 * --------------------------------------------------------------------------*/

static void cmd_iquery(const DaliCliTokens *t)
{
    uint8_t addr;
    uint8_t instance;

    if (!dali_cli_parse_short_addr(t->tok[1], &addr) ||
        !dali_cli_parse_instance(t->tok[2], &instance)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_IQUERY));
        return;
    }

    const DaliCliInstanceQuery *spec = dali_cli_iquery_find(t->tok[3]);
    if (spec == NULL) {
        shell_printf("iquery: unknown query '%s'\r\n", t->tok[3]);
        shell_printf("use 'list iquery'\r\n");
        return;
    }

    uint8_t dtr0 = 0u;
    uint8_t expected = spec->needs_dtr0 ? 5u : 4u;
    if (t->count != expected ||
        (spec->needs_dtr0 && !dali_cli_parse_u8(t->tok[4], 255u, &dtr0))) {
        shell_printf("usage: iquery <addr> <instance> %s%s\r\n",
               spec->name,
               spec->needs_dtr0 ? " <0-255>" : "");
        if (spec->dtr0_help != NULL) {
            shell_printf("       %s\r\n", spec->dtr0_help);
        }
        return;
    }

    DaliFrame command = spec->build(addr, instance);
    if (command.bit_length == 0u) {
        dali_cli_print_error(&s_out, spec->name, DALI_ERR_INVALID);
        return;
    }

    DaliFrame reply = {0u, 0u};
    DaliError err;

    if (spec->needs_dtr0) {
        /* The selector and the query that reads it must stay together. */
        DaliSequence seq;
        err = dali_input_build_config_sequence(command, false, true, &dtr0, 1u, &seq);
        if (err != DALI_OK) {
            dali_cli_print_error(&s_out, spec->name, err);
            return;
        }

        DaliSequenceResult result;
        err = shell_sched_sequence_sync(&seq, &result);
        if (err != DALI_OK) {
            shell_print_sequence_result(spec->name, err, &result);
            return;
        }
        if (!dali_sequence_result_reply(&result, 1u, &reply)) {
            shell_printf("%s: no reply captured\r\n", spec->name);
            return;
        }
    } else {
        /* Plain instance reads change nothing on the device, so one retry after
         * a lost reply is safe. */
        err = shell_sched_sync(&command, true, 1u, false, &reply);
        if (err != DALI_OK) {
            dali_cli_print_error(&s_out, spec->name, err);
            return;
        }
    }

    dali_cli_print_response(&s_out, spec->name, spec->response_kind, &reply);
}

static void cmd_iconfig(const DaliCliTokens *t)
{
    uint8_t addr;
    uint8_t instance;

    if (!dali_cli_parse_short_addr(t->tok[1], &addr) ||
        !dali_cli_parse_instance(t->tok[2], &instance)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_ICONFIG));
        return;
    }

    const DaliCliInstanceConfig *spec = dali_cli_iconfig_find(t->tok[3]);
    if (spec == NULL) {
        shell_printf("iconfig: unknown command '%s'\r\n", t->tok[3]);
        shell_printf("use 'list iconfig'\r\n");
        return;
    }

    uint8_t dtr[DALI_INPUT_CONFIG_MAX_DTR_BYTES] = {0};
    if (t->count != (uint8_t)(4u + spec->dtr_count) ||
        !shell_parse_dt_dtr_bytes(t, 4u, spec->dtr_count, dtr)) {
        shell_printf("usage: iconfig <addr> <instance> %s", spec->name);
        for (uint8_t i = 0u; i < spec->dtr_count; i++) {
            shell_printf(" <0-255>");
        }
        shell_printf("\r\n");
        if (spec->dtr_help != NULL) {
            shell_printf("       %s\r\n", spec->dtr_help);
        }
        return;
    }

    /* Out-of-range configuration bytes are refused here rather than sent: some
     * gear stores them, and the mistake then shows up as odd behaviour later
     * instead of as a rejected command. */
    for (uint8_t i = 0u; i < spec->dtr_count; i++) {
        if (!dali_cli_dtr_value_valid(&spec->dtr_range[i], dtr[i])) {
            shell_printf("iconfig: DTR%u value %u out of range\r\n",
                   (unsigned)i, (unsigned)dtr[i]);
            if (spec->dtr_help != NULL) {
                shell_printf("       %s\r\n", spec->dtr_help);
            }
            return;
        }
    }

    DaliFrame command = spec->build(addr, instance);
    if (command.bit_length == 0u) {
        dali_cli_print_error(&s_out, spec->name, DALI_ERR_INVALID);
        return;
    }

    DaliSequence seq;
    DaliError err = dali_input_build_config_sequence(command, spec->send_twice, false,
                                                     dtr, spec->dtr_count, &seq);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, spec->name, err);
        return;
    }

    DaliSequenceResult result;
    err = shell_sched_sequence_sync(&seq, &result);
    shell_print_sequence_result(spec->name, err, &result);
    if (err == DALI_OK) {
        /* Transmitted, not acknowledged: read the value back with iquery before
         * treating an input-device configuration write as applied. */
        shell_printf("%s: transmitted; verify with iquery\r\n", spec->name);
    }
}

/* ---------------------------------------------------------------------------
 * Vendor helpers
 * --------------------------------------------------------------------------*/

static void cmd_vendor(const DaliCliTokens *t)
{
    const DaliCliCommandSpec *usage = dali_cli_command_for_id(DALI_CLI_CMD_VENDOR);

    if (!dali_cli_has_subcommand(usage, t->tok[1])) {
        dali_cli_print_usage(&s_out, usage);
        return;
    }

    if (strcmp(t->tok[1], "lunatone") == 0) {
        uint8_t addr;
        uint8_t instance;
        if (t->count != 5u ||
            !dali_cli_parse_short_addr(t->tok[2], &addr) ||
            !dali_cli_parse_instance(t->tok[3], &instance)) {
            shell_printf("usage: vendor lunatone <addr> <instance> <name>\r\n");
            return;
        }

        const DaliCliLunatoneCommand *spec = dali_cli_lunatone_find(t->tok[4]);
        if (spec == NULL) {
            shell_printf("vendor: unknown lunatone query '%s'\r\n", t->tok[4]);
            shell_printf("use 'list vendor'\r\n");
            return;
        }

        DaliFrame frame;
        DaliFrame reply = {0u, 0u};
        DaliError err = dali_lunatone_build_instance_command(addr, instance,
                                                             spec->id, &frame);
        if (err == DALI_OK) {
            err = shell_sched_sync(&frame, true, 1u, false, &reply);
        }
        if (err != DALI_OK) {
            dali_cli_print_error(&s_out, spec->name, err);
            return;
        }

        const DaliLunatoneCommandInfo *info = dali_lunatone_command_lookup(spec->id);
        dali_cli_print_response(&s_out, spec->name,
                                info != NULL ? info->response_kind : DALI_RESP_UINT8,
                                &reply);
        return;
    }

    if (strcmp(t->tok[1], "steinel") == 0) {
        uint8_t instance;
        uint32_t raw;
        if (t->count != 4u ||
            !dali_cli_parse_instance(t->tok[2], &instance) ||
            !dali_cli_parse_u32(t->tok[3], 0xFFFFu, &raw)) {
            shell_printf("usage: vendor steinel <instance> <raw>\r\n");
            return;
        }

        const DaliSteinelInstanceInfo *info =
            dali_steinel_hf360_instance_lookup(instance);
        if (info == NULL) {
            shell_printf("vendor steinel: instance %u is not in the HF 360 II profile\r\n",
                   (unsigned)instance);
            return;
        }

        shell_printf("steinel %s (instance %u, type %u): raw=%u\r\n",
               info->name, (unsigned)instance, (unsigned)info->type, (unsigned)raw);
        switch (instance) {
            case DALI_STEINEL_HF360_INSTANCE_TEMPERATURE: {
                int32_t deci = dali_steinel_temperature_deci_c((uint16_t)raw);
                shell_printf("  temperature: %ld.%ld C\r\n",
                       (long)(deci / 10), (long)(deci < 0 ? -(deci % 10) : deci % 10));
                break;
            }
            case DALI_STEINEL_HF360_INSTANCE_HUMIDITY: {
                uint32_t deci = dali_steinel_humidity_deci_percent((uint16_t)raw);
                shell_printf("  humidity: %lu.%lu %%\r\n",
                       (unsigned long)(deci / 10u), (unsigned long)(deci % 10u));
                break;
            }
            case DALI_STEINEL_HF360_INSTANCE_BRIGHTNESS:
                shell_printf("  illuminance: %lu.%02lu lx (scale 0.01)\r\n",
                       (unsigned long)(raw / 100u), (unsigned long)(raw % 100u));
                break;
            default:
                shell_printf("  no conversion defined; the raw value is authoritative\r\n");
                break;
        }
        return;
    }

    dali_cli_print_usage(&s_out, usage);
}

static void cmd_help(void)
{
    dali_cli_print_help(&s_out);
}

/* ---------------------------------------------------------------------------
 * Dispatch
 *
 * The switch has no default case, so a verb added to the CLI table without a
 * handler here is a -Wswitch diagnostic rather than a silently dead command.
 * test_cli additionally asserts that the table and DaliCliCommandId agree.
 * --------------------------------------------------------------------------*/

static void shell_execute(DaliCliCommandId id, const DaliCliTokens *t)
{
    switch (id) {
        case DALI_CLI_CMD_HELP:         cmd_help(); break;
        case DALI_CLI_CMD_STATS:        cmd_stats(); break;
        case DALI_CLI_CMD_QUEUE:        cmd_queue(t); break;
        case DALI_CLI_CMD_BUS:          cmd_bus(t); break;
        case DALI_CLI_CMD_CAPTURE:      cmd_capture(t); break;
        case DALI_CLI_CMD_TRACE:        cmd_trace(t); break;
        case DALI_CLI_CMD_READ:         cmd_read(); break;
        case DALI_CLI_CMD_RXDEBUG:      cmd_rxdebug(); break;
        case DALI_CLI_CMD_RESET:        cmd_reset(); break;

        case DALI_CLI_CMD_LIST:         cmd_list(t); break;
        case DALI_CLI_CMD_SCHEMA:       cmd_schema(); break;
        case DALI_CLI_CMD_QUERY_LIST:   dali_cli_print_table(&s_out, DALI_CLI_TABLE_QUERY); break;
        case DALI_CLI_CMD_SPECIAL_LIST: dali_cli_print_table(&s_out, DALI_CLI_TABLE_SPECIAL); break;
        case DALI_CLI_CMD_CONFIG_LIST:  dali_cli_print_table(&s_out, DALI_CLI_TABLE_CONFIG); break;

        case DALI_CLI_CMD_RAW:          cmd_raw(t, false); break;
        case DALI_CLI_CMD_RAW2:         cmd_raw(t, true); break;
        case DALI_CLI_CMD_DTR:          cmd_dtr(t); break;

        case DALI_CLI_CMD_LEVEL:        cmd_level(t); break;
        case DALI_CLI_CMD_MASK:         cmd_mask(t); break;
        case DALI_CLI_CMD_OFF:
            cmd_target_frame(t, id, dali_control_build_off); break;
        case DALI_CLI_CMD_UP:
            cmd_target_frame(t, id, dali_control_build_up); break;
        case DALI_CLI_CMD_DOWN:
            cmd_target_frame(t, id, dali_control_build_down); break;
        case DALI_CLI_CMD_STEP_UP:
            cmd_target_frame(t, id, dali_control_build_step_up); break;
        case DALI_CLI_CMD_STEP_DOWN:
            cmd_target_frame(t, id, dali_control_build_step_down); break;
        case DALI_CLI_CMD_STEP_OFF:
            cmd_target_frame(t, id, dali_control_build_step_down_and_off); break;
        case DALI_CLI_CMD_ON_STEP:
            cmd_target_frame(t, id, dali_control_build_on_and_step_up); break;
        case DALI_CLI_CMD_CONT_UP:
            cmd_target_frame(t, id, dali_control_build_continuous_up); break;
        case DALI_CLI_CMD_CONT_DOWN:
            cmd_target_frame(t, id, dali_control_build_continuous_down); break;
        case DALI_CLI_CMD_DAPC_SEQ:
            cmd_target_frame(t, id, dali_control_build_enable_dapc_sequence); break;
        case DALI_CLI_CMD_LAST:
            cmd_target_frame(t, id, dali_control_build_go_to_last_active_level); break;
        case DALI_CLI_CMD_MAX:
            cmd_target_frame(t, id, dali_control_build_recall_max); break;
        case DALI_CLI_CMD_MIN:
            cmd_target_frame(t, id, dali_control_build_recall_min); break;
        case DALI_CLI_CMD_SCENE:        cmd_scene(t); break;
        case DALI_CLI_CMD_STATUS:       cmd_status(t); break;

        case DALI_CLI_CMD_QUERY:        cmd_query(t); break;
        case DALI_CLI_CMD_SPECIAL:      cmd_special(t); break;
        case DALI_CLI_CMD_CONFIG:       cmd_config(t); break;
        case DALI_CLI_CMD_CONFIG_DTR0:  cmd_config_dtr0(t); break;

        case DALI_CLI_CMD_MEMREAD:      cmd_memread(t); break;
        case DALI_CLI_CMD_MEMINFO:      cmd_meminfo(t); break;
        case DALI_CLI_CMD_DEVMEM:       cmd_devmem(t); break;
        case DALI_CLI_CMD_DTRCHECK:     cmd_dtrcheck(t); break;

        case DALI_CLI_CMD_DT6:          cmd_dt6(t); break;
        case DALI_CLI_CMD_DT8:          cmd_dt8(t); break;

        case DALI_CLI_CMD_IQUERY:       cmd_iquery(t); break;
        case DALI_CLI_CMD_ICONFIG:      cmd_iconfig(t); break;
        case DALI_CLI_CMD_VENDOR:       cmd_vendor(t); break;

        case DALI_CLI_CMD_SCAN:         cmd_scan(); break;
        case DALI_CLI_CMD_DISCOVER:     cmd_discover(); break;
        case DALI_CLI_CMD_INVENTORY:    cmd_inventory(); break;
        case DALI_CLI_CMD_COMMISSION:   cmd_commission(t); break;
        case DALI_CLI_CMD_INSTANCES:    cmd_instances(t); break;
        case DALI_CLI_CMD_SENSOR:       cmd_sensor(t); break;
        case DALI_CLI_CMD_SMOKE:        cmd_smoke(t); break;
        case DALI_CLI_CMD_EVENTS:       cmd_events(); break;
        case DALI_CLI_CMD_FIND:         cmd_find(t); break;
        case DALI_CLI_CMD_EXPORT:       cmd_export(t); break;
        case DALI_CLI_CMD_IDENTIFY:     cmd_identify(t); break;

        case DALI_CLI_CMD_COUNT:
            break;
    }
}

#endif /* !DALI_HOST_BUILD */

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------*/

/* Clear everything a session accumulates, so a new operator never inherits the
 * previous one's view of the bus. */
static void shell_reset_caches(void)
{
    s_trace_enabled = false;
    shell_last_rx_reset();
    shell_inventory_reset();
    shell_events_reset();
    shell_switch_mappings_reset();
    shell_input_cache_reset();
    shell_sensor_value_cache_reset();
    shell_capture_reset();
}

DaliError dali_shell_init(void)
{
    s_attached = false;
    memset(&s_session, 0, sizeof(s_session));
    shell_reset_caches();

    /*
     * Registered for the life of the firmware rather than per session: a
     * subscriber removed while the DALI task is running can still be entered
     * once more, and the handlers below are written to be harmless when nothing
     * is attached. See dali_scheduler.h.
     */
    DaliError err = dali_sched_add_trace_subscriber(dali_shell_on_trace, NULL);
    if (err != DALI_OK) {
        return err;
    }
    err = dali_sched_add_event_subscriber(dali_shell_on_event, NULL);
    if (err != DALI_OK) {
        return err;
    }

    return DALI_OK;
}

DaliError dali_shell_attach(const DaliShellSession *session)
{
    if (session == NULL || session->out.write == NULL ||
        !dali_transport_valid(&session->transport)) {
        return DALI_ERR_INVALID;
    }
    if (s_attached) {
        return DALI_ERR_BUSY;
    }

    s_session = *session;
#ifndef DALI_HOST_BUILD
    /* Recorded before the session goes live, so the very first foreign-task
     * write is already classified correctly. */
    s_session_task = xTaskGetCurrentTaskHandle();
#endif
    shell_deferred_reset();
    shell_line_reset();
    s_attached = true;
    shell_reset_caches();
    return DALI_OK;
}

void dali_shell_detach(void)
{
    /* Clear the attach flag before the binding: shell_printf() and
     * shell_sink_write() both test it, so no in-flight formatting can reach a
     * sink the front end has already torn down. */
    s_attached = false;
    memset(&s_session, 0, sizeof(s_session));
#ifndef DALI_HOST_BUILD
    s_session_task = NULL;
#endif
    shell_deferred_reset();
}

uint32_t dali_shell_pump_deferred_output(void)
{
    char line[SHELL_DEFERRED_LINE_MAX];

    while (s_attached && shell_deferred_pop(line, sizeof(line))) {
        if (s_session.out.write != NULL) {
            s_session.out.write(s_session.out.ctx, line);
        }
    }

    SHELL_DEFERRED_ENTER();
    uint32_t dropped   = s_deferred_dropped;
    s_deferred_dropped = 0u;
    SHELL_DEFERRED_EXIT();
    return dropped;
}

bool dali_shell_is_attached(void)
{
    return s_attached;
}

void dali_shell_write_banner(void)
{
    shell_printf("\r\nDALI-2 diagnostic shell. Type 'help' for commands.\r\n");
}

void dali_shell_write_prompt(void)
{
    shell_printf("> ");
}

void dali_shell_dispatch(const char *line)
{
    if (line == NULL || !s_attached) {
        return;
    }

    DaliCliTokens tokens;
    const DaliCliCommandSpec *spec = NULL;

    DaliCliResolveResult result = dali_cli_resolve(line, &tokens, &spec);
    if (result == DALI_CLI_RESOLVE_EMPTY) {
        return;
    }

    shell_printf("> %s\r\n", line);

    if (result != DALI_CLI_RESOLVE_OK) {
        dali_cli_report_resolve(&s_out, result, &tokens, spec);
        return;
    }

    /*
     * Policy is applied here rather than inside each handler so that a verb
     * cannot be reached by a path that forgot to ask. `special` checks its own
     * argument against dali_cli_special_is_commissioning() as well, because the
     * verb itself is permitted while nine of its names are not.
     */
    if (spec->id == DALI_CLI_CMD_COMMISSION &&
        !shell_policy_allows(DALI_SHELL_ALLOW_COMMISSION, spec->name)) {
        return;
    }
    if (spec->id == DALI_CLI_CMD_RESET &&
        !shell_policy_allows(DALI_SHELL_ALLOW_RESET, spec->name)) {
        return;
    }

    shell_execute(spec->id, &tokens);
}

bool dali_shell_feed_byte(uint8_t ch)
{
    /*
     * Swallow the LF of a CRLF pair. Without this a client that ends lines the
     * usual network way dispatches twice per line — once for the CR, once for
     * an empty line — and a front end that writes a prompt per dispatched line
     * emits two. That is invisible to a human, but it desynchronises any client
     * that reads replies up to the prompt.
     */
    bool follows_cr = s_line_last_was_cr;
    s_line_last_was_cr = (ch == '\r');
    if (ch == '\n' && follows_cr) {
        return false;
    }

    if (ch == '\n' || ch == '\r') {
        if (s_line_overflowed) {
            /* Rejected whole rather than truncated: a clipped line can
             * otherwise become a different, valid command. */
            shell_printf("line too long (max %u)\r\n",
                         (unsigned)(DALI_SHELL_LINE_MAX - 1u));
            shell_line_reset();
            return true;
        }
        s_line[s_line_len] = '\0';
        s_line_len         = 0u;
        dali_shell_dispatch(s_line);
        return true;
    }

    if (ch == '\b' || ch == 127u) {
        if (s_line_len > 0u) {
            s_line_len--;
        }
        return false;
    }

    if (s_line_len < (DALI_SHELL_LINE_MAX - 1u)) {
        s_line[s_line_len++] = (char)ch;
    } else {
        s_line_overflowed = true;
    }
    return false;
}

bool dali_shell_trace_enabled(void)
{
    return s_trace_enabled;
}
