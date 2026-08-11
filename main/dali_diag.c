#include "dali_diag.h"
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

#ifndef DALI_HOST_BUILD
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#endif

static const char *TAG = "DALI-DIAG";

#define DIAG_TASK_STACK   8192u
#define DIAG_TASK_PRIORITY  2u
#define DIAG_LINE_MAX      80u
#define DIAG_UART_NUM       0u   /* UART0 = default serial monitor port */
#define DIAG_UART_BAUD 115200u
#define DIAG_UART_RX_BUFFER_SIZE 1024u
#define DIAG_UART_TX_BUFFER_SIZE 1024u

static volatile bool s_trace_enabled;

/* ---------------------------------------------------------------------------
 * Synchronous scheduler helper — device builds only
 *
 * Enqueues one transaction and blocks the calling task via FreeRTOS task
 * notification until the completion callback fires (or 200 ms elapses).
 * The DALI processing task must be calling dali_sched_run() concurrently.
 * --------------------------------------------------------------------------*/
#ifndef DALI_HOST_BUILD

#define DIAG_SYNC_SLOT_COUNT 4u
#define DIAG_SYNC_WAIT_MS  200u
#define DIAG_RESET_WAIT_MS 1000u
#define DIAG_IDENTIFY_CYCLES 5u
#define DIAG_IDENTIFY_STEP_MS 1000u
#define DIAG_FIND_SWITCH_DEFAULT_SECONDS 30u
#define DIAG_FIND_SWITCH_MAX_SECONDS 300u
#define DIAG_SWITCH_MAPPING_MAX 32u
#define DIAG_INPUT_CACHE_MAX 16u
#define DIAG_SENSOR_VALUE_CACHE_MAX 64u
#define DIAG_CAPTURE_MAX 128u

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
    DIAG_CAPTURE_TX = 0,
    DIAG_CAPTURE_RX,
    DIAG_CAPTURE_EVENT,
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

/* All CLI output goes through dali_cli's sink so the device and the host tests
 * exercise the same formatting code. */
static void diag_stdout_write(void *ctx, const char *text)
{
    (void)ctx;
    fputs(text, stdout);
}

static const DaliCliOut s_out = { .write = diag_stdout_write, .ctx = NULL };

static DiagSyncCtx s_diag_sync_slots[DIAG_SYNC_SLOT_COUNT];
static portMUX_TYPE s_diag_sync_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_diag_state_mux = portMUX_INITIALIZER_UNLOCKED;
static DaliFrame s_last_rx_frame;
static uint32_t  s_last_rx_timestamp_us;
static uint32_t  s_last_rx_since_tx_us;
static bool      s_last_rx_has_since_tx;
static bool      s_has_last_rx_frame;
static DaliDiscoveryInventory s_inventory;
static DaliInputEventQueue s_event_queue;
static DiagSwitchMapping s_switch_mappings[DIAG_SWITCH_MAPPING_MAX];
static uint8_t s_switch_mapping_count;
static DiagInputCacheEntry s_input_cache[DIAG_INPUT_CACHE_MAX];
static DiagSensorValueCacheEntry s_sensor_value_cache[DIAG_SENSOR_VALUE_CACHE_MAX];
static DiagCaptureRecord s_capture[DIAG_CAPTURE_MAX];
static DiagCaptureRecord s_capture_export[DIAG_CAPTURE_MAX];
static uint8_t s_capture_head;
static uint8_t s_capture_count;
static uint32_t s_capture_dropped;
static bool s_capture_enabled;

static DaliError diag_uart_init(void)
{
    uart_port_t uart_num = (uart_port_t)DIAG_UART_NUM;

    if (!uart_is_driver_installed(uart_num)) {
        esp_err_t err = uart_driver_install(uart_num,
                                            DIAG_UART_RX_BUFFER_SIZE,
                                            DIAG_UART_TX_BUFFER_SIZE,
                                            0,
                                            NULL,
                                            0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install failed: %d", (int)err);
            return DALI_ERR_INVALID;
        }
    }

    esp_err_t err = uart_set_baudrate(uart_num, DIAG_UART_BAUD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_baudrate failed: %d", (int)err);
        return DALI_ERR_INVALID;
    }

    uart_vfs_dev_use_driver(DIAG_UART_NUM);
    return DALI_OK;
}

static DiagSyncCtx *diag_sync_alloc_slot(TaskHandle_t waiting_task)
{
    DiagSyncCtx *slot = NULL;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    for (uint8_t i = 0u; i < DIAG_SYNC_SLOT_COUNT; i++) {
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

static void diag_sync_release_slot(DiagSyncCtx *ctx)
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

static void diag_sync_cb(DaliError result, const DaliFrame *reply, void *cb_ctx)
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

static void diag_sequence_sync_cb(const DaliSequenceResult *result, void *cb_ctx)
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

static bool diag_sync_complete(DiagSyncCtx *ctx)
{
    bool complete;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    complete = ctx->complete;
    taskEXIT_CRITICAL(&s_diag_sync_mux);

    return complete;
}

/* Runs on the DALI scheduler owner task inside the reset admission barrier. */
static void diag_reset_owner_cb(void *cb_ctx)
{
    DaliError result = dali_phy_reset();
    diag_sync_cb(result, NULL, cb_ctx);
}

static DaliError diag_reset_sync(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0u);

    DiagSyncCtx *ctx = diag_sync_alloc_slot(current_task);
    if (ctx == NULL) {
        return DALI_ERR_BUSY;
    }

    DaliError err = dali_sched_request_reset(diag_reset_owner_cb, ctx);
    if (err != DALI_OK) {
        diag_sync_release_slot(ctx);
        return err;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(DIAG_RESET_WAIT_MS);
    TickType_t start_tick = xTaskGetTickCount();
    while (!diag_sync_complete(ctx)) {
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
 * Enqueue frame, wait up to DIAG_SYNC_WAIT_MS for completion.
 * retries_left is the scheduler retry budget after the first attempt.
 * reply_out may be NULL when the reply data is not needed.
 */
static DaliError diag_sched_sync(const DaliFrame *frame, bool needs_reply,
                                  uint8_t retries_left, bool send_twice,
                                  DaliFrame *reply_out)
{
    if (frame == NULL) {
        return DALI_ERR_INVALID;
    }

    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0u);

    DiagSyncCtx *ctx = diag_sync_alloc_slot(current_task);
    if (ctx == NULL) {
        return DALI_ERR_BUSY;
    }

    DaliTransaction txn = {
        .frame        = *frame,
        .needs_reply  = needs_reply,
        .send_twice   = send_twice,
        .retries_left = retries_left,
        .on_complete  = diag_sync_cb,
        .cb_ctx       = ctx,
    };
    DaliError err = dali_sched_enqueue(&txn);
    if (err != DALI_OK) {
        diag_sync_release_slot(ctx);
        return err;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(DIAG_SYNC_WAIT_MS);
    TickType_t start_tick = xTaskGetTickCount();
    while (!diag_sync_complete(ctx)) {
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
static void diag_sequence_result_init(DaliSequenceResult *result_out,
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
static DaliError diag_sched_sequence_sync(DaliSequence *seq,
                                          DaliSequenceResult *result_out)
{
    diag_sequence_result_init(result_out, DALI_ERR_INVALID);

    if (seq == NULL) {
        return DALI_ERR_INVALID;
    }

    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0u);

    DiagSyncCtx *ctx = diag_sync_alloc_slot(current_task);
    if (ctx == NULL) {
        diag_sequence_result_init(result_out, DALI_ERR_BUSY);
        return DALI_ERR_BUSY;
    }

    seq->on_complete = diag_sequence_sync_cb;
    seq->cb_ctx = ctx;

    DaliError err = dali_sched_enqueue_sequence(seq);
    if (err != DALI_OK) {
        diag_sync_release_slot(ctx);
        diag_sequence_result_init(result_out, err);
        return err;
    }

    /* A multi-step sequence can outlast the single-frame wait several times
     * over, so size the budget from the sequence itself. */
    TickType_t wait_ticks = pdMS_TO_TICKS(dali_transport_sequence_timeout_ms(seq));
    TickType_t start_tick = xTaskGetTickCount();
    while (!diag_sync_complete(ctx)) {
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

static DaliError diag_discovery_transact(const DaliFrame *frame,
                                         bool needs_reply,
                                         uint8_t retries_left,
                                         bool send_twice,
                                         DaliFrame *reply_out,
                                         void *ctx)
{
    (void)ctx;
    return diag_sched_sync(frame,
                           needs_reply,
                           retries_left,
                           send_twice,
                           reply_out);
}

/* Atomic-group half of the transport: shared protocol code that needs a DTR
 * setup and its consuming command to stay together gets that here too, not just
 * in the ESPHome scan task. */
static DaliError diag_discovery_sequence_transact(const DaliSequence *seq,
                                                  DaliSequenceResult *result_out,
                                                  void *ctx)
{
    (void)ctx;
    if (seq == NULL) {
        return DALI_ERR_INVALID;
    }

    /* diag_sched_sequence_sync installs its own completion callback. */
    DaliSequence local = *seq;
    return diag_sched_sequence_sync(&local, result_out);
}

static DaliDiscoveryTransport diag_discovery_transport(void)
{
    return (DaliDiscoveryTransport){
        .transact = diag_discovery_transact,
        .transact_sequence = diag_discovery_sequence_transact,
        .ctx = NULL,
    };
}

static int diag_frame_hex_width(const DaliFrame *frame)
{
    return (int)((frame->bit_length + 3u) / 4u);
}

static void diag_print_frame(const char *prefix, const DaliFrame *frame)
{
    dali_cli_print_frame(&s_out, prefix, frame);
}

static void diag_store_last_rx(const DaliSchedTraceEvent *event)
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

static void diag_inventory_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    (void)dali_discovery_inventory_reset(&s_inventory);
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void diag_last_rx_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    s_last_rx_frame        = (DaliFrame){0u, 0u};
    s_last_rx_timestamp_us = 0u;
    s_last_rx_since_tx_us  = 0u;
    s_last_rx_has_since_tx = false;
    s_has_last_rx_frame    = false;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void diag_inventory_replace(const DaliDiscoveryInventory *inventory)
{
    if (inventory == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    s_inventory = *inventory;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static bool diag_inventory_snapshot(DaliDiscoveryInventory *out)
{
    if (out == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    *out = s_inventory;
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return out->valid;
}

static void diag_events_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    (void)dali_event_queue_init(&s_event_queue);
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static bool diag_event_pop(DaliInputEventRecord *out)
{
    bool popped;

    taskENTER_CRITICAL(&s_diag_state_mux);
    popped = dali_event_queue_pop(&s_event_queue, out);
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return popped;
}

static uint32_t diag_event_queue_dropped_snapshot(void)
{
    uint32_t dropped;

    taskENTER_CRITICAL(&s_diag_state_mux);
    dropped = dali_event_queue_dropped(&s_event_queue);
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return dropped;
}

static uint8_t diag_event_queue_count_snapshot(void)
{
    uint8_t count;

    taskENTER_CRITICAL(&s_diag_state_mux);
    count = dali_event_queue_count(&s_event_queue);
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return count;
}

static void diag_switch_mappings_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    memset(s_switch_mappings, 0, sizeof(s_switch_mappings));
    s_switch_mapping_count = 0u;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void diag_input_cache_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    memset(s_input_cache, 0, sizeof(s_input_cache));
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void diag_sensor_value_cache_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    memset(s_sensor_value_cache, 0, sizeof(s_sensor_value_cache));
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void diag_sensor_value_cache_store(uint8_t addr,
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
    uint8_t slot = DIAG_SENSOR_VALUE_CACHE_MAX;
    for (uint8_t i = 0u; i < DIAG_SENSOR_VALUE_CACHE_MAX; i++) {
        if (s_sensor_value_cache[i].valid &&
            s_sensor_value_cache[i].address == addr &&
            s_sensor_value_cache[i].instance == info->instance) {
            slot = i;
            break;
        }
        if (!s_sensor_value_cache[i].valid && slot == DIAG_SENSOR_VALUE_CACHE_MAX) {
            slot = i;
        }
    }
    if (slot == DIAG_SENSOR_VALUE_CACHE_MAX) {
        slot = (uint8_t)(((uint16_t)addr + info->instance) %
                         DIAG_SENSOR_VALUE_CACHE_MAX);
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

static bool diag_sensor_value_cache_lookup(uint8_t addr,
                                           uint8_t instance,
                                           DiagSensorValueCacheEntry *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT ||
        instance >= DALI_INPUT_MAX_INSTANCES) {
        return false;
    }

    bool found = false;
    taskENTER_CRITICAL(&s_diag_state_mux);
    for (uint8_t i = 0u; i < DIAG_SENSOR_VALUE_CACHE_MAX; i++) {
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

static void diag_input_cache_store(const DaliDiscoveryInputDevice *input)
{
    if (input == NULL || input->device.address >= DALI_SHORT_ADDRESS_COUNT) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    uint8_t slot = DIAG_INPUT_CACHE_MAX;
    for (uint8_t i = 0u; i < DIAG_INPUT_CACHE_MAX; i++) {
        if (s_input_cache[i].valid &&
            s_input_cache[i].input.device.address == input->device.address) {
            slot = i;
            break;
        }
        if (!s_input_cache[i].valid && slot == DIAG_INPUT_CACHE_MAX) {
            slot = i;
        }
    }
    if (slot == DIAG_INPUT_CACHE_MAX) {
        slot = (uint8_t)(input->device.address % DIAG_INPUT_CACHE_MAX);
    }
    s_input_cache[slot] = (DiagInputCacheEntry){
        .valid = true,
        .input = *input,
    };
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static bool diag_input_cache_lookup(uint8_t addr, DaliDiscoveryInputDevice *out)
{
    if (out == NULL || addr >= DALI_SHORT_ADDRESS_COUNT) {
        return false;
    }

    bool found = false;

    taskENTER_CRITICAL(&s_diag_state_mux);
    for (uint8_t i = 0u; i < DIAG_INPUT_CACHE_MAX; i++) {
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

static bool diag_event_is_switch_candidate(const DaliInputEvent *event)
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
    if (!diag_input_cache_lookup(event->source.device_address, &input) ||
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

static bool diag_event_same_source(const DaliInputEvent *a, const DaliInputEvent *b)
{
    return a != NULL && b != NULL &&
           a->frame_kind == b->frame_kind &&
           a->raw.bit_length == b->raw.bit_length &&
           a->raw.data == b->raw.data;
}

static bool diag_record_switch_mapping(const DaliInputEventRecord *record)
{
    if (record == NULL) {
        return false;
    }

    bool recorded = false;

    taskENTER_CRITICAL(&s_diag_state_mux);
    for (uint8_t i = 0u; i < s_switch_mapping_count; i++) {
        if (diag_event_same_source(&s_switch_mappings[i].event, &record->event)) {
            s_switch_mappings[i].seen_count++;
            taskEXIT_CRITICAL(&s_diag_state_mux);
            return false;
        }
    }

    if (s_switch_mapping_count < DIAG_SWITCH_MAPPING_MAX) {
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

static uint8_t diag_switch_mappings_snapshot(DiagSwitchMapping *out,
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

static const char *diag_capture_kind_name(DiagCaptureKind kind)
{
    switch (kind) {
        case DIAG_CAPTURE_TX:
            return "tx";
        case DIAG_CAPTURE_RX:
            return "rx";
        case DIAG_CAPTURE_EVENT:
            return "event";
        default:
            return "unknown";
    }
}

static const char *diag_sched_state_name(DaliSchedState state)
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

static void diag_capture_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_head = 0u;
    s_capture_count = 0u;
    s_capture_dropped = 0u;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void diag_capture_set_enabled(bool enabled)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    s_capture_enabled = enabled;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void diag_capture_push(const DiagCaptureRecord *record)
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
    if (s_capture_count < DIAG_CAPTURE_MAX) {
        index = (uint8_t)((s_capture_head + s_capture_count) % DIAG_CAPTURE_MAX);
        s_capture_count++;
    } else {
        index = s_capture_head;
        s_capture_head = (uint8_t)((s_capture_head + 1u) % DIAG_CAPTURE_MAX);
        s_capture_dropped++;
    }
    s_capture[index] = *record;
    s_capture[index].valid = true;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void diag_capture_push_trace(const DaliSchedTraceEvent *event)
{
    if (event == NULL) {
        return;
    }

    DiagCaptureRecord record = {
        .valid        = true,
        .kind         = event->direction == DALI_SCHED_TRACE_TX
                      ? DIAG_CAPTURE_TX
                      : DIAG_CAPTURE_RX,
        .frame        = event->frame,
        .timestamp_us = event->timestamp_us,
        .since_tx_us  = event->since_tx_us,
        .has_since_tx = event->has_since_tx,
    };
    diag_capture_push(&record);
}

static void diag_capture_push_event(const DaliInputEventRecord *event_record)
{
    if (event_record == NULL) {
        return;
    }

    DiagCaptureRecord record = {
        .valid        = true,
        .kind         = DIAG_CAPTURE_EVENT,
        .frame        = event_record->event.raw,
        .event        = event_record->event,
        .has_event    = true,
        .timestamp_us = event_record->timestamp_us,
    };
    diag_capture_push(&record);
}

static uint8_t diag_capture_snapshot(DiagCaptureRecord *out,
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
            uint8_t index = (uint8_t)((s_capture_head + i) % DIAG_CAPTURE_MAX);
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

static void diag_print_event_json_fields(const DaliInputEvent *event)
{
    if (event == NULL) {
        return;
    }

    printf(", \"frame_kind\": \"%s\"",
           dali_event_frame_kind_name(event->frame_kind));

    if (event->frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT) {
        printf(", \"address_byte\": %u, \"address_byte_hex\": \"0x%02X\"",
               (unsigned)event->address_byte,
               (unsigned)event->address_byte);
        printf(", \"address_kind\": \"%s\"",
               dali_event_address_kind_name(event->address_kind));
        if (event->address_kind == DALI_EVENT_ADDRESS_SHORT ||
            event->address_kind == DALI_EVENT_ADDRESS_GROUP) {
            printf(", \"address\": %u", (unsigned)event->address);
        }
        printf(", \"selector\": %s",
               event->address_selector ? "true" : "false");
        printf(", \"action_code\": %u", (unsigned)event->legacy_data);
        printf(", \"action_hex\": \"0x%02X\"",
               (unsigned)event->legacy_data);
        printf(", \"action\": \"%s\"", dali_event_action_name(event));
        return;
    }

    printf(", \"source_scheme\": \"%s\"",
           dali_event_source_scheme_name(event->source.scheme));
    if (event->source.has_device_address) {
        printf(", \"device_address\": %u",
               (unsigned)event->source.device_address);
    }
    if (event->source.has_device_group) {
        printf(", \"device_group\": %u",
               (unsigned)event->source.device_group);
    }
    if (event->source.has_instance) {
        printf(", \"instance\": %u", (unsigned)event->source.instance);
    }
    if (event->source.has_instance_group) {
        printf(", \"instance_group\": %u",
               (unsigned)event->source.instance_group);
    }
    if (event->source.has_instance_type) {
        printf(", \"instance_type\": %u",
               (unsigned)event->source.instance_type);
    }

    if (event->frame_kind == DALI_EVENT_FRAME_INPUT_24BIT) {
        printf(", \"event_information\": %u",
               (unsigned)event->event_information);
        printf(", \"event_information_hex\": \"0x%03X\"",
               (unsigned)event->event_information);
    }
    printf(", \"action\": \"%s\"", dali_event_action_name(event));
}

static void diag_print_capture_json(void)
{
    uint32_t dropped = 0u;
    bool enabled = false;
    uint8_t count = diag_capture_snapshot(s_capture_export,
                                          DIAG_CAPTURE_MAX,
                                          &dropped,
                                          &enabled);

    printf("  \"capture\": {\r\n");
    printf("    \"enabled\": %s,\r\n", enabled ? "true" : "false");
    printf("    \"count\": %u,\r\n", (unsigned)count);
    printf("    \"dropped\": %" PRIu32 ",\r\n", dropped);
    printf("    \"records\": [\r\n");
    for (uint8_t i = 0u; i < count; i++) {
        const DiagCaptureRecord *record = &s_capture_export[i];
        if (i > 0u) {
            printf(",\r\n");
        }
        printf("      { \"kind\": \"%s\", \"timestamp_us\": %" PRIu32,
               diag_capture_kind_name(record->kind),
               record->timestamp_us);
        printf(", \"raw\": \"0x%0*" PRIX32 "\"",
               diag_frame_hex_width(&record->frame),
               record->frame.data);
        printf(", \"raw_bits\": %u", (unsigned)record->frame.bit_length);
        if (record->has_since_tx) {
            printf(", \"since_tx_us\": %" PRIu32, record->since_tx_us);
        }
        if (record->has_event) {
            diag_print_event_json_fields(&record->event);
        }
        printf(" }");
    }
    printf("\r\n    ]\r\n");
    printf("  }");
}

static void diag_print_event_record(const char *prefix,
                                    const DaliInputEventRecord *record)
{
    if (record == NULL) {
        return;
    }

    const DaliInputEvent *event = &record->event;
    printf("%sraw=0x%0*" PRIX32 " frame=%s",
           prefix,
           diag_frame_hex_width(&event->raw),
           event->raw.data,
           dali_event_frame_kind_name(event->frame_kind));

    if (event->frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT) {
        printf(" addr_byte=0x%02X kind=%s",
               (unsigned)event->address_byte,
               dali_event_address_kind_name(event->address_kind));
        if (event->address_kind == DALI_EVENT_ADDRESS_SHORT ||
            event->address_kind == DALI_EVENT_ADDRESS_GROUP) {
            printf(" addr=%u", (unsigned)event->address);
        }
        printf(" selector=%u", event->address_selector ? 1u : 0u);
        printf(" action=0x%02X %s time=%" PRIu32 "us\r\n",
               (unsigned)event->legacy_data,
               dali_event_action_name(event),
               record->timestamp_us);
        return;
    }

    printf(" source=%s",
           dali_event_source_scheme_name(event->source.scheme));
    if (event->source.has_device_address) {
        printf(" device_address=%u", (unsigned)event->source.device_address);
    }
    if (event->source.has_device_group) {
        printf(" device_group=%u", (unsigned)event->source.device_group);
    }
    if (event->source.has_instance) {
        printf(" instance=%u", (unsigned)event->source.instance);
    }
    if (event->source.has_instance_group) {
        printf(" instance_group=%u", (unsigned)event->source.instance_group);
    }
    if (event->source.has_instance_type) {
        printf(" instance_type=%u", (unsigned)event->source.instance_type);
    }

    if (event->frame_kind == DALI_EVENT_FRAME_INPUT_24BIT) {
        printf(" event_information=0x%03X %s",
               (unsigned)event->event_information,
               dali_event_action_name(event));
    } else {
        printf(" %s", dali_event_action_name(event));
    }
    printf(" time=%" PRIu32 "us\r\n", record->timestamp_us);
}

static void diag_event_cb(const DaliFrame *frame, void *cb_ctx)
{
    (void)cb_ctx;

    DaliInputEventRecord record = {
        .timestamp_us = (uint32_t)esp_timer_get_time(),
    };
    if (dali_event_parse_frame(frame, &record.event) != DALI_OK) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    (void)dali_event_queue_push(&s_event_queue, &record);
    taskEXIT_CRITICAL(&s_diag_state_mux);

    diag_capture_push_event(&record);
}

static void diag_trace_cb(const DaliSchedTraceEvent *event, void *cb_ctx)
{
    (void)cb_ctx;

    diag_store_last_rx(event);
    diag_capture_push_trace(event);

    if (!s_trace_enabled || event == NULL) {
        return;
    }

    const DaliFrame *frame = &event->frame;
    unsigned bits = (unsigned)frame->bit_length;

    if (event->direction == DALI_SCHED_TRACE_TX) {
        printf("[BUS] TX 0x%0*" PRIX32 " (%u-bit)\r\n",
               diag_frame_hex_width(frame),
               frame->data,
               bits);
    } else if (event->has_since_tx) {
        uint32_t tenths_ms = (event->since_tx_us + 50u) / 100u;
        printf("[BUS] RX 0x%0*" PRIX32 " (%u-bit, %" PRIu32 ".%" PRIu32 " ms after TX)\r\n",
               diag_frame_hex_width(frame),
               frame->data,
               bits,
               tenths_ms / 10u,
               tenths_ms % 10u);
    } else {
        printf("[BUS] RX 0x%0*" PRIX32 " (%u-bit)\r\n",
               diag_frame_hex_width(frame),
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
        diag_capture_snapshot(NULL, 0u, &capture_dropped, &capture_enabled);

    printf("RX overflow:      %" PRIu32 "\r\n", g_dali_stats.rx_overflow);
    printf("TX retries:       %" PRIu32 "\r\n", g_dali_stats.tx_retries);
    printf("Malformed frames: %" PRIu32 "\r\n", g_dali_stats.malformed_frames);
    printf("Reply timeouts:   %" PRIu32 "\r\n", g_dali_stats.reply_timeouts);
    printf("RX ignored:       %" PRIu32 "\r\n", g_dali_stats.rx_ignored_outside_reply);
    printf("RX events routed: %" PRIu32 "\r\n", g_dali_stats.unsolicited_events_routed);
    printf("Raw malformed:    %" PRIu32 "\r\n", g_dali_stats.raw_malformed);
    printf("ISR overruns:     %" PRIu32 "\r\n", g_dali_stats.isr_overruns);
    printf("Bus idle fails:   %" PRIu32 "\r\n", g_dali_stats.bus_idle_failures);
    printf("RX TX echo drop:  %" PRIu32 "\r\n", g_dali_stats.rx_self_echo_suppressed);
    printf("RX settle drop:   %" PRIu32 "\r\n", g_dali_stats.rx_settle_suppressed);
    printf("RX glitch drop:   %" PRIu32 "\r\n", g_dali_stats.rx_glitch_drops);
    printf("Event queued:     %u\r\n", (unsigned)diag_event_queue_count_snapshot());
    printf("Event dropped:    %" PRIu32 "\r\n", diag_event_queue_dropped_snapshot());
    printf("Capture:          %s, %u queued, %" PRIu32 " dropped\r\n",
           capture_enabled ? "on" : "off",
           (unsigned)capture_count,
           capture_dropped);

    DaliSchedQueueStats q;
    if (dali_sched_queue_stats(&q) == DALI_OK) {
        printf("Queue depth:      %u/%u (high-water %u)\r\n",
               (unsigned)q.depth, (unsigned)q.capacity, (unsigned)q.high_water);
        printf("Queue admitted:   %" PRIu32 "\r\n", q.admitted);
        printf("Queue dropped:    %" PRIu32 " full, %" PRIu32 " busy\r\n",
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
    printf("depth %u/%u  high-water %u  admitted %" PRIu32
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
        printf("trace on\r\n");
    } else if (strcmp(t->tok[1], "off") == 0) {
        s_trace_enabled = false;
        printf("trace off\r\n");
    } else {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_TRACE));
    }
}

static void cmd_reset(void)
{
#ifndef DALI_HOST_BUILD
    DaliError err = diag_reset_sync();
    if (err != DALI_OK) {
        printf("reset: ERR %d\r\n", (int)err);
        return;
    }

    diag_last_rx_reset();
    diag_inventory_reset();
    diag_events_reset();
    diag_switch_mappings_reset();
    diag_input_cache_reset();
    diag_sensor_value_cache_reset();
    diag_capture_reset();
    printf("reset OK\r\n");
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
        printf("read: no RX frame captured\r\n");
        return;
    }

    diag_print_frame("read: ", &frame);
    printf("  timestamp: %" PRIu32 " us\r\n", timestamp_us);
    if (has_since_tx) {
        uint32_t tenths_ms = (since_tx_us + 50u) / 100u;
        printf("  after TX:  %" PRIu32 ".%" PRIu32 " ms\r\n",
               tenths_ms / 10u,
               tenths_ms % 10u);
    }
#endif
}

static char diag_rxdebug_bucket(uint32_t interval_us)
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
        printf("rxdebug: ERR %d\r\n", (int)err);
        return;
    }
    if (!snapshot.valid) {
        printf("rxdebug: no malformed RX snapshot\r\n");
        return;
    }

    printf("rxdebug: err=%d, intervals=%u, edges=%u\r\n",
           (int)snapshot.error,
           (unsigned)snapshot.interval_count,
           (unsigned)snapshot.edge_count);

    printf("  edge levels:");
    for (uint8_t i = 0u; i < snapshot.edge_count; i++) {
        printf(" %u", (unsigned)snapshot.edge_levels[i]);
    }
    printf("\r\n");

    printf("  intervals us (H=half, F=full):");
    for (uint8_t i = 0u; i < snapshot.interval_count; i++) {
        if ((i % 8u) == 0u) {
            printf("\r\n    ");
        }
        printf("%" PRIu32 "%c ", snapshot.intervals_us[i],
               diag_rxdebug_bucket(snapshot.intervals_us[i]));
    }
    printf("\r\n");
#endif
}

static DaliError diag_send_no_reply(const DaliFrame *frame, bool send_twice)
{
    return diag_sched_sync(frame, false, 0u, send_twice, NULL);
}

static void diag_print_tx_result(const char *name, DaliError err)
{
    dali_cli_print_tx_result(&s_out, name, err);
}

/* A target that is not a single short address may be answered by several
 * devices at once, or acted on by several at once. Say so before the traffic. */
static void diag_warn_multi_target(const char *name, DaliTarget target, bool is_query)
{
    if (target.type == DALI_ADDR_SHORT) {
        return;
    }
    if (is_query) {
        printf("%s: group/broadcast replies may collide on a real bus\r\n", name);
    } else {
        printf("%s: group/broadcast target may affect multiple devices\r\n", name);
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
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result(level.is_mask ? "mask" : "level", err);
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
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result("mask", err);
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
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result(spec->name, err);
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
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result("scene", err);
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
    DaliError err = diag_sched_sync(&frame, wait_reply, 0u, send_twice,
                                    wait_reply ? &reply : NULL);
    if (wait_reply) {
        if (err == DALI_OK) {
            dali_cli_print_frame(&s_out, "RX: ", &reply);
        } else if (err == DALI_ERR_TIMEOUT) {
            printf("RX: timeout\r\n");
        } else {
            printf("TX/RX: ERR %d\r\n", (int)err);
        }
        return;
    }

    diag_print_tx_result("TX", err);
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
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result("dtr", err);
}

static uint8_t diag_command_reply_retries_left(DaliCommandId id)
{
    return dali_command_response_retry_safe(id) ? 1u : 0u;
}

static DaliError diag_query_status(DaliTarget target, DaliFrame *reply)
{
    DaliFrame frame;
    DaliError err = dali_control_build_query_status(target, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return diag_sched_sync(
        &frame,
        true,
        diag_command_reply_retries_left(DALI_CMD_QUERY_STATUS),
        false,
        reply);
}

static DaliError diag_query_u8(DaliTarget target,
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

    err = diag_sched_sync(&frame,
                          true,
                          diag_command_reply_retries_left(id),
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
    diag_warn_multi_target("status", target, true);

    DaliError err = diag_query_status(target, &reply);
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
        printf("query: unknown query '%s'\r\n", t->tok[2]);
        printf("use 'list query'\r\n");
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (t->count != 4u ||
            !dali_cli_parse_u8(t->tok[3], spec->max_param, &param)) {
            printf("usage: query " DALI_CLI_TARGET_ARG " %s <0-%u>\r\n",
                   spec->name, (unsigned)spec->max_param);
            return;
        }
    } else if (t->count != 3u) {
        printf("usage: query " DALI_CLI_TARGET_ARG " %s\r\n", spec->name);
        return;
    }

    diag_warn_multi_target("query", target, true);

    const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
    DaliFrame frame;
    DaliFrame reply = {0u, 0u};
    DaliError err = dali_control_build_query(target, spec->id, param, &frame);
    if (err == DALI_OK) {
        err = diag_sched_sync(&frame,
                              true,
                              diag_command_reply_retries_left(spec->id),
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
        printf("special: unknown command '%s'\r\n", t->tok[1]);
        printf("use 'list special'\r\n");
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (t->count != 3u ||
            !dali_cli_parse_u8(t->tok[2], spec->max_param, &param)) {
            printf("usage: special %s <0-%u>\r\n",
                   spec->name, (unsigned)spec->max_param);
            return;
        }
    } else if (t->count != 2u) {
        printf("usage: special %s\r\n", spec->name);
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
        err = diag_sched_sync(&frame,
                              needs_reply,
                              needs_reply
                                  ? diag_command_reply_retries_left(spec->id)
                                  : 0u,
                              cmd->send_twice,
                              needs_reply ? &reply : NULL);
    }

    if (err == DALI_OK && needs_reply) {
        dali_cli_print_response(&s_out, spec->name, cmd->response_kind, &reply);
    } else if (err == DALI_OK) {
        printf("%s: OK\r\n", spec->name);
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
        printf("config: unknown config '%s'\r\n", t->tok[2]);
        printf("use 'list config'\r\n");
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (t->count != 4u ||
            !dali_cli_parse_u8(t->tok[3], spec->max_param, &param)) {
            printf("usage: config " DALI_CLI_TARGET_ARG " %s <0-%u>\r\n",
                   spec->name, (unsigned)spec->max_param);
            return;
        }
    } else if (t->count != 3u) {
        printf("usage: config " DALI_CLI_TARGET_ARG " %s\r\n", spec->name);
        return;
    }

    diag_warn_multi_target("config", target, false);
    if (spec->uses_dtr0) {
        printf("config: using current DTR0 value; use config-dtr0 to set it atomically\r\n");
    }

    DaliFrame frame;
    DaliError err = dali_control_build_config(target, spec->id, param, &frame);
    const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
    if (err == DALI_OK && cmd != NULL) {
        err = diag_send_no_reply(&frame, cmd->send_twice);
    }
    diag_print_tx_result(spec->name, err);
}

/*
 * Report a sequence outcome, naming the step that ended it. Which step failed
 * is the difference between "nothing happened" and "the DTR was written but the
 * command that consumes it was not", so it is never folded into a bare error.
 */
static void diag_print_sequence_result(const char *name,
                                       DaliError err,
                                       const DaliSequenceResult *result)
{
    if (err == DALI_OK) {
        printf("%s: OK\r\n", name);
        return;
    }
    if (result != NULL && result->failed_step != DALI_SEQUENCE_NO_FAILED_STEP) {
        printf("%s: ERR %d at sequence step %u\r\n",
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
        printf("config-dtr0: unknown config '%s'\r\n", t->tok[2]);
        printf("use 'list config'\r\n");
        return;
    }
    if (!spec->uses_dtr0 || !dali_control_config_uses_dtr0(spec->id)) {
        printf("config-dtr0: '%s' does not consume DTR0\r\n", spec->name);
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
            printf("usage: config-dtr0 " DALI_CLI_TARGET_ARG " %s <0-255> <0-%u>\r\n",
                   spec->name, (unsigned)spec->max_param);
            return;
        }
    } else if (t->count != 4u) {
        printf("usage: config-dtr0 " DALI_CLI_TARGET_ARG " %s <0-255>\r\n",
               spec->name);
        return;
    }

    diag_warn_multi_target("config-dtr0", target, false);

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
        diag_print_tx_result(spec->name, err);
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
    err = diag_sched_sequence_sync(&seq, &seq_result);
    diag_print_sequence_result(spec->name, err, &seq_result);
}

static void diag_print_input_instance(const DaliInputInstanceInfo *info)
{
    if (info == NULL) {
        return;
    }

    printf("  %2u: type=%u %s, usable=%s, source=%s",
           (unsigned)info->instance,
           (unsigned)info->type,
           dali_input_role_name(info->role),
           dali_input_usable_name(info->usable),
           dali_input_role_source_name(info->role_source));

    if (info->has_enabled) {
        printf(", enabled=%s", info->enabled ? "yes" : "no");
    }
    if (info->has_resolution) {
        printf(", resolution=%u", (unsigned)info->resolution);
    }
    if (info->has_status) {
        printf(", status=0x%02X", (unsigned)info->status);
    }
    if (info->has_error) {
        printf(", error=%s", dali_is_yes(info->error) ? "yes" : "no");
    }
    printf("\r\n");
}

static void cmd_instances(const DaliCliTokens *t)
{
    uint8_t addr;

    if (!dali_cli_parse_short_addr(t->tok[1], &addr)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_INSTANCES));
        return;
    }

    DaliDiscoveryTransport transport = diag_discovery_transport();
    DaliDiscoveryInputDevice input;
    DaliError err = dali_discovery_query_input_device(&transport, addr, &input);
    if (err == DALI_ERR_TIMEOUT) {
        printf("instances: timeout querying device %u\r\n", (unsigned)addr);
        return;
    }
    if (err != DALI_OK) {
        printf("instances: ERR %d\r\n", (int)err);
        return;
    }
    diag_input_cache_store(&input);

    DaliDiscoveryInventory inventory;
    if (diag_inventory_snapshot(&inventory)) {
        if (dali_discovery_inventory_update_input_device(&inventory, &input) == DALI_OK) {
            diag_inventory_replace(&inventory);
        }
    }

    uint8_t count = dali_discovery_input_visible_instance_count(&input);
    printf("Input device %u:\r\n", (unsigned)addr);
    if (input.device.instance_count > DALI_INPUT_MAX_INSTANCES) {
        printf("  instances: %u (showing first %u)\r\n",
               (unsigned)input.device.instance_count,
               (unsigned)DALI_INPUT_MAX_INSTANCES);
    } else {
        printf("  instances: %u\r\n", (unsigned)input.device.instance_count);
    }

    for (uint8_t instance = 0u; instance < count; instance++) {
        DaliError type_err = input.instance_type_errors[instance];
        const DaliInputInstanceInfo *info = &input.device.instances[instance];
        if (type_err == DALI_ERR_TIMEOUT) {
            printf("  %2u: type timeout\r\n", (unsigned)instance);
            continue;
        }
        if (type_err != DALI_OK) {
            printf("  %2u: type ERR %d\r\n", (unsigned)instance, (int)type_err);
            continue;
        }

        diag_print_input_instance(info);
    }
}

static void diag_print_poll_value(const DaliInputPollResult *poll)
{
    if (poll == NULL) {
        return;
    }

    int width = (int)(poll->value.byte_count * 2u);
    if (width <= 0) {
        width = 2;
    }

    printf(" raw=0x%0*" PRIX32 " bytes=%u",
           width,
           poll->value.value,
           (unsigned)poll->value.byte_count);
}

static void diag_sensor_poll_instance(uint8_t addr,
                                      const DaliInputInstanceInfo *info)
{
    if (info == NULL) {
        return;
    }

    uint8_t expected_bytes = info->has_resolution
                           ? dali_input_poll_bytes_for_resolution(info->resolution)
                           : 1u;
    DaliDiscoveryTransport transport = diag_discovery_transport();
    DaliInputPollResult poll;
    DaliError err = dali_input_poll_value(&transport,
                                          addr,
                                          info->instance,
                                          expected_bytes,
                                          &poll);
    diag_sensor_value_cache_store(addr, info, expected_bytes, err, &poll);

    printf("  %2u: type=%u %s",
           (unsigned)info->instance,
           (unsigned)info->type,
           dali_input_role_name(info->role));

    if (info->has_enabled) {
        printf(", enabled=%s", info->enabled ? "yes" : "no");
    }
    if (info->has_resolution) {
        printf(", resolution=%u", (unsigned)info->resolution);
    } else {
        printf(", resolution=unknown");
    }

    if (err == DALI_OK) {
        diag_print_poll_value(&poll);
        printf("\r\n");
    } else if (err == DALI_ERR_TIMEOUT) {
        printf(" poll=timeout\r\n");
    } else {
        printf(" poll=ERR %d\r\n", (int)err);
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

    DaliDiscoveryTransport transport = diag_discovery_transport();
    DaliDiscoveryInputDevice input;
    DaliError err = dali_discovery_query_input_device(&transport, addr, &input);
    if (err == DALI_ERR_TIMEOUT) {
        printf("sensor poll: timeout querying device %u\r\n", (unsigned)addr);
        return;
    }
    if (err != DALI_OK) {
        printf("sensor poll: ERR %d\r\n", (int)err);
        return;
    }
    diag_input_cache_store(&input);

    DaliDiscoveryInventory inventory;
    if (diag_inventory_snapshot(&inventory)) {
        if (dali_discovery_inventory_update_input_device(&inventory, &input) == DALI_OK) {
            diag_inventory_replace(&inventory);
        }
    }

    uint8_t count = dali_discovery_input_visible_instance_count(&input);
    printf("Sensor poll %u:\r\n", (unsigned)addr);

    if (has_selected_instance) {
        if (selected_instance >= count) {
            printf("  instance %u not visible; discovered count=%u\r\n",
                   (unsigned)selected_instance,
                   (unsigned)input.device.instance_count);
            return;
        }
        diag_sensor_poll_instance(addr, &input.device.instances[selected_instance]);
        return;
    }

    for (uint8_t instance = 0u; instance < count; instance++) {
        DaliError type_err = input.instance_type_errors[instance];
        if (type_err == DALI_ERR_TIMEOUT) {
            printf("  %2u: type timeout\r\n", (unsigned)instance);
            continue;
        }
        if (type_err != DALI_OK) {
            printf("  %2u: type ERR %d\r\n", (unsigned)instance, (int)type_err);
            continue;
        }
        diag_sensor_poll_instance(addr, &input.device.instances[instance]);
    }
}

static void cmd_events(void)
{
#ifndef DALI_HOST_BUILD
    DaliInputEventRecord record;
    uint8_t count = 0u;

    while (diag_event_pop(&record)) {
        diag_print_event_record("event: ", &record);
        count++;
    }

    if (count == 0u) {
        printf("events: none queued\r\n");
    }
    printf("events: drained=%u dropped=%" PRIu32 "\r\n",
           (unsigned)count,
           diag_event_queue_dropped_snapshot());
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
        diag_capture_set_enabled(true);
        printf("capture: started\r\n");
    } else if (strcmp(action, "stop") == 0) {
        diag_capture_set_enabled(false);
        printf("capture: stopped\r\n");
    } else if (strcmp(action, "clear") == 0) {
        diag_capture_reset();
        printf("capture: cleared\r\n");
    } else if (strcmp(action, "status") == 0) {
        uint32_t dropped = 0u;
        bool enabled = false;
        uint8_t count = diag_capture_snapshot(NULL, 0u, &dropped, &enabled);
        printf("capture: %s, count=%u, dropped=%" PRIu32 "\r\n",
               enabled ? "on" : "off",
               (unsigned)count,
               dropped);
    } else if (strcmp(action, "export") == 0) {
        printf("{\r\n");
        diag_print_capture_json();
        printf("\r\n}\r\n");
    } else {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_CAPTURE));
    }
}

static void cmd_find(const DaliCliTokens *t)
{
    uint32_t seconds = DIAG_FIND_SWITCH_DEFAULT_SECONDS;

    if (!dali_cli_has_subcommand(dali_cli_command_for_id(DALI_CLI_CMD_FIND),
                                 t->tok[1]) ||
        (t->count == 3u &&
         !dali_cli_parse_u32(t->tok[2], DIAG_FIND_SWITCH_MAX_SECONDS, &seconds)) ||
        seconds == 0u) {
        printf("usage: find switches [1-%u seconds]\r\n",
               (unsigned)DIAG_FIND_SWITCH_MAX_SECONDS);
        return;
    }

    diag_events_reset();
    diag_switch_mappings_reset();

    printf("find switches: listening for %u seconds; double-press DALI-2 switches or trigger legacy coupler actions.\r\n",
           (unsigned)seconds);
    printf("Run 'discover' first so Device/Instance switch types can be resolved safely.\r\n");

    TickType_t start = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(seconds * 1000u);
    DaliInputEventRecord record;

    while ((xTaskGetTickCount() - start) < duration) {
        while (diag_event_pop(&record)) {
            diag_print_event_record("event: ", &record);
            if (diag_event_is_switch_candidate(&record.event)) {
                bool recorded = diag_record_switch_mapping(&record);
                if (recorded) {
                    uint8_t order = diag_switch_mappings_snapshot(NULL, 0u);
                    printf("  mapped switch %u\r\n", (unsigned)order);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50u));
    }

    while (diag_event_pop(&record)) {
        diag_print_event_record("event: ", &record);
        if (diag_event_is_switch_candidate(&record.event)) {
            bool recorded = diag_record_switch_mapping(&record);
            if (recorded) {
                uint8_t order = diag_switch_mappings_snapshot(NULL, 0u);
                printf("  mapped switch %u\r\n", (unsigned)order);
            }
        }
    }

    uint8_t mapped = diag_switch_mappings_snapshot(NULL, 0u);
    printf("find switches: mapped=%u dropped=%" PRIu32 "\r\n",
           (unsigned)mapped,
           diag_event_queue_dropped_snapshot());
}

typedef struct {
    bool detailed;
} DiagDiscoveryPrintCtx;

static void diag_discovery_found_cb(uint8_t addr,
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
            printf("%02u: present, %s, status=0x%02X",
                   (unsigned)addr, kind, (unsigned)device->status);
            if (device->has_version) {
                printf(", v%u", (unsigned)(device->version / 2u));
            }
            if (device->has_actual_level) {
                printf(", level=%u", (unsigned)device->actual_level);
            }
            if (device->has_groups && device->groups != 0u) {
                printf(", groups=[");
                bool first = true;
                for (uint8_t g = 0u; g < 16u; g++) {
                    if (device->groups & (1u << g)) {
                        if (!first) {
                            printf(",");
                        }
                        printf("%u", (unsigned)g);
                        first = false;
                    }
                }
                printf("]");
            }
            if (device->has_input_device) {
                printf(", input-device(%u instances)", (unsigned)device->instance_count);
            }
            printf("\r\n");
        } else {
            printf("%02u: input-device, %u instance(s)\r\n",
                   (unsigned)addr,
                   (unsigned)(device->has_instance_count ? device->instance_count : 0u));
        }
    } else {
        if (device->has_status) {
            printf("Device %2u: present (status=0x%02X)\r\n",
                   (unsigned)addr,
                   (unsigned)device->status);
        } else {
            printf("Device %2u: input-device (%u instance(s))\r\n",
                   (unsigned)addr,
                   (unsigned)(device->has_instance_count ? device->instance_count : 0u));
        }
    }
}

static uint8_t diag_discover_bus(bool detailed)
{
    uint8_t found = 0u;
    DaliDiscoveryInventory inventory;
    DaliDiscoveryTransport transport = diag_discovery_transport();
    DiagDiscoveryPrintCtx print_ctx = {
        .detailed = detailed,
    };

    printf("Scanning short addresses 0-%u...\r\n", (unsigned)DALI_MAX_SHORT_ADDRESS);
    DaliError err = dali_discovery_scan(&inventory,
                                        &transport,
                                        diag_discovery_found_cb,
                                        &print_ctx,
                                        &found);
    if (err != DALI_OK) {
        diag_inventory_reset();
        printf("Scan ERR %d\r\n", (int)err);
        return 0u;
    }

    /* Enumerate instances for any detected input devices. */
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *entry =
            dali_discovery_inventory_get(&inventory, addr);
        if (entry == NULL || !entry->present || !entry->has_input_device) {
            continue;
        }
        DaliDiscoveryInputDevice input;
        if (dali_discovery_query_input_device(&transport, addr, &input) == DALI_OK) {
            (void)dali_discovery_inventory_update_input_device(&inventory, &input);
            diag_input_cache_store(&input);
            if (detailed) {
                uint8_t count = dali_discovery_input_visible_instance_count(&input);
                printf("  %02u: %u input instance(s) enumerated\r\n",
                       (unsigned)addr, (unsigned)count);
            }
        }
    }

    diag_inventory_replace(&inventory);
    printf("Scan complete: %u device(s) found.\r\n", (unsigned)found);
    return found;
}

static void cmd_scan(void)
{
#ifndef DALI_HOST_BUILD
    (void)diag_discover_bus(false);
#endif
}

static void cmd_discover(void)
{
#ifndef DALI_HOST_BUILD
    (void)diag_discover_bus(true);
#endif
}

static void cmd_inventory(void)
{
#ifndef DALI_HOST_BUILD
    uint8_t found = 0u;
    static DaliDiscoveryInventory inventory;

    if (!diag_inventory_snapshot(&inventory)) {
        printf("inventory: empty; run discover first\r\n");
        return;
    }
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *entry =
            dali_discovery_inventory_get(&inventory, addr);
        if (entry != NULL && entry->present &&
            (entry->has_status || entry->has_input_device)) {
            if (entry->has_status) {
                const char *kind = entry->has_device_type
                    ? dali_discovery_device_type_name(entry->device_type)
                    : "unknown-type";
                printf("%02u: %s, status=0x%02X", (unsigned)addr, kind, (unsigned)entry->status);
            } else {
                printf("%02u: input-device", (unsigned)addr);
            }
            if (entry->has_version) {
                printf(", v%u", (unsigned)(entry->version / 2u));
            }
            if (entry->has_actual_level) {
                printf(", level=%u", (unsigned)entry->actual_level);
            }
            if (entry->has_groups) {
                printf(", groups=[");
                bool first_group = true;
                for (uint8_t g = 0u; g < 16u; g++) {
                    if (entry->groups & (1u << g)) {
                        if (!first_group) {
                            printf(",");
                        }
                        printf("%u", (unsigned)g);
                        first_group = false;
                    }
                }
                printf("]");
            }
            if (entry->has_input_device) {
                printf(", input-device(%u instances)", (unsigned)entry->instance_count);
            }
            printf("\r\n");
            found++;
        }
    }

    printf("Inventory: %u device(s)\r\n", (unsigned)found);
#endif
}

static void diag_commission_progress_cb(const DaliCommissioningEvent *event,
                                        void *ctx)
{
#ifndef DALI_HOST_BUILD
    (void)ctx;
    if (event == NULL) {
        return;
    }

    switch (event->kind) {
        case DALI_COMMISSIONING_EVENT_INITIALISED:
            printf("commission: initialise unaddressed\r\n");
            break;

        case DALI_COMMISSIONING_EVENT_RANDOMISED:
            printf("commission: randomize\r\n");
            break;

        case DALI_COMMISSIONING_EVENT_SEARCH_FOUND:
            printf("commission: found random=0x%06" PRIX32
                   " -> short %u\r\n",
                   event->random_address,
                   (unsigned)event->short_address);
            break;

        case DALI_COMMISSIONING_EVENT_ASSIGNED:
            printf("commission: assigned short %u"
                   " (count=%u)\r\n",
                   (unsigned)event->short_address,
                   (unsigned)event->assigned_count);
            break;

        case DALI_COMMISSIONING_EVENT_NO_MORE_DEVICES:
            printf("commission: no more unaddressed devices\r\n");
            break;

        case DALI_COMMISSIONING_EVENT_ADDRESS_SPACE_FULL:
            printf("commission: no free short addresses\r\n");
            break;

        case DALI_COMMISSIONING_EVENT_TERMINATED:
            printf("commission: terminate\r\n");
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
        printf("usage: commission unaddressed [0-%u] [0-%u]\r\n",
               (unsigned)DALI_MAX_SHORT_ADDRESS,
               (unsigned)DALI_SHORT_ADDRESS_COUNT);
        return;
    }

    DaliDiscoveryTransport transport = diag_discovery_transport();
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;

    printf("commission: pre-scan occupied short addresses\r\n");
    DaliError err = dali_discovery_scan(&inventory,
                                        &transport,
                                        NULL,
                                        NULL,
                                        &found);
    if (err != DALI_OK) {
        diag_inventory_reset();
        printf("commission: pre-scan ERR %d\r\n", (int)err);
        return;
    }
    diag_inventory_replace(&inventory);
    printf("commission: occupied=%u\r\n", (unsigned)found);

    DaliCommissioningOptions options = {
        .first_short_address = first_address,
        .max_devices = max_devices,
        .used_address_mask =
            dali_commissioning_used_mask_from_inventory(&inventory),
        .query_short_address = true,
    };
    DaliCommissioningResult result;
    err = dali_commissioning_commission_unaddressed(
        &transport,
        &options,
        &result,
        diag_commission_progress_cb,
        NULL);

    if (err != DALI_OK) {
        printf("commission: ERR %d after %u assignment(s)\r\n",
               (int)err,
               (unsigned)result.assigned_count);
        return;
    }

    printf("commission: complete assigned=%u",
           (unsigned)result.assigned_count);
    if (result.no_more_devices) {
        printf(" no-more-devices=yes");
    }
    if (result.address_space_full) {
        printf(" address-space-full=yes");
    }
    printf("\r\n");

    for (uint8_t i = 0u; i < result.assigned_count; i++) {
        const DaliCommissioningAssignment *assignment =
            &result.assignments[i];
        printf("  short %u <= random 0x%06" PRIX32,
               (unsigned)assignment->short_address,
               assignment->random_address);
        if (assignment->has_query_short) {
            printf(" query=0x%02X",
                   (unsigned)assignment->query_short_raw);
        }
        printf("\r\n");
    }

    if (result.assigned_count > 0u) {
        printf("commission: verifying with post-scan\r\n");
        found = 0u;
        err = dali_discovery_scan(&inventory,
                                  &transport,
                                  NULL,
                                  NULL,
                                  &found);
        if (err != DALI_OK) {
            diag_inventory_reset();
            printf("commission: post-scan ERR %d\r\n", (int)err);
            return;
        }
        diag_inventory_replace(&inventory);
        printf("commission: post-scan found=%u\r\n", (unsigned)found);
    }
}

static void cmd_export(const DaliCliTokens *t)
{
    static DaliDiscoveryInventory inventory;
    bool has_inventory;
    static DiagSwitchMapping mappings[DIAG_SWITCH_MAPPING_MAX];
    uint8_t mapping_count;

    if (!dali_cli_has_subcommand(dali_cli_command_for_id(DALI_CLI_CMD_EXPORT),
                                 t->tok[1])) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_EXPORT));
        return;
    }

    has_inventory = diag_inventory_snapshot(&inventory);
    mapping_count = diag_switch_mappings_snapshot(mappings, DIAG_SWITCH_MAPPING_MAX);

    printf("{\r\n");
    printf("  \"schema_version\": 1,\r\n");
    printf("  \"devices\": [\r\n");
    bool first_device = true;
    if (has_inventory) {
        for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
            const DaliDiscoveryDeviceInfo *entry =
                dali_discovery_inventory_get(&inventory, addr);
            if (entry == NULL || !entry->present) {
                continue;
            }

            if (!first_device) {
                printf(",\r\n");
            }
            first_device = false;

            DaliDiscoveryInputDevice cached_input;
            bool has_input_cache = diag_input_cache_lookup(addr, &cached_input);

            printf("    { \"address\": %u, \"present\": true",
                   (unsigned)addr);
            if (entry->has_status) {
                printf(", \"status\": %u, \"status_hex\": \"0x%02X\"",
                       (unsigned)entry->status,
                       (unsigned)entry->status);
            }
            if (entry->has_device_type) {
                printf(", \"device_type\": %u, \"device_type_name\": \"%s\"",
                       (unsigned)entry->device_type,
                       dali_discovery_device_type_name(entry->device_type));
            }
            if (entry->has_version) {
                printf(", \"version\": %u", (unsigned)entry->version);
            }
            if (entry->has_actual_level) {
                printf(", \"actual_level\": %u", (unsigned)entry->actual_level);
            }
            if (entry->has_groups) {
                printf(", \"groups\": [");
                bool first_group = true;
                for (uint8_t g = 0u; g < 16u; g++) {
                    if (entry->groups & (1u << g)) {
                        if (!first_group) {
                            printf(", ");
                        }
                        printf("%u", (unsigned)g);
                        first_group = false;
                    }
                }
                printf("]");
            }
            if (entry->has_control_gear && entry->has_input_device) {
                printf(", \"kind\": \"hybrid\"");
                if (entry->has_instance_count) {
                    printf(", \"instance_count\": %u",
                           (unsigned)entry->instance_count);
                }
            } else if (entry->has_input_device) {
                printf(", \"kind\": \"input_device\"");
                if (entry->has_instance_count) {
                    printf(", \"instance_count\": %u",
                           (unsigned)entry->instance_count);
                }
            } else if (entry->has_control_gear) {
                printf(", \"kind\": \"control_gear\"");
            } else {
                printf(", \"kind\": \"unknown\"");
            }

            if (has_input_cache) {
                uint8_t count =
                    dali_discovery_input_visible_instance_count(&cached_input);
                printf(", \"instances\": [");
                for (uint8_t instance = 0u; instance < count; instance++) {
                    const DaliInputInstanceInfo *info =
                        &cached_input.device.instances[instance];
                    DaliError type_err = cached_input.instance_type_errors[instance];

                    if (instance > 0u) {
                        printf(", ");
                    }

                    printf("{ \"instance\": %u", (unsigned)instance);
                    if (type_err == DALI_OK && info->has_type) {
                        printf(", \"type\": %u", (unsigned)info->type);
                        printf(", \"type_name\": \"%s\"",
                               dali_input_type_name(info->type));
                        printf(", \"role\": \"%s\"",
                               dali_input_role_name(info->role));
                        printf(", \"usable\": \"%s\"",
                               dali_input_usable_name(info->usable));
                        printf(", \"source\": \"%s\"",
                               dali_input_role_source_name(info->role_source));
                        if (info->has_enabled) {
                            printf(", \"enabled\": %s",
                                   info->enabled ? "true" : "false");
                        }
                        if (info->has_resolution) {
                            printf(", \"resolution\": %u",
                                   (unsigned)info->resolution);
                        }
                        if (info->has_status) {
                            printf(", \"status\": %u, \"status_hex\": \"0x%02X\"",
                                   (unsigned)info->status,
                                   (unsigned)info->status);
                        }
                        if (info->has_error) {
                            printf(", \"error\": %s",
                                   dali_is_yes(info->error) ? "true" : "false");
                        }
                    } else {
                        printf(", \"query_error\": %d", (int)type_err);
                    }

                    DiagSensorValueCacheEntry cached_value;
                    if (diag_sensor_value_cache_lookup(addr,
                                                       instance,
                                                       &cached_value)) {
                        int value_width = (int)(cached_value.byte_count * 2u);
                        if (value_width <= 0) {
                            value_width = 2;
                        }
                        printf(", \"latest_value\": {");
                        printf(" \"result\": %d", (int)cached_value.result);
                        printf(", \"timestamp_us\": %" PRIu32,
                               cached_value.timestamp_us);
                        printf(", \"expected_bytes\": %u",
                               (unsigned)cached_value.expected_bytes);
                        printf(", \"byte_count\": %u",
                               (unsigned)cached_value.byte_count);
                        printf(", \"complete\": %s",
                               cached_value.complete ? "true" : "false");
                        printf(", \"raw\": \"0x%0*" PRIX32 "\"",
                               value_width,
                               cached_value.value);
                        printf(", \"raw_value\": %" PRIu32,
                               cached_value.value);
                        if (cached_value.has_resolution) {
                            printf(", \"resolution\": %u",
                                   (unsigned)cached_value.resolution);
                        }
                        printf(", \"byte_errors\": [%d, %d, %d, %d]",
                               (int)cached_value.byte_errors[0],
                               (int)cached_value.byte_errors[1],
                               (int)cached_value.byte_errors[2],
                               (int)cached_value.byte_errors[3]);
                        printf(" }");
                    }
                    printf(" }");
                }
                printf("]");
            }
            printf(" }");
        }
    }
    printf("\r\n  ],\r\n");

    printf("  \"switches\": [\r\n");
    for (uint8_t i = 0u; i < mapping_count; i++) {
        const DiagSwitchMapping *mapping = &mappings[i];
        const DaliInputEvent *event = &mapping->event;
        if (!mapping->valid) {
            continue;
        }

        if (i > 0u) {
            printf(",\r\n");
        }

        printf("    { \"order\": %u", (unsigned)mapping->order);
        diag_print_event_json_fields(event);
        printf(", \"raw\": \"0x%0*" PRIX32 "\"",
               diag_frame_hex_width(&event->raw),
               event->raw.data);
        printf(", \"raw_bits\": %u", (unsigned)event->raw.bit_length);
        printf(", \"first_seen_us\": %" PRIu32, mapping->first_seen_us);
        printf(", \"seen_count\": %" PRIu32, mapping->seen_count);
        printf(" }");
    }
    printf("\r\n  ],\r\n");
    diag_print_capture_json();
    printf("\r\n");
    printf("}\r\n");
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
    printf("bus check:\r\n");
    if (rx_err == DALI_OK) {
        printf("  RX level: %u (%s)\r\n",
               (unsigned)rx_level,
               rx_level != 0u ? "idle-high candidate" : "active-low/stuck-low candidate");
    } else {
        printf("  RX level: unavailable (ERR %d)\r\n", (int)rx_err);
    }
    printf("  scheduler: %s\r\n",
           diag_sched_state_name(dali_sched_state()));
    printf("  last RX: %s\r\n", s_has_last_rx_frame ? "yes" : "none");
    if (s_has_last_rx_frame) {
        diag_print_frame("    ", &s_last_rx_frame);
    }
    printf("  event queue: %u queued, %" PRIu32 " dropped\r\n",
           (unsigned)diag_event_queue_count_snapshot(),
           diag_event_queue_dropped_snapshot());

    uint32_t capture_dropped = 0u;
    bool capture_enabled = false;
    uint8_t capture_count =
        diag_capture_snapshot(NULL, 0u, &capture_dropped, &capture_enabled);
    printf("  capture: %s, %u records, %" PRIu32 " dropped\r\n",
           capture_enabled ? "on" : "off",
           (unsigned)capture_count,
           capture_dropped);
    printf("  stats: malformed=%" PRIu32 ", timeouts=%" PRIu32
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

    printf("smoke %u: query-only diagnostic pass\r\n", (unsigned)addr);

    DaliFrame status_reply = {0u, 0u};
    DaliError err = diag_query_status(target, &status_reply);
    if (err == DALI_OK) {
        uint8_t status = (uint8_t)(status_reply.data & 0xFFu);
        printf("  status: PASS 0x%02X\r\n", (unsigned)status);
        pass++;

        DaliDiscoveryInventory inventory;
        if (!diag_inventory_snapshot(&inventory)) {
            (void)dali_discovery_inventory_reset(&inventory);
            inventory.valid = true;
        }
        if (dali_discovery_inventory_store_status(&inventory, addr, status) == DALI_OK) {
            diag_inventory_replace(&inventory);
        }
    } else {
        printf("  status: ERR %d\r\n", (int)err);
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
        err = diag_query_u8(target, queries[i].id, 0u, &value);
        if (err == DALI_OK) {
            printf("  %s: PASS %u (0x%02X)\r\n",
                   queries[i].name,
                   (unsigned)value,
                   (unsigned)value);
            pass++;
        } else if (err == DALI_ERR_TIMEOUT) {
            printf("  %s: timeout/skip\r\n", queries[i].name);
            skip++;
        } else {
            printf("  %s: ERR %d\r\n", queries[i].name, (int)err);
            fail++;
        }
    }

    DaliDiscoveryTransport transport = diag_discovery_transport();
    DaliDiscoveryInputDevice input;
    err = dali_discovery_query_input_device(&transport, addr, &input);
    if (err == DALI_OK) {
        printf("  input instances: PASS %u\r\n",
               (unsigned)input.device.instance_count);
        pass++;
        diag_input_cache_store(&input);

        DaliDiscoveryInventory inventory;
        if (!diag_inventory_snapshot(&inventory)) {
            (void)dali_discovery_inventory_reset(&inventory);
            inventory.valid = true;
        }
        if (dali_discovery_inventory_update_input_device(&inventory, &input) == DALI_OK) {
            diag_inventory_replace(&inventory);
        }

        uint8_t count = dali_discovery_input_visible_instance_count(&input);
        for (uint8_t instance = 0u; instance < count; instance++) {
            if (input.instance_type_errors[instance] == DALI_OK) {
                diag_sensor_poll_instance(addr, &input.device.instances[instance]);
            }
        }
    } else if (err == DALI_ERR_TIMEOUT) {
        printf("  input instances: timeout/skip\r\n");
        skip++;
    } else {
        printf("  input instances: ERR %d\r\n", (int)err);
        fail++;
    }

    printf("smoke %u: pass=%u fail=%u skip=%u\r\n",
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
        printf("identify: invalid address\r\n");
        return;
    }

    printf("Blinking addr %u between min and max for %u seconds.\r\n",
           (unsigned)addr,
           (unsigned)((DIAG_IDENTIFY_CYCLES * DIAG_IDENTIFY_STEP_MS * 2u) / 1000u));

    for (uint8_t i = 0u; i < DIAG_IDENTIFY_CYCLES; i++) {
        DaliError err = diag_send_no_reply(&max_frame, false);
        if (err != DALI_OK) {
            printf("identify: max ERR %d\r\n", (int)err);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(DIAG_IDENTIFY_STEP_MS));

        err = diag_send_no_reply(&min_frame, false);
        if (err != DALI_OK) {
            printf("identify: min ERR %d\r\n", (int)err);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(DIAG_IDENTIFY_STEP_MS));
    }

    printf("identify: done\r\n");
}

/* ---------------------------------------------------------------------------
 * Named tables
 * --------------------------------------------------------------------------*/

static void cmd_list(const DaliCliTokens *t)
{
    DaliCliTableId table;
    if (!dali_cli_table_find(t->tok[1], &table)) {
        printf("list: unknown table '%s'\r\n", t->tok[1]);
        dali_cli_print_table_names(&s_out);
        return;
    }
    dali_cli_print_table(&s_out, table);
}

/* ---------------------------------------------------------------------------
 * Memory
 * --------------------------------------------------------------------------*/

#define DIAG_MEMREAD_MAX_BYTES 32u

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
         (!dali_cli_parse_u8(t->tok[4], DIAG_MEMREAD_MAX_BYTES, &count) || count == 0u))) {
        dali_cli_print_usage(&s_out, usage);
        printf("       count 1-%u; the block must end at offset 255\r\n",
               (unsigned)DIAG_MEMREAD_MAX_BYTES);
        return;
    }
    if ((unsigned)offset + (unsigned)count > 256u) {
        printf("memread: bank %u has no location 0x%02X\r\n",
               (unsigned)bank, (unsigned)((unsigned)offset + count - 1u));
        return;
    }

    uint8_t buf[DIAG_MEMREAD_MAX_BYTES];
    DaliDiscoveryTransport transport = diag_discovery_transport();
    DaliError err = dali_memory_read_bytes(&transport, addr, bank, offset, buf, count);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, "memread", err);
        return;
    }

    printf("memread %u:\r\n", (unsigned)addr);
    for (uint8_t i = 0u; i < count; i += 8u) {
        uint8_t chunk = (uint8_t)(count - i);
        if (chunk > 8u) { chunk = 8u; }
        dali_cli_print_memory_block(&s_out, bank, (uint8_t)(offset + i), &buf[i], chunk);
    }
}

static void diag_print_hex_bytes(const char *label, const uint8_t *data, uint8_t count)
{
    printf("  %s:", label);
    for (uint8_t i = 0u; i < count; i++) {
        printf(" %02X", (unsigned)data[i]);
    }
    printf("\r\n");
}

static void cmd_meminfo(const DaliCliTokens *t)
{
    uint8_t addr;
    if (!dali_cli_parse_short_addr(t->tok[1], &addr)) {
        dali_cli_print_usage(&s_out, dali_cli_command_for_id(DALI_CLI_CMD_MEMINFO));
        return;
    }

    DaliDiscoveryTransport transport = diag_discovery_transport();
    DaliMemoryBank0Identity identity;
    DaliError err = dali_memory_read_bank0_identity(&transport, addr, &identity);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, "meminfo", err);
        return;
    }

    printf("meminfo %u (control gear, bank 0):\r\n", (unsigned)addr);
    diag_print_hex_bytes("GTIN          ", identity.gtin, DALI_MEMORY_BANK0_GTIN_LEN);
    printf("  firmware      : %u.%u\r\n",
           (unsigned)identity.fw_major, (unsigned)identity.fw_minor);
    diag_print_hex_bytes("identification", identity.serial,
                         DALI_MEMORY_BANK0_IDENTIFICATION_LEN);
    printf("  hardware      : %u.%u\r\n",
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
            printf("usage: devmem write <addr> <bank> <offset> <value>\r\n");
            return;
        }
        err = dali_memory_build_control_device_write_sequence(addr, bank, offset,
                                                             value, &seq);
        if (err != DALI_OK) {
            dali_cli_print_error(&s_out, "devmem write", err);
            return;
        }
        err = diag_sched_sequence_sync(&seq, &result);
        diag_print_sequence_result("devmem write", err, &result);
        if (err == DALI_OK) {
            printf("devmem write: queued and transmitted; not read back\r\n");
        }
        return;
    }

    uint8_t count = 1u;
    if (t->count == 6u &&
        (!dali_cli_parse_u8(t->tok[5], DALI_MEMORY_MAX_SEQUENCE_READ_BYTES, &count) ||
         count == 0u)) {
        printf("usage: devmem read <addr> <bank> <offset> [1-%u]\r\n",
               (unsigned)DALI_MEMORY_MAX_SEQUENCE_READ_BYTES);
        return;
    }
    if ((unsigned)offset + (unsigned)count > 256u) {
        printf("devmem read: bank %u has no location 0x%02X\r\n",
               (unsigned)bank, (unsigned)((unsigned)offset + count - 1u));
        return;
    }

    err = dali_memory_build_control_device_read_sequence(addr, bank, offset,
                                                         count, &seq);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, "devmem read", err);
        return;
    }

    err = diag_sched_sequence_sync(&seq, &result);
    if (err != DALI_OK) {
        diag_print_sequence_result("devmem read", err, &result);
        return;
    }

    uint8_t buf[DALI_MEMORY_MAX_SEQUENCE_READ_BYTES];
    err = dali_memory_read_from_sequence(&result, count, buf);
    if (err != DALI_OK) {
        dali_cli_print_error(&s_out, "devmem read", err);
        return;
    }

    printf("devmem read %u:\r\n", (unsigned)addr);
    dali_cli_print_memory_block(&s_out, bank, offset, buf, count);
}

/* ---------------------------------------------------------------------------
 * Device types 6 and 8
 *
 * Every command here goes out as one sequence: the DTR loads, ENABLE DEVICE
 * TYPE, and the command itself cannot be separated without the gear answering
 * a different question than the one asked.
 * --------------------------------------------------------------------------*/

static bool diag_parse_dt_dtr_bytes(const DaliCliTokens *t,
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

static void diag_run_dt_command(const char             *verb,
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
    err = diag_sched_sequence_sync(&seq, &result);
    if (err != DALI_OK) {
        diag_print_sequence_result(spec->name, err, &result);
        return;
    }

    if (!expects_reply) {
        printf("%s %s: OK\r\n", verb, spec->name);
        return;
    }

    DaliFrame reply;
    uint8_t step = (uint8_t)(spec->dtr_count + 1u);
    if (!dali_sequence_result_reply(&result, step, &reply)) {
        printf("%s: no reply captured\r\n", spec->name);
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
        printf("dt6: unknown command '%s'\r\n", t->tok[2]);
        printf("use 'list dt6'\r\n");
        return;
    }

    uint8_t dtr[DALI_DT6_MAX_DTR_BYTES] = {0};
    if (t->count != (uint8_t)(3u + spec->dtr_count) ||
        !diag_parse_dt_dtr_bytes(t, 3u, spec->dtr_count, dtr)) {
        printf("usage: dt6 <addr> %s", spec->name);
        for (uint8_t i = 0u; i < spec->dtr_count; i++) {
            printf(" <0-255>");
        }
        printf("\r\n");
        if (spec->dtr_help != NULL) {
            printf("       %s\r\n", spec->dtr_help);
        }
        return;
    }

    diag_run_dt_command("dt6", 6u, spec, addr, dtr);
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
        printf("dt8: unknown command '%s'\r\n", t->tok[2]);
        printf("use 'list dt8'\r\n");
        return;
    }

    if (spec->kind == DALI_CLI_DT_COLOUR16) {
        const DaliCliDt8Selector *sel = t->count == 4u
                                      ? dali_cli_dt8_selector_find(t->tok[3])
                                      : NULL;
        if (sel == NULL) {
            printf("usage: dt8 <addr> colour <selector>\r\n");
            printf("       use 'list selectors'\r\n");
            return;
        }

        DaliSequence seq;
        DaliError err = dali_dt8_build_colour_value_sequence(addr, sel->selector, &seq);
        if (err != DALI_OK) {
            dali_cli_print_error(&s_out, "colour", err);
            return;
        }

        DaliSequenceResult result;
        err = diag_sched_sequence_sync(&seq, &result);
        if (err != DALI_OK) {
            diag_print_sequence_result("colour", err, &result);
            return;
        }

        uint16_t value = 0u;
        err = dali_dt8_colour_value_from_sequence(&result, &value);
        if (err != DALI_OK) {
            dali_cli_print_error(&s_out, "colour", err);
            return;
        }

        printf("colour %s: %u (0x%04X)", sel->name, (unsigned)value, (unsigned)value);
        if (sel->selector == DALI_DT8_VALUE_COLOUR_TEMP_TC) {
            printf(" = %u K", (unsigned)dali_dt8_mirek_to_kelvin(value));
        }
        printf("\r\n");
        return;
    }

    uint8_t dtr[DALI_DT8_MAX_DTR_BYTES] = {0};
    if (t->count != (uint8_t)(3u + spec->dtr_count) ||
        !diag_parse_dt_dtr_bytes(t, 3u, spec->dtr_count, dtr)) {
        printf("usage: dt8 <addr> %s", spec->name);
        for (uint8_t i = 0u; i < spec->dtr_count; i++) {
            printf(" <0-255>");
        }
        printf("\r\n");
        if (spec->dtr_help != NULL) {
            printf("       %s\r\n", spec->dtr_help);
        }
        return;
    }

    diag_run_dt_command("dt8", 8u, spec, addr, dtr);
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
        printf("iquery: unknown query '%s'\r\n", t->tok[3]);
        printf("use 'list iquery'\r\n");
        return;
    }

    uint8_t dtr0 = 0u;
    uint8_t expected = spec->needs_dtr0 ? 5u : 4u;
    if (t->count != expected ||
        (spec->needs_dtr0 && !dali_cli_parse_u8(t->tok[4], 255u, &dtr0))) {
        printf("usage: iquery <addr> <instance> %s%s\r\n",
               spec->name,
               spec->needs_dtr0 ? " <0-255>" : "");
        if (spec->dtr0_help != NULL) {
            printf("       %s\r\n", spec->dtr0_help);
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
        err = diag_sched_sequence_sync(&seq, &result);
        if (err != DALI_OK) {
            diag_print_sequence_result(spec->name, err, &result);
            return;
        }
        if (!dali_sequence_result_reply(&result, 1u, &reply)) {
            printf("%s: no reply captured\r\n", spec->name);
            return;
        }
    } else {
        /* Plain instance reads change nothing on the device, so one retry after
         * a lost reply is safe. */
        err = diag_sched_sync(&command, true, 1u, false, &reply);
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
        printf("iconfig: unknown command '%s'\r\n", t->tok[3]);
        printf("use 'list iconfig'\r\n");
        return;
    }

    uint8_t dtr[DALI_INPUT_CONFIG_MAX_DTR_BYTES] = {0};
    if (t->count != (uint8_t)(4u + spec->dtr_count) ||
        !diag_parse_dt_dtr_bytes(t, 4u, spec->dtr_count, dtr)) {
        printf("usage: iconfig <addr> <instance> %s", spec->name);
        for (uint8_t i = 0u; i < spec->dtr_count; i++) {
            printf(" <0-255>");
        }
        printf("\r\n");
        if (spec->dtr_help != NULL) {
            printf("       %s\r\n", spec->dtr_help);
        }
        return;
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
    err = diag_sched_sequence_sync(&seq, &result);
    diag_print_sequence_result(spec->name, err, &result);
    if (err == DALI_OK) {
        /* Transmitted, not acknowledged: read the value back with iquery before
         * treating an input-device configuration write as applied. */
        printf("%s: transmitted; verify with iquery\r\n", spec->name);
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
            printf("usage: vendor lunatone <addr> <instance> <name>\r\n");
            return;
        }

        const DaliCliLunatoneCommand *spec = dali_cli_lunatone_find(t->tok[4]);
        if (spec == NULL) {
            printf("vendor: unknown lunatone query '%s'\r\n", t->tok[4]);
            printf("use 'list vendor'\r\n");
            return;
        }

        DaliFrame frame;
        DaliFrame reply = {0u, 0u};
        DaliError err = dali_lunatone_build_instance_command(addr, instance,
                                                             spec->id, &frame);
        if (err == DALI_OK) {
            err = diag_sched_sync(&frame, true, 1u, false, &reply);
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
            printf("usage: vendor steinel <instance> <raw>\r\n");
            return;
        }

        const DaliSteinelInstanceInfo *info =
            dali_steinel_hf360_instance_lookup(instance);
        if (info == NULL) {
            printf("vendor steinel: instance %u is not in the HF 360 II profile\r\n",
                   (unsigned)instance);
            return;
        }

        printf("steinel %s (instance %u, type %u): raw=%u\r\n",
               info->name, (unsigned)instance, (unsigned)info->type, (unsigned)raw);
        switch (instance) {
            case DALI_STEINEL_HF360_INSTANCE_TEMPERATURE: {
                int32_t deci = dali_steinel_temperature_deci_c((uint16_t)raw);
                printf("  temperature: %ld.%ld C\r\n",
                       (long)(deci / 10), (long)(deci < 0 ? -(deci % 10) : deci % 10));
                break;
            }
            case DALI_STEINEL_HF360_INSTANCE_HUMIDITY: {
                uint32_t deci = dali_steinel_humidity_deci_percent((uint16_t)raw);
                printf("  humidity: %lu.%lu %%\r\n",
                       (unsigned long)(deci / 10u), (unsigned long)(deci % 10u));
                break;
            }
            case DALI_STEINEL_HF360_INSTANCE_BRIGHTNESS:
                printf("  illuminance: %lu.%02lu lx (scale 0.01)\r\n",
                       (unsigned long)(raw / 100u), (unsigned long)(raw % 100u));
                break;
            default:
                printf("  no conversion defined; the raw value is authoritative\r\n");
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

static void diag_execute(DaliCliCommandId id, const DaliCliTokens *t)
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

static void dispatch(char *line)
{
    DaliCliTokens tokens;
    const DaliCliCommandSpec *spec = NULL;

    DaliCliResolveResult result = dali_cli_resolve(line, &tokens, &spec);
    if (result == DALI_CLI_RESOLVE_EMPTY) {
        return;
    }

    printf("> %s\r\n", line);

    if (result != DALI_CLI_RESOLVE_OK) {
        dali_cli_report_resolve(&s_out, result, &tokens, spec);
        return;
    }

    diag_execute(spec->id, &tokens);
}

#endif /* !DALI_HOST_BUILD */

/* ---------------------------------------------------------------------------
 * Diagnostic task
 * --------------------------------------------------------------------------*/
#ifndef DALI_HOST_BUILD
static void diag_task(void *arg)
{
    (void)arg;

    char line[DIAG_LINE_MAX];
    uint8_t pos = 0u;

    ESP_LOGI(TAG, "Diagnostic CLI ready (115200 baud)");
    printf("\r\nDALI-2 diagnostic shell. Type 'help' for commands.\r\n> ");

    for (;;) {
        uint8_t ch;
        int n = uart_read_bytes(DIAG_UART_NUM, &ch, 1, pdMS_TO_TICKS(10));
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(100u));
            continue;
        }
        if (n == 0) { continue; }

        if (ch == '\n' || ch == '\r') {
            line[pos] = '\0';
            dispatch(line);
            pos = 0u;
            printf("> ");
            fflush(stdout);
        } else if (ch == '\b' || ch == 127u) {
            if (pos > 0u) { pos--; }
        } else if (pos < (DIAG_LINE_MAX - 1u)) {
            line[pos++] = (char)ch;
        }
    }
}
#endif /* !DALI_HOST_BUILD */

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------*/

DaliError dali_diag_init(void)
{
    s_trace_enabled = false;

#ifndef DALI_HOST_BUILD
    DaliError err = diag_uart_init();
    if (err != DALI_OK) {
        return err;
    }

    diag_last_rx_reset();
    diag_inventory_reset();
    diag_events_reset();
    diag_switch_mappings_reset();
    diag_input_cache_reset();
    diag_sensor_value_cache_reset();
    diag_capture_reset();

    err = dali_sched_set_trace_callback(diag_trace_cb, NULL);
    if (err != DALI_OK) {
        return err;
    }
    err = dali_sched_set_event_callback(diag_event_cb, NULL);
    if (err != DALI_OK) {
        return err;
    }

    xTaskCreate(diag_task, "dali_diag", DIAG_TASK_STACK, NULL,
                DIAG_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Diagnostic task started");
#endif

    return DALI_OK;
}

bool dali_diag_trace_enabled(void)
{
    return s_trace_enabled;
}
