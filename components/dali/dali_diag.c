#include "dali_diag.h"
#include "dali_phy.h"
#include "dali_scheduler.h"
#include "dali_protocol.h"
#include "dali_control.h"
#include "dali_input_device.h"
#include "dali_input_poll.h"
#include "dali_event.h"
#include "dali_discovery.h"
#include "dali_commissioning.h"

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
    uint8_t      failed_step;
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
                .failed_step  = DALI_SEQUENCE_NO_FAILED_STEP,
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
    ctx->result    = result;
    ctx->has_reply = (reply != NULL);
    ctx->failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
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

static void diag_sequence_sync_cb(DaliError result,
                                  uint8_t failed_step,
                                  const DaliFrame *last_reply,
                                  void *cb_ctx)
{
    DiagSyncCtx *ctx = (DiagSyncCtx *)cb_ctx;
    if (ctx == NULL) {
        return;
    }

    TaskHandle_t notify_task = NULL;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    ctx->result = result;
    ctx->failed_step = failed_step;
    ctx->has_reply = (last_reply != NULL);
    if (last_reply != NULL) {
        ctx->reply = *last_reply;
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

static bool diag_sync_complete(DiagSyncCtx *ctx)
{
    bool complete;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    complete = ctx->complete;
    taskEXIT_CRITICAL(&s_diag_sync_mux);

    return complete;
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

static DaliError diag_sched_sequence_sync(DaliSequence *seq,
                                          DaliFrame *reply_out,
                                          uint8_t *failed_step_out)
{
    if (seq == NULL) {
        return DALI_ERR_INVALID;
    }

    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0u);

    DiagSyncCtx *ctx = diag_sync_alloc_slot(current_task);
    if (ctx == NULL) {
        return DALI_ERR_BUSY;
    }

    seq->on_complete = diag_sequence_sync_cb;
    seq->cb_ctx = ctx;

    DaliError err = dali_sched_enqueue_sequence(seq);
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
    uint8_t failed_step = DALI_SEQUENCE_NO_FAILED_STEP;

    taskENTER_CRITICAL(&s_diag_sync_mux);
    completed = ctx->complete;
    if (completed) {
        result      = ctx->result;
        has_reply   = ctx->has_reply;
        reply       = ctx->reply;
        failed_step = ctx->failed_step;
        ctx->waiting_task = NULL;
        ctx->in_use       = false;
        ctx->complete     = false;
    } else {
        ctx->waiting_task = NULL;
    }
    taskEXIT_CRITICAL(&s_diag_sync_mux);

    if (failed_step_out != NULL) {
        *failed_step_out = failed_step;
    }
    if (completed && reply_out != NULL && has_reply) {
        *reply_out = reply;
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

static DaliDiscoveryTransport diag_discovery_transport(void)
{
    return (DaliDiscoveryTransport){
        .transact = diag_discovery_transact,
        .ctx = NULL,
    };
}

static int diag_frame_hex_width(const DaliFrame *frame)
{
    return (int)((frame->bit_length + 3u) / 4u);
}

static void diag_print_frame(const char *prefix, const DaliFrame *frame)
{
    if (frame == NULL) {
        return;
    }
    printf("%s0x%0*" PRIX32 " (%u-bit)\r\n",
           prefix,
           diag_frame_hex_width(frame),
           frame->data,
           (unsigned)frame->bit_length);
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

static bool diag_parse_uint8_token(const char *text, unsigned max_value, uint8_t *out)
{
    if (text == NULL || text[0] == '\0' || out == NULL) {
        return false;
    }

    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > max_value) {
        return false;
    }

    *out = (uint8_t)value;
    return true;
}

static bool diag_parse_target_token(const char *text, DaliTarget *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }

    if (strcmp(text, "b") == 0 || strcmp(text, "broadcast") == 0) {
        *out = (DaliTarget){ .type = DALI_ADDR_BROADCAST, .address = 0u };
        return true;
    }

    if (text[0] == 'g') {
        uint8_t group;
        if (!diag_parse_uint8_token(text + 1, DALI_MAX_GROUP, &group)) {
            return false;
        }
        *out = (DaliTarget){ .type = DALI_ADDR_GROUP, .address = group };
        return true;
    }

    if (text[0] == 's') {
        uint8_t addr;
        if (!diag_parse_uint8_token(text + 1, DALI_MAX_SHORT_ADDRESS, &addr)) {
            return false;
        }
        *out = (DaliTarget){ .type = DALI_ADDR_SHORT, .address = addr };
        return true;
    }

    uint8_t addr;
    if (!diag_parse_uint8_token(text, DALI_MAX_SHORT_ADDRESS, &addr)) {
        return false;
    }
    *out = (DaliTarget){ .type = DALI_ADDR_SHORT, .address = addr };
    return true;
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
    if (event->has_instance) {
        printf(", \"instance\": %u", (unsigned)event->instance);
    }
    printf(", \"action_code\": %u", (unsigned)event->event_code);
    printf(", \"action_hex\": \"0x%02X\"", (unsigned)event->event_code);
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
    printf("%sraw=0x%0*" PRIX32 " frame=%s addr_byte=0x%02X kind=%s",
           prefix,
           diag_frame_hex_width(&event->raw),
           event->raw.data,
           dali_event_frame_kind_name(event->frame_kind),
           (unsigned)event->address_byte,
           dali_event_address_kind_name(event->address_kind));

    if (event->address_kind == DALI_EVENT_ADDRESS_SHORT ||
        event->address_kind == DALI_EVENT_ADDRESS_GROUP) {
        printf(" addr=%u", (unsigned)event->address);
    }

    printf(" selector=%u", event->address_selector ? 1u : 0u);
    if (event->has_instance) {
        printf(" inst=%u", (unsigned)event->instance);
    }

    printf(" action=0x%02X %s time=%" PRIu32 "us\r\n",
           (unsigned)event->event_code,
           dali_event_action_name(event),
           record->timestamp_us);
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
 * --------------------------------------------------------------------------*/

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
#endif
}

static void cmd_trace(const char *args)
{
#ifndef DALI_HOST_BUILD
    if (strncmp(args, "on", 2) == 0) {
        s_trace_enabled = true;
        printf("trace on\r\n");
    } else if (strncmp(args, "off", 3) == 0) {
        s_trace_enabled = false;
        printf("trace off\r\n");
    } else {
        printf("usage: trace on|off\r\n");
    }
#else
    (void)args;
#endif
}

static void cmd_reset(void)
{
    dali_sched_reset();
    dali_phy_reset();
#ifndef DALI_HOST_BUILD
    diag_last_rx_reset();
    diag_inventory_reset();
    diag_events_reset();
    diag_switch_mappings_reset();
    diag_input_cache_reset();
    diag_sensor_value_cache_reset();
    diag_capture_reset();
    printf("reset OK\r\n");
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
    if (err == DALI_OK) {
        printf("%s: OK\r\n", name);
    } else {
        printf("%s: ERR %d\r\n", name, (int)err);
    }
}

static bool diag_parse_target_arg(const char *args, DaliTarget *target)
{
    char target_text[16] = {0};
    char extra[2] = {0};

    if (sscanf(args, "%15s %1s", target_text, extra) != 1) {
        return false;
    }
    return diag_parse_target_token(target_text, target);
}

static bool diag_parse_short_addr_arg(const char *args, uint8_t *addr)
{
    char addr_text[16] = {0};
    char extra[2] = {0};

    if (sscanf(args, "%15s %1s", addr_text, extra) != 1) {
        return false;
    }
    return diag_parse_uint8_token(addr_text, DALI_MAX_SHORT_ADDRESS, addr);
}

static void cmd_level(const char *args)
{
#ifndef DALI_HOST_BUILD
    char target_text[16] = {0};
    unsigned level = 0u;
    char extra[2] = {0};
    if (sscanf(args, "%15s %u %1s", target_text, &level, extra) != 2 ||
        level > DALI_DAPC_MAX_LEVEL) {
        printf("usage: level <addr|sN|gN|b> <0-%u>\r\n",
               (unsigned)DALI_DAPC_MAX_LEVEL);
        return;
    }

    DaliTarget target;
    DaliFrame frame;
    DaliError err;
    if (!diag_parse_target_token(target_text, &target)) {
        printf("usage: level <addr|sN|gN|b> <0-%u>\r\n",
               (unsigned)DALI_DAPC_MAX_LEVEL);
        return;
    }

    err = dali_control_build_dapc(target, (uint8_t)level, &frame);
    if (err == DALI_OK) {
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result("level", err);
#else
    (void)args;
#endif
}

static void cmd_off(const char *args)
{
#ifndef DALI_HOST_BUILD
    DaliTarget target;
    DaliFrame frame;
    DaliError err;

    if (!diag_parse_target_arg(args, &target)) {
        printf("usage: off <addr|sN|gN|b>\r\n");
        return;
    }

    err = dali_control_build_off(target, &frame);
    if (err == DALI_OK) {
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result("off", err);
#else
    (void)args;
#endif
}

typedef DaliError (*DiagTargetFrameBuilder)(DaliTarget target, DaliFrame *out);

static void cmd_target_frame(const char *args,
                             const char *name,
                             DiagTargetFrameBuilder builder)
{
#ifndef DALI_HOST_BUILD
    DaliTarget target;
    DaliFrame frame;
    DaliError err;

    if (name == NULL || builder == NULL || !diag_parse_target_arg(args, &target)) {
        printf("usage: %s <addr|sN|gN|b>\r\n", name != NULL ? name : "cmd");
        return;
    }

    err = builder(target, &frame);
    if (err == DALI_OK) {
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result(name, err);
#else
    (void)args;
    (void)name;
    (void)builder;
#endif
}

static void cmd_recall(const char *args, bool max_level)
{
#ifndef DALI_HOST_BUILD
    DaliTarget target;
    DaliFrame frame;
    DaliError err;

    if (!diag_parse_target_arg(args, &target)) {
        printf("usage: %s <addr|sN|gN|b>\r\n", max_level ? "max" : "min");
        return;
    }

    err = max_level ? dali_control_build_recall_max(target, &frame)
                    : dali_control_build_recall_min(target, &frame);
    if (err == DALI_OK) {
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result(max_level ? "max" : "min", err);
#else
    (void)args;
    (void)max_level;
#endif
}

static void cmd_scene(const char *args)
{
#ifndef DALI_HOST_BUILD
    char target_text[16] = {0};
    unsigned scene = 0u;
    char extra[2] = {0};

    if (sscanf(args, "%15s %u %1s", target_text, &scene, extra) != 2 ||
        scene > DALI_MAX_SCENE) {
        printf("usage: scene <addr|sN|gN|b> <0-%u>\r\n", (unsigned)DALI_MAX_SCENE);
        return;
    }

    DaliTarget target;
    DaliFrame frame;
    if (!diag_parse_target_token(target_text, &target)) {
        printf("usage: scene <addr|sN|gN|b> <0-%u>\r\n", (unsigned)DALI_MAX_SCENE);
        return;
    }

    DaliError err = dali_control_build_go_to_scene(target, (uint8_t)scene, &frame);
    if (err == DALI_OK) {
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result("scene", err);
#else
    (void)args;
#endif
}

static void cmd_raw(const char *args)
{
#ifndef DALI_HOST_BUILD
    unsigned long hex_val = 0;
    int bit_len = 0;
    char opt[8] = {0};
    char extra[2] = {0};
    int parsed = sscanf(args, "%lx len=%d %7s %1s",
                        &hex_val, &bit_len, opt, extra);
    bool wait_reply = false;

    if (parsed == 3) {
        wait_reply = (strcmp(opt, "wait") == 0);
    }
    if (parsed < 2 || parsed > 3 ||
        (parsed == 3 && !wait_reply) ||
        bit_len < 1 || bit_len > 24 ||
        hex_val > ((1UL << (unsigned)bit_len) - 1UL)) {
        g_dali_stats.raw_malformed++;
        printf("usage: raw <hex> len=<bits> [wait]\r\n");
        return;
    }
    DaliFrame f;
    f.data       = (uint32_t)hex_val;
    f.bit_length = (uint8_t)bit_len;

    DaliFrame reply = {0u, 0u};
    DaliError err = diag_sched_sync(&f, wait_reply, 0u, false,
                                    wait_reply ? &reply : NULL);
    if (wait_reply) {
        if (err == DALI_OK) {
            printf("RX: 0x%0*" PRIX32 " (%u-bit)\r\n",
                   (int)((reply.bit_length + 3u) / 4u),
                   reply.data,
                   (unsigned)reply.bit_length);
        } else if (err == DALI_ERR_TIMEOUT) {
            printf("RX: timeout\r\n");
        } else {
            printf("TX/RX: ERR %d\r\n", (int)err);
        }
        return;
    }

    if (err == DALI_OK) {
        printf("TX: OK\r\n");
    } else {
        printf("TX: ERR %d\r\n", (int)err);
    }
#else
    (void)args;
#endif
}

static void cmd_dtr(const char *args)
{
#ifndef DALI_HOST_BUILD
    unsigned reg = 0u;
    unsigned value = 0u;
    char extra[2] = {0};

    if (sscanf(args, "%u %u %1s", &reg, &value, extra) != 2 ||
        reg > 2u || value > 255u) {
        printf("usage: dtr <0|1|2> <0-255>\r\n");
        return;
    }

    DaliFrame frame;
    DaliError err = dali_control_build_dtr((DaliDtrRegister)reg,
                                           (uint8_t)value,
                                           &frame);
    if (err == DALI_OK) {
        err = diag_send_no_reply(&frame, false);
    }
    diag_print_tx_result("dtr", err);
#else
    (void)args;
#endif
}

static void diag_print_status_fields(uint8_t raw)
{
    DaliStatus s;
    if (dali_parse_status(raw, &s) != DALI_OK) {
        return;
    }

    printf("  Ballast failure:       %s\r\n", s.ballast_failure       ? "YES" : "no");
    printf("  Lamp failure:          %s\r\n", s.lamp_failure          ? "YES" : "no");
    printf("  Lamp arc power on:     %s\r\n", s.lamp_arc_power_on     ? "YES" : "no");
    printf("  Limit error:           %s\r\n", s.limit_error           ? "YES" : "no");
    printf("  Fade running:          %s\r\n", s.fade_running          ? "YES" : "no");
    printf("  Reset state:           %s\r\n", s.reset_state           ? "YES" : "no");
    printf("  Missing short address: %s\r\n", s.missing_short_address ? "YES" : "no");
    printf("  Power failure:         %s\r\n", s.power_failure         ? "YES" : "no");
}

static DaliError diag_query_status(DaliTarget target, DaliFrame *reply)
{
    DaliFrame frame;
    DaliError err = dali_control_build_query_status(target, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return diag_sched_sync(&frame, true, 1u, false, reply);
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

    err = diag_sched_sync(&frame, true, 1u, false, &reply);
    if (err != DALI_OK) {
        return err;
    }
    if (reply.bit_length != DALI_BACKWARD_FRAME_BITS) {
        return DALI_ERR_MALFORMED;
    }

    *out = (uint8_t)(reply.data & 0xFFu);
    return DALI_OK;
}

static void cmd_status(const char *args)
{
#ifndef DALI_HOST_BUILD
    DaliTarget target;
    DaliFrame reply = {0u, 0u};

    if (!diag_parse_target_arg(args, &target)) {
        printf("usage: status <addr|sN|gN|b>\r\n");
        return;
    }

    if (target.type != DALI_ADDR_SHORT) {
        printf("status: group/broadcast replies may collide on a real bus\r\n");
    }

    DaliError err = diag_query_status(target, &reply);
    if (err == DALI_OK) {
        uint8_t raw = (uint8_t)(reply.data & 0xFFu);
        printf("status: 0x%02X\r\n", (unsigned)raw);
        diag_print_status_fields(raw);
    } else if (err == DALI_ERR_TIMEOUT) {
        printf("status: timeout\r\n");
    } else {
        printf("status: ERR %d\r\n", (int)err);
    }
#else
    (void)args;
#endif
}

typedef struct {
    const char   *name;
    DaliCommandId id;
    bool          needs_param;
    uint8_t       max_param;
} DiagQuerySpec;

static const DiagQuerySpec s_diag_query_specs[] = {
    { "status",            DALI_CMD_QUERY_STATUS,                     false, 0u },
    { "present",           DALI_CMD_QUERY_CONTROL_GEAR_PRESENT,       false, 0u },
    { "lamp-failure",      DALI_CMD_QUERY_LAMP_FAILURE,               false, 0u },
    { "lamp-on",           DALI_CMD_QUERY_LAMP_POWER_ON,              false, 0u },
    { "limit-error",       DALI_CMD_QUERY_LIMIT_ERROR,                false, 0u },
    { "reset-state",       DALI_CMD_QUERY_RESET_STATE,                false, 0u },
    { "missing-address",   DALI_CMD_QUERY_MISSING_SHORT_ADDRESS,      false, 0u },
    { "version",           DALI_CMD_QUERY_VERSION_NUMBER,             false, 0u },
    { "dtr0",              DALI_CMD_QUERY_CONTENT_DTR0,               false, 0u },
    { "device-type",       DALI_CMD_QUERY_DEVICE_TYPE,                false, 0u },
    { "physical-min",      DALI_CMD_QUERY_PHYSICAL_MINIMUM,           false, 0u },
    { "power-failure",     DALI_CMD_QUERY_POWER_FAILURE,              false, 0u },
    { "dtr1",              DALI_CMD_QUERY_CONTENT_DTR1,               false, 0u },
    { "dtr2",              DALI_CMD_QUERY_CONTENT_DTR2,               false, 0u },
    { "operating-mode",    DALI_CMD_QUERY_OPERATING_MODE,             false, 0u },
    { "light-source",      DALI_CMD_QUERY_LIGHT_SOURCE_TYPE,          false, 0u },
    { "actual",            DALI_CMD_QUERY_ACTUAL_LEVEL,               false, 0u },
    { "max-level",         DALI_CMD_QUERY_MAX_LEVEL,                  false, 0u },
    { "min-level",         DALI_CMD_QUERY_MIN_LEVEL,                  false, 0u },
    { "power-on",          DALI_CMD_QUERY_POWER_ON_LEVEL,             false, 0u },
    { "failure-level",     DALI_CMD_QUERY_SYSTEM_FAILURE_LEVEL,       false, 0u },
    { "fade",              DALI_CMD_QUERY_FADE_TIME_FADE_RATE,        false, 0u },
    { "manufacturer-mode", DALI_CMD_QUERY_MANUFACTURER_SPECIFIC_MODE, false, 0u },
    { "next-device-type",  DALI_CMD_QUERY_NEXT_DEVICE_TYPE,           false, 0u },
    { "extended-fade",     DALI_CMD_QUERY_EXTENDED_FADE_TIME,         false, 0u },
    { "gear-failure",      DALI_CMD_QUERY_CONTROL_GEAR_FAILURE,       false, 0u },
    { "scene-level",       DALI_CMD_QUERY_SCENE_LEVEL,                true,  DALI_MAX_SCENE },
    { "groups-0-7",        DALI_CMD_QUERY_GROUPS_0_7,                 false, 0u },
    { "groups-8-15",       DALI_CMD_QUERY_GROUPS_8_15,                false, 0u },
    { "random-h",          DALI_CMD_QUERY_RANDOM_ADDRESS_H,           false, 0u },
    { "random-m",          DALI_CMD_QUERY_RANDOM_ADDRESS_M,           false, 0u },
    { "random-l",          DALI_CMD_QUERY_RANDOM_ADDRESS_L,           false, 0u },
    { "memory",            DALI_CMD_READ_MEMORY_LOCATION,             false, 0u },
    { "extended-version",  DALI_CMD_QUERY_EXTENDED_VERSION_NUMBER,    false, 0u },
};

static const DiagQuerySpec *diag_find_query_spec(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(s_diag_query_specs) / sizeof(s_diag_query_specs[0]));
         i++) {
        if (strcmp(name, s_diag_query_specs[i].name) == 0) {
            return &s_diag_query_specs[i];
        }
    }
    return NULL;
}

static void cmd_query_list(void)
{
#ifndef DALI_HOST_BUILD
    printf("query names:\r\n");
    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(s_diag_query_specs) / sizeof(s_diag_query_specs[0]));
         i++) {
        const DiagQuerySpec *spec = &s_diag_query_specs[i];
        printf("  %s", spec->name);
        if (spec->needs_param) {
            printf(" <0-%u>", (unsigned)spec->max_param);
        }
        printf("\r\n");
    }
#endif
}

static void diag_print_query_response(const DiagQuerySpec *spec,
                                      const DaliFrame *reply)
{
#ifndef DALI_HOST_BUILD
    if (spec == NULL || reply == NULL) {
        return;
    }

    DaliParsedResponse parsed;
    DaliError err = dali_parse_command_response(spec->id, reply, &parsed);
    if (err != DALI_OK) {
        printf("%s: malformed reply\r\n", spec->name);
        return;
    }

    switch (parsed.kind) {
        case DALI_RESP_STATUS:
            printf("%s: 0x%02X\r\n", spec->name, (unsigned)parsed.raw);
            diag_print_status_fields(parsed.raw);
            break;

        case DALI_RESP_YES_NO:
            printf("%s: %s (0x%02X)\r\n",
                   spec->name,
                   parsed.yes ? "yes" : "no",
                   (unsigned)parsed.raw);
            break;

        case DALI_RESP_UINT8:
            printf("%s: %u (0x%02X)\r\n",
                   spec->name,
                   (unsigned)parsed.value,
                   (unsigned)parsed.raw);
            break;

        case DALI_RESP_BITSET8:
            printf("%s: 0x%02X\r\n", spec->name, (unsigned)parsed.bitset);
            break;

        case DALI_RESP_MEMORY_BYTE:
            printf("%s: %u (0x%02X)\r\n",
                   spec->name,
                   (unsigned)parsed.value,
                   (unsigned)parsed.raw);
            break;

        case DALI_RESP_FADE_TIME_RATE:
            printf("%s: fade_time=%u fade_rate=%u (0x%02X)\r\n",
                   spec->name,
                   (unsigned)parsed.fade.fade_time,
                   (unsigned)parsed.fade.fade_rate,
                   (unsigned)parsed.raw);
            break;

        default:
            printf("%s: 0x%02X\r\n", spec->name, (unsigned)parsed.raw);
            break;
    }
#else
    (void)spec;
    (void)reply;
#endif
}

static void cmd_query(const char *args)
{
#ifndef DALI_HOST_BUILD
    char target_text[16] = {0};
    char query_text[32] = {0};
    char param_text[16] = {0};
    char extra[2] = {0};
    int parsed = sscanf(args, "%15s %31s %15s %1s",
                        target_text,
                        query_text,
                        param_text,
                        extra);

    if (parsed == 1) {
        cmd_status(args);
        return;
    }
    if (parsed < 2 || parsed > 3) {
        printf("usage: query <addr|sN|gN|b> [query-name] [param]\r\n");
        printf("       query-list\r\n");
        return;
    }

    DaliTarget target;
    if (!diag_parse_target_token(target_text, &target)) {
        printf("usage: query <addr|sN|gN|b> [query-name] [param]\r\n");
        return;
    }

    const DiagQuerySpec *spec = diag_find_query_spec(query_text);
    if (spec == NULL) {
        printf("query: unknown query '%s'\r\n", query_text);
        printf("use query-list\r\n");
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (parsed != 3 || !diag_parse_uint8_token(param_text, spec->max_param, &param)) {
            printf("usage: query <addr|sN|gN|b> %s <0-%u>\r\n",
                   spec->name,
                   (unsigned)spec->max_param);
            return;
        }
    } else if (parsed != 2) {
        printf("usage: query <addr|sN|gN|b> %s\r\n", spec->name);
        return;
    }

    if (target.type != DALI_ADDR_SHORT) {
        printf("query: group/broadcast replies may collide on a real bus\r\n");
    }

    DaliFrame frame;
    DaliFrame reply = {0u, 0u};
    DaliError err = dali_control_build_query(target, spec->id, param, &frame);
    if (err == DALI_OK) {
        err = diag_sched_sync(&frame, true, 1u, false, &reply);
    }
    if (err == DALI_OK) {
        diag_print_query_response(spec, &reply);
    } else if (err == DALI_ERR_TIMEOUT) {
        printf("%s: timeout\r\n", spec->name);
    } else {
        printf("%s: ERR %d\r\n", spec->name, (int)err);
    }
#else
    (void)args;
#endif
}

typedef struct {
    const char   *name;
    DaliCommandId id;
    bool          needs_param;
    uint8_t       max_param;
} DiagSpecialSpec;

static const DiagSpecialSpec s_diag_special_specs[] = {
    { "terminate",       DALI_CMD_TERMINATE,                    false, 0u },
    { "dtr0",            DALI_CMD_DTR0_DATA,                    true,  255u },
    { "initialise",      DALI_CMD_INITIALISE,                   true,  255u },
    { "randomize",       DALI_CMD_RANDOMIZE,                    false, 0u },
    { "compare",         DALI_CMD_COMPARE,                      false, 0u },
    { "withdraw",        DALI_CMD_WITHDRAW,                     false, 0u },
    { "ping",            DALI_CMD_PING,                         false, 0u },
    { "search-h",        DALI_CMD_SEARCH_ADDRH,                 true,  255u },
    { "search-m",        DALI_CMD_SEARCH_ADDRM,                 true,  255u },
    { "search-l",        DALI_CMD_SEARCH_ADDRL,                 true,  255u },
    { "program-short",   DALI_CMD_PROGRAM_SHORT_ADDRESS,        true,  255u },
    { "verify-short",    DALI_CMD_VERIFY_SHORT_ADDRESS,         true,  255u },
    { "query-short",     DALI_CMD_QUERY_SHORT_ADDRESS,          false, 0u },
    { "enable-type",     DALI_CMD_ENABLE_DEVICE_TYPE,           true,  255u },
    { "dtr1",            DALI_CMD_DTR1_DATA,                    true,  255u },
    { "dtr2",            DALI_CMD_DTR2_DATA,                    true,  255u },
    { "write-memory",    DALI_CMD_WRITE_MEMORY_LOCATION,        true,  255u },
    { "write-memory-nr", DALI_CMD_WRITE_MEMORY_LOCATION_NO_REPLY, true, 255u },
};

static const DiagSpecialSpec *diag_find_special_spec(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(s_diag_special_specs) / sizeof(s_diag_special_specs[0]));
         i++) {
        if (strcmp(name, s_diag_special_specs[i].name) == 0) {
            return &s_diag_special_specs[i];
        }
    }
    return NULL;
}

static void cmd_special_list(void)
{
#ifndef DALI_HOST_BUILD
    printf("special names:\r\n");
    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(s_diag_special_specs) / sizeof(s_diag_special_specs[0]));
         i++) {
        const DiagSpecialSpec *spec = &s_diag_special_specs[i];
        const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
        printf("  %s", spec->name);
        if (spec->needs_param) {
            printf(" <0-%u>", (unsigned)spec->max_param);
        }
        if (cmd != NULL && cmd->send_twice) {
            printf(" [send twice]");
        }
        if (cmd != NULL && cmd->response_kind != DALI_RESP_NONE) {
            printf(" [waits reply]");
        }
        printf("\r\n");
    }
#endif
}

static void diag_print_special_response(const DiagSpecialSpec *spec,
                                        const DaliFrame *reply)
{
#ifndef DALI_HOST_BUILD
    if (spec == NULL || reply == NULL) {
        return;
    }

    DaliParsedResponse parsed;
    DaliError err = dali_parse_command_response(spec->id, reply, &parsed);
    if (err != DALI_OK) {
        printf("%s: malformed reply\r\n", spec->name);
        return;
    }

    switch (parsed.kind) {
        case DALI_RESP_YES_NO:
            printf("%s: %s (0x%02X)\r\n",
                   spec->name,
                   parsed.yes ? "yes" : "no",
                   (unsigned)parsed.raw);
            break;

        case DALI_RESP_UINT8:
        case DALI_RESP_MEMORY_BYTE:
            printf("%s: %u (0x%02X)\r\n",
                   spec->name,
                   (unsigned)parsed.value,
                   (unsigned)parsed.raw);
            break;

        default:
            printf("%s: 0x%02X\r\n", spec->name, (unsigned)parsed.raw);
            break;
    }
#else
    (void)spec;
    (void)reply;
#endif
}

static void cmd_special(const char *args)
{
#ifndef DALI_HOST_BUILD
    char special_text[32] = {0};
    char param_text[16] = {0};
    char extra[2] = {0};
    int parsed = sscanf(args, "%31s %15s %1s",
                        special_text,
                        param_text,
                        extra);

    if (parsed < 1 || parsed > 2) {
        printf("usage: special <name> [param]\r\n");
        printf("       special-list\r\n");
        return;
    }

    const DiagSpecialSpec *spec = diag_find_special_spec(special_text);
    if (spec == NULL) {
        printf("special: unknown command '%s'\r\n", special_text);
        printf("use special-list\r\n");
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (parsed != 2 || !diag_parse_uint8_token(param_text, spec->max_param, &param)) {
            printf("usage: special %s <0-%u>\r\n",
                   spec->name,
                   (unsigned)spec->max_param);
            return;
        }
    } else if (parsed != 1) {
        printf("usage: special %s\r\n", spec->name);
        return;
    }

    const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
    if (cmd == NULL || cmd->frame_kind != DALI_CMD_FRAME_SPECIAL) {
        printf("%s: ERR %d\r\n", spec->name, (int)DALI_ERR_INVALID);
        return;
    }

    DaliFrame frame;
    DaliFrame reply = {0u, 0u};
    DaliError err = dali_build_special(spec->id, param, &frame);
    if (err == DALI_OK) {
        bool needs_reply = cmd->response_kind != DALI_RESP_NONE;
        err = diag_sched_sync(&frame, needs_reply, needs_reply ? 1u : 0u,
                              cmd->send_twice, needs_reply ? &reply : NULL);
    }

    if (err == DALI_OK && cmd->response_kind != DALI_RESP_NONE) {
        diag_print_special_response(spec, &reply);
    } else if (err == DALI_OK) {
        printf("%s: OK\r\n", spec->name);
    } else if (err == DALI_ERR_TIMEOUT) {
        printf("%s: timeout\r\n", spec->name);
    } else {
        printf("%s: ERR %d\r\n", spec->name, (int)err);
    }
#else
    (void)args;
#endif
}

typedef struct {
    const char   *name;
    DaliCommandId id;
    bool          needs_param;
    uint8_t       max_param;
    bool          uses_dtr0;
} DiagConfigSpec;

static const DiagConfigSpec s_diag_config_specs[] = {
    { "reset",                  DALI_CMD_RESET,                     false, 0u,             false },
    { "store-actual-dtr0",      DALI_CMD_STORE_ACTUAL_LEVEL_DTR0,   false, 0u,             false },
    { "save-persistent",        DALI_CMD_SAVE_PERSISTENT_VARIABLES, false, 0u,             false },
    { "set-operating-mode-dtr0", DALI_CMD_SET_OPERATING_MODE_DTR0,  false, 0u,             true  },
    { "reset-memory-dtr0",      DALI_CMD_RESET_MEMORY_BANK_DTR0,    false, 0u,             true  },
    { "identify-device",        DALI_CMD_IDENTIFY_DEVICE,           false, 0u,             false },
    { "set-max-dtr0",           DALI_CMD_SET_MAX_LEVEL_DTR0,        false, 0u,             true  },
    { "set-min-dtr0",           DALI_CMD_SET_MIN_LEVEL_DTR0,        false, 0u,             true  },
    { "set-failure-dtr0",       DALI_CMD_SET_SYSTEM_FAILURE_LEVEL_DTR0, false, 0u,         true  },
    { "set-power-on-dtr0",      DALI_CMD_SET_POWER_ON_LEVEL_DTR0,   false, 0u,             true  },
    { "set-fade-time-dtr0",     DALI_CMD_SET_FADE_TIME_DTR0,        false, 0u,             true  },
    { "set-fade-rate-dtr0",     DALI_CMD_SET_FADE_RATE_DTR0,        false, 0u,             true  },
    { "set-extended-fade-dtr0", DALI_CMD_SET_EXTENDED_FADE_TIME_DTR0, false, 0u,           true  },
    { "set-scene",              DALI_CMD_SET_SCENE,                 true,  DALI_MAX_SCENE, true  },
    { "remove-scene",           DALI_CMD_REMOVE_FROM_SCENE,         true,  DALI_MAX_SCENE, false },
    { "add-group",              DALI_CMD_ADD_TO_GROUP,              true,  DALI_MAX_GROUP, false },
    { "remove-group",           DALI_CMD_REMOVE_FROM_GROUP,         true,  DALI_MAX_GROUP, false },
    { "set-short-address-dtr0", DALI_CMD_SET_SHORT_ADDRESS_DTR0,    false, 0u,             true  },
    { "enable-write-memory",    DALI_CMD_ENABLE_WRITE_MEMORY,       false, 0u,             false },
};

static const DiagConfigSpec *diag_find_config_spec(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(s_diag_config_specs) / sizeof(s_diag_config_specs[0]));
         i++) {
        if (strcmp(name, s_diag_config_specs[i].name) == 0) {
            return &s_diag_config_specs[i];
        }
    }
    return NULL;
}

static void cmd_config_list(void)
{
#ifndef DALI_HOST_BUILD
    printf("config names:\r\n");
    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(s_diag_config_specs) / sizeof(s_diag_config_specs[0]));
         i++) {
        const DiagConfigSpec *spec = &s_diag_config_specs[i];
        printf("  %s", spec->name);
        if (spec->needs_param) {
            printf(" <0-%u>", (unsigned)spec->max_param);
        }
        if (spec->uses_dtr0) {
            printf(" [uses DTR0]");
        }
        printf("\r\n");
    }
#endif
}

static void cmd_config(const char *args)
{
#ifndef DALI_HOST_BUILD
    char target_text[16] = {0};
    char config_text[40] = {0};
    char param_text[16] = {0};
    char extra[2] = {0};
    int parsed = sscanf(args, "%15s %39s %15s %1s",
                        target_text,
                        config_text,
                        param_text,
                        extra);

    if (parsed < 2 || parsed > 3) {
        printf("usage: config <addr|sN|gN|b> <config-name> [param]\r\n");
        printf("       config-list\r\n");
        return;
    }

    DaliTarget target;
    if (!diag_parse_target_token(target_text, &target)) {
        printf("usage: config <addr|sN|gN|b> <config-name> [param]\r\n");
        return;
    }

    const DiagConfigSpec *spec = diag_find_config_spec(config_text);
    if (spec == NULL) {
        printf("config: unknown config '%s'\r\n", config_text);
        printf("use config-list\r\n");
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (parsed != 3 || !diag_parse_uint8_token(param_text, spec->max_param, &param)) {
            printf("usage: config <addr|sN|gN|b> %s <0-%u>\r\n",
                   spec->name,
                   (unsigned)spec->max_param);
            return;
        }
    } else if (parsed != 2) {
        printf("usage: config <addr|sN|gN|b> %s\r\n", spec->name);
        return;
    }

    if (target.type != DALI_ADDR_SHORT) {
        printf("config: group/broadcast target may affect multiple devices\r\n");
    }
    if (spec->uses_dtr0) {
        printf("config: using current DTR0 value\r\n");
    }

    DaliFrame frame;
    DaliError err = dali_control_build_config(target, spec->id, param, &frame);
    const DaliCommandInfo *cmd = dali_command_lookup(spec->id);
    if (err == DALI_OK && cmd != NULL) {
        err = diag_send_no_reply(&frame, cmd->send_twice);
    }
    diag_print_tx_result(spec->name, err);
#else
    (void)args;
#endif
}

static void cmd_config_dtr0(const char *args)
{
#ifndef DALI_HOST_BUILD
    char target_text[16] = {0};
    char config_text[40] = {0};
    char dtr0_text[16] = {0};
    char param_text[16] = {0};
    char extra[2] = {0};
    int parsed = sscanf(args, "%15s %39s %15s %15s %1s",
                        target_text,
                        config_text,
                        dtr0_text,
                        param_text,
                        extra);

    if (parsed < 3 || parsed > 4) {
        printf("usage: config-dtr0 <addr|sN|gN|b> <config-name> <0-255> [param]\r\n");
        printf("       config-list\r\n");
        return;
    }

    DaliTarget target;
    if (!diag_parse_target_token(target_text, &target)) {
        printf("usage: config-dtr0 <addr|sN|gN|b> <config-name> <0-255> [param]\r\n");
        return;
    }

    const DiagConfigSpec *spec = diag_find_config_spec(config_text);
    if (spec == NULL) {
        printf("config-dtr0: unknown config '%s'\r\n", config_text);
        printf("use config-list\r\n");
        return;
    }
    if (!spec->uses_dtr0 || !dali_control_config_uses_dtr0(spec->id)) {
        printf("config-dtr0: '%s' does not consume DTR0\r\n", spec->name);
        return;
    }

    uint8_t dtr0_value = 0u;
    if (!diag_parse_uint8_token(dtr0_text, 255u, &dtr0_value)) {
        printf("usage: config-dtr0 <addr|sN|gN|b> %s <0-255>",
               spec->name);
        if (spec->needs_param) {
            printf(" <0-%u>", (unsigned)spec->max_param);
        }
        printf("\r\n");
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (parsed != 4 || !diag_parse_uint8_token(param_text, spec->max_param, &param)) {
            printf("usage: config-dtr0 <addr|sN|gN|b> %s <0-255> <0-%u>\r\n",
                   spec->name,
                   (unsigned)spec->max_param);
            return;
        }
    } else if (parsed != 3) {
        printf("usage: config-dtr0 <addr|sN|gN|b> %s <0-255>\r\n", spec->name);
        return;
    }

    if (target.type != DALI_ADDR_SHORT) {
        printf("config-dtr0: group/broadcast target may affect multiple devices\r\n");
    }

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
            {
                .frame        = dtr_frame,
                .needs_reply  = false,
                .send_twice   = false,
                .retries_left = 0u,
            },
            {
                .frame        = config_frame,
                .needs_reply  = false,
                .send_twice   = cmd->send_twice,
                .retries_left = 0u,
            },
        },
        .step_count = 2u,
        .on_complete = NULL,
        .cb_ctx = NULL,
    };

    uint8_t failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
    err = diag_sched_sequence_sync(&seq, NULL, &failed_step);
    if (err == DALI_OK) {
        printf("%s: OK\r\n", spec->name);
    } else if (failed_step != DALI_SEQUENCE_NO_FAILED_STEP) {
        printf("%s: ERR %d at sequence step %u\r\n",
               spec->name,
               (int)err,
               (unsigned)failed_step);
    } else {
        printf("%s: ERR %d\r\n", spec->name, (int)err);
    }
#else
    (void)args;
#endif
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

static void cmd_instances(const char *args)
{
#ifndef DALI_HOST_BUILD
    uint8_t addr;

    if (!diag_parse_short_addr_arg(args, &addr)) {
        printf("usage: instances <addr>\r\n");
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
#else
    (void)args;
#endif
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

static void cmd_sensor(const char *args)
{
#ifndef DALI_HOST_BUILD
    char subcommand[16] = {0};
    char addr_text[16] = {0};
    char instance_text[16] = {0};
    char extra[2] = {0};
    int parsed = sscanf(args, "%15s %15s %15s %1s",
                        subcommand,
                        addr_text,
                        instance_text,
                        extra);
    uint8_t addr;
    uint8_t selected_instance = 0u;
    bool has_selected_instance = false;

    if ((parsed != 2 && parsed != 3) || strcmp(subcommand, "poll") != 0 ||
        !diag_parse_uint8_token(addr_text, DALI_MAX_SHORT_ADDRESS, &addr)) {
        printf("usage: sensor poll <addr> [instance]\r\n");
        return;
    }
    if (parsed == 3) {
        if (!diag_parse_uint8_token(instance_text,
                                    DALI_MAX_INSTANCE,
                                    &selected_instance)) {
            printf("usage: sensor poll <addr> [instance]\r\n");
            return;
        }
        has_selected_instance = true;
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
#else
    (void)args;
#endif
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

static void cmd_capture(const char *args)
{
#ifndef DALI_HOST_BUILD
    char action[16] = {0};
    char extra[2] = {0};
    if (sscanf(args, "%15s %1s", action, extra) != 1) {
        printf("usage: capture start|stop|clear|status|export\r\n");
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
        printf("usage: capture start|stop|clear|status|export\r\n");
    }
#else
    (void)args;
#endif
}

static void cmd_find(const char *args)
{
#ifndef DALI_HOST_BUILD
    char subcommand[16] = {0};
    unsigned seconds = DIAG_FIND_SWITCH_DEFAULT_SECONDS;
    char extra[2] = {0};
    int parsed = sscanf(args, "%15s %u %1s", subcommand, &seconds, extra);

    if ((parsed != 1 && parsed != 2) ||
        strcmp(subcommand, "switches") != 0 ||
        seconds == 0u ||
        seconds > DIAG_FIND_SWITCH_MAX_SECONDS) {
        printf("usage: find switches [1-%u seconds]\r\n",
               (unsigned)DIAG_FIND_SWITCH_MAX_SECONDS);
        return;
    }

    diag_events_reset();
    diag_switch_mappings_reset();

    printf("find switches: listening for %u seconds; double-press DALI-2 switches or trigger legacy coupler actions.\r\n",
           seconds);

    TickType_t start = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(seconds * 1000u);
    DaliInputEventRecord record;

    while ((xTaskGetTickCount() - start) < duration) {
        while (diag_event_pop(&record)) {
            diag_print_event_record("event: ", &record);
            if (dali_event_is_switch_mapping_candidate(&record.event)) {
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
        if (dali_event_is_switch_mapping_candidate(&record.event)) {
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
#else
    (void)args;
#endif
}

typedef struct {
    bool detailed;
} DiagDiscoveryPrintCtx;

static void diag_discovery_found_cb(uint8_t addr,
                                    const DaliDiscoveryDeviceInfo *device,
                                    void *ctx)
{
    DiagDiscoveryPrintCtx *print_ctx = (DiagDiscoveryPrintCtx *)ctx;
    if (device == NULL || !device->has_status) {
        return;
    }

    if (print_ctx != NULL && print_ctx->detailed) {
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
        printf("Device %2u: present (status=0x%02X)\r\n",
               (unsigned)addr,
               (unsigned)device->status);
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
    DaliDiscoveryInventory inventory;

    if (!diag_inventory_snapshot(&inventory)) {
        printf("inventory: empty; run discover first\r\n");
        return;
    }
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *entry =
            dali_discovery_inventory_get(&inventory, addr);
        if (entry != NULL && entry->present && entry->has_status) {
            const char *kind = entry->has_device_type
                ? dali_discovery_device_type_name(entry->device_type)
                : "unknown-type";
            printf("%02u: %s, status=0x%02X", (unsigned)addr, kind, (unsigned)entry->status);
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

static void cmd_commission(const char *args)
{
#ifndef DALI_HOST_BUILD
    char mode[16] = {0};
    char first_text[16] = {0};
    char max_text[16] = {0};
    char extra[2] = {0};
    int parsed = sscanf(args, "%15s %15s %15s %1s",
                        mode,
                        first_text,
                        max_text,
                        extra);

    if (parsed < 1 || parsed > 3 || strcmp(mode, "unaddressed") != 0) {
        printf("usage: commission unaddressed [first-addr] [max-devices]\r\n");
        return;
    }

    uint8_t first_address = 0u;
    uint8_t max_devices = 0u;
    if (parsed >= 2 &&
        !diag_parse_uint8_token(first_text, DALI_MAX_SHORT_ADDRESS, &first_address)) {
        printf("usage: commission unaddressed [0-%u] [0-%u]\r\n",
               (unsigned)DALI_MAX_SHORT_ADDRESS,
               (unsigned)DALI_SHORT_ADDRESS_COUNT);
        return;
    }
    if (parsed == 3 &&
        !diag_parse_uint8_token(max_text, DALI_SHORT_ADDRESS_COUNT, &max_devices)) {
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
#else
    (void)args;
#endif
}

static void cmd_export(const char *args)
{
#ifndef DALI_HOST_BUILD
    char what[16] = {0};
    char extra[2] = {0};
    DaliDiscoveryInventory inventory;
    bool has_inventory;
    DiagSwitchMapping mappings[DIAG_SWITCH_MAPPING_MAX];
    uint8_t mapping_count;

    if (sscanf(args, "%15s %1s", what, extra) != 1 ||
        strcmp(what, "inventory") != 0) {
        printf("usage: export inventory\r\n");
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
            if (entry->has_input_device) {
                printf(", \"kind\": \"input_device\"");
                if (entry->has_instance_count) {
                    printf(", \"instance_count\": %u",
                           (unsigned)entry->instance_count);
                }
            } else if (entry->has_device_type) {
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
#else
    (void)args;
#endif
}

static void cmd_bus(const char *args)
{
#ifndef DALI_HOST_BUILD
    char action[16] = {0};
    char extra[2] = {0};
    if (sscanf(args, "%15s %1s", action, extra) != 1 ||
        strcmp(action, "check") != 0) {
        printf("usage: bus check\r\n");
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
#else
    (void)args;
#endif
}

static void cmd_smoke(const char *args)
{
#ifndef DALI_HOST_BUILD
    uint8_t addr;
    if (!diag_parse_short_addr_arg(args, &addr)) {
        printf("usage: smoke <addr>\r\n");
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
#else
    (void)args;
#endif
}

static void cmd_identify(const char *args)
{
#ifndef DALI_HOST_BUILD
    uint8_t addr;
    DaliTarget target;
    DaliFrame max_frame;
    DaliFrame min_frame;

    if (!diag_parse_uint8_token(args, DALI_MAX_SHORT_ADDRESS, &addr)) {
        printf("usage: identify <addr>\r\n");
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
#else
    (void)args;
#endif
}

static void cmd_help(void)
{
#ifndef DALI_HOST_BUILD
    printf("commands:\r\n");
    printf("  stats\r\n");
    printf("  bus check\r\n");
    printf("  capture start|stop|clear|status|export\r\n");
    printf("  trace on|off\r\n");
    printf("  read\r\n");
    printf("  rxdebug\r\n");
    printf("  reset\r\n");
    printf("  raw <hex> len=<n> [wait]\r\n");
    printf("  dtr <0|1|2> <0-255>\r\n");
    printf("  special <name> [param]\r\n");
    printf("  special-list\r\n");
    printf("  level <addr|sN|gN|b> <0-%u>\r\n", (unsigned)DALI_DAPC_MAX_LEVEL);
    printf("  off <addr|sN|gN|b>\r\n");
    printf("  up <addr|sN|gN|b>\r\n");
    printf("  down <addr|sN|gN|b>\r\n");
    printf("  step-up <addr|sN|gN|b>\r\n");
    printf("  step-down <addr|sN|gN|b>\r\n");
    printf("  step-off <addr|sN|gN|b>\r\n");
    printf("  on-step <addr|sN|gN|b>\r\n");
    printf("  dapc-seq <addr|sN|gN|b>\r\n");
    printf("  last <addr|sN|gN|b>\r\n");
    printf("  scene <addr|sN|gN|b> <0-%u>\r\n", (unsigned)DALI_MAX_SCENE);
    printf("  max <addr|sN|gN|b>\r\n");
    printf("  min <addr|sN|gN|b>\r\n");
    printf("  status <addr|sN|gN|b>\r\n");
    printf("  query <addr|sN|gN|b> [query-name] [param]\r\n");
    printf("  query-list\r\n");
    printf("  config <addr|sN|gN|b> <config-name> [param]\r\n");
    printf("  config-dtr0 <addr|sN|gN|b> <config-name> <0-255> [param]\r\n");
    printf("  config-list\r\n");
    printf("  scan\r\n");
    printf("  discover\r\n");
    printf("  inventory\r\n");
    printf("  commission unaddressed [first-addr] [max-devices]\r\n");
    printf("  instances <addr>\r\n");
    printf("  sensor poll <addr> [instance]\r\n");
    printf("  smoke <addr>\r\n");
    printf("  events\r\n");
    printf("  find switches [seconds]\r\n");
    printf("  export inventory\r\n");
    printf("  identify <addr>\r\n");
#endif
}

static void dispatch(char *line)
{
#ifndef DALI_HOST_BUILD
    /* Strip trailing whitespace */
    size_t len = strlen(line);
    while (len > 0u && (line[len - 1u] == '\r' || line[len - 1u] == '\n' ||
                        line[len - 1u] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0u) { return; }

    printf("> %s\r\n", line);

    if (strcmp(line, "help") == 0) {
        cmd_help();
    } else if (strcmp(line, "stats") == 0) {
        cmd_stats();
    } else if (strncmp(line, "bus ", 4) == 0) {
        cmd_bus(line + 4);
    } else if (strncmp(line, "capture ", 8) == 0) {
        cmd_capture(line + 8);
    } else if (strncmp(line, "trace ", 6) == 0) {
        cmd_trace(line + 6);
    } else if (strcmp(line, "read") == 0) {
        cmd_read();
    } else if (strcmp(line, "rxdebug") == 0) {
        cmd_rxdebug();
    } else if (strcmp(line, "reset") == 0) {
        cmd_reset();
    } else if (strcmp(line, "query-list") == 0) {
        cmd_query_list();
    } else if (strcmp(line, "special-list") == 0) {
        cmd_special_list();
    } else if (strcmp(line, "config-list") == 0) {
        cmd_config_list();
    } else if (strncmp(line, "raw ", 4) == 0) {
        cmd_raw(line + 4);
    } else if (strncmp(line, "dtr ", 4) == 0) {
        cmd_dtr(line + 4);
    } else if (strncmp(line, "level ", 6) == 0) {
        cmd_level(line + 6);
    } else if (strncmp(line, "off ", 4) == 0) {
        cmd_off(line + 4);
    } else if (strncmp(line, "up ", 3) == 0) {
        cmd_target_frame(line + 3, "up", dali_control_build_up);
    } else if (strncmp(line, "down ", 5) == 0) {
        cmd_target_frame(line + 5, "down", dali_control_build_down);
    } else if (strncmp(line, "step-up ", 8) == 0) {
        cmd_target_frame(line + 8, "step-up", dali_control_build_step_up);
    } else if (strncmp(line, "step-down ", 10) == 0) {
        cmd_target_frame(line + 10, "step-down", dali_control_build_step_down);
    } else if (strncmp(line, "step-off ", 9) == 0) {
        cmd_target_frame(line + 9, "step-off", dali_control_build_step_down_and_off);
    } else if (strncmp(line, "on-step ", 8) == 0) {
        cmd_target_frame(line + 8, "on-step", dali_control_build_on_and_step_up);
    } else if (strncmp(line, "dapc-seq ", 9) == 0) {
        cmd_target_frame(line + 9, "dapc-seq", dali_control_build_enable_dapc_sequence);
    } else if (strncmp(line, "last ", 5) == 0) {
        cmd_target_frame(line + 5, "last", dali_control_build_go_to_last_active_level);
    } else if (strncmp(line, "scene ", 6) == 0) {
        cmd_scene(line + 6);
    } else if (strncmp(line, "max ", 4) == 0) {
        cmd_recall(line + 4, true);
    } else if (strncmp(line, "min ", 4) == 0) {
        cmd_recall(line + 4, false);
    } else if (strncmp(line, "status ", 7) == 0) {
        cmd_status(line + 7);
    } else if (strcmp(line, "scan") == 0) {
        cmd_scan();
    } else if (strcmp(line, "discover") == 0) {
        cmd_discover();
    } else if (strcmp(line, "inventory") == 0) {
        cmd_inventory();
    } else if (strncmp(line, "commission ", 11) == 0) {
        cmd_commission(line + 11);
    } else if (strncmp(line, "instances ", 10) == 0) {
        cmd_instances(line + 10);
    } else if (strncmp(line, "sensor ", 7) == 0) {
        cmd_sensor(line + 7);
    } else if (strncmp(line, "smoke ", 6) == 0) {
        cmd_smoke(line + 6);
    } else if (strcmp(line, "events") == 0) {
        cmd_events();
    } else if (strncmp(line, "find ", 5) == 0) {
        cmd_find(line + 5);
    } else if (strncmp(line, "export ", 7) == 0) {
        cmd_export(line + 7);
    } else if (strncmp(line, "identify ", 9) == 0) {
        cmd_identify(line + 9);
    } else if (strncmp(line, "query ", 6) == 0) {
        cmd_query(line + 6);
    } else if (strncmp(line, "special ", 8) == 0) {
        cmd_special(line + 8);
    } else if (strncmp(line, "config-dtr0 ", 12) == 0) {
        cmd_config_dtr0(line + 12);
    } else if (strncmp(line, "config ", 7) == 0) {
        cmd_config(line + 7);
    } else {
        printf("unknown command: %s\r\n", line);
        printf("type 'help' for commands\r\n");
    }
#else
    (void)line;
#endif
}

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
