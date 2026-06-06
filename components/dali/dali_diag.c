#include "dali_diag.h"
#include "dali_phy.h"
#include "dali_scheduler.h"
#include "dali_protocol.h"
#include "dali_control.h"
#include "dali_input_device.h"

#ifndef DALI_HOST_BUILD
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#endif

static const char *TAG = "DALI-DIAG";

#define DIAG_TASK_STACK   2048u
#define DIAG_TASK_PRIORITY  2u
#define DIAG_LINE_MAX      80u
#define DIAG_UART_NUM       0u   /* UART0 = default serial monitor port */

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
#define DIAG_INVENTORY_ADDR_COUNT 64u
#define DIAG_IDENTIFY_CYCLES 5u
#define DIAG_IDENTIFY_STEP_MS 1000u

typedef struct {
    TaskHandle_t waiting_task;
    DaliError    result;
    DaliFrame    reply;
    bool         has_reply;
    bool         in_use;
    bool         complete;
} DiagSyncCtx;

typedef struct {
    bool    present;
    uint8_t status;
} DiagInventoryEntry;

static DiagSyncCtx s_diag_sync_slots[DIAG_SYNC_SLOT_COUNT];
static portMUX_TYPE s_diag_sync_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_diag_state_mux = portMUX_INITIALIZER_UNLOCKED;
static DaliFrame s_last_rx_frame;
static uint32_t  s_last_rx_timestamp_us;
static uint32_t  s_last_rx_since_tx_us;
static bool      s_last_rx_has_since_tx;
static bool      s_has_last_rx_frame;
static DiagInventoryEntry s_inventory[DIAG_INVENTORY_ADDR_COUNT];
static bool      s_inventory_valid;

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
        if (!diag_parse_uint8_token(text + 1, 15u, &group)) {
            return false;
        }
        *out = (DaliTarget){ .type = DALI_ADDR_GROUP, .address = group };
        return true;
    }

    if (text[0] == 's') {
        uint8_t addr;
        if (!diag_parse_uint8_token(text + 1, 63u, &addr)) {
            return false;
        }
        *out = (DaliTarget){ .type = DALI_ADDR_SHORT, .address = addr };
        return true;
    }

    uint8_t addr;
    if (!diag_parse_uint8_token(text, 63u, &addr)) {
        return false;
    }
    *out = (DaliTarget){ .type = DALI_ADDR_SHORT, .address = addr };
    return true;
}

static void diag_inventory_reset(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    for (uint8_t i = 0u; i < DIAG_INVENTORY_ADDR_COUNT; i++) {
        s_inventory[i] = (DiagInventoryEntry){ .present = false, .status = 0u };
    }
    s_inventory_valid = false;
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

static void diag_inventory_store(uint8_t addr, uint8_t status)
{
    if (addr >= DIAG_INVENTORY_ADDR_COUNT) {
        return;
    }

    taskENTER_CRITICAL(&s_diag_state_mux);
    s_inventory[addr] = (DiagInventoryEntry){ .present = true, .status = status };
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static void diag_inventory_mark_valid(void)
{
    taskENTER_CRITICAL(&s_diag_state_mux);
    s_inventory_valid = true;
    taskEXIT_CRITICAL(&s_diag_state_mux);
}

static bool diag_inventory_get(uint8_t addr, DiagInventoryEntry *out)
{
    if (addr >= DIAG_INVENTORY_ADDR_COUNT || out == NULL) {
        return false;
    }

    bool valid;
    taskENTER_CRITICAL(&s_diag_state_mux);
    valid = s_inventory_valid;
    *out = s_inventory[addr];
    taskEXIT_CRITICAL(&s_diag_state_mux);
    return valid;
}

static void diag_trace_cb(const DaliSchedTraceEvent *event, void *cb_ctx)
{
    (void)cb_ctx;

    diag_store_last_rx(event);

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
    return diag_parse_uint8_token(addr_text, 63u, addr);
}

static void cmd_level(const char *args)
{
#ifndef DALI_HOST_BUILD
    char target_text[16] = {0};
    unsigned level = 0u;
    char extra[2] = {0};
    if (sscanf(args, "%15s %u %1s", target_text, &level, extra) != 2 ||
        level > 254u) {
        printf("usage: level <addr|sN|gN|b> <0-254>\r\n");
        return;
    }

    DaliTarget target;
    DaliFrame frame;
    DaliError err;
    if (!diag_parse_target_token(target_text, &target)) {
        printf("usage: level <addr|sN|gN|b> <0-254>\r\n");
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

typedef DaliError (*DiagInputQueryBuilder)(uint8_t addr,
                                           uint8_t instance,
                                           DaliFrame *out);

static DaliError diag_query_u8(const DaliFrame *frame, uint8_t *out)
{
    if (frame == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame reply = {0u, 0u};
    DaliError err = diag_sched_sync(frame, true, 1u, false, &reply);
    if (err != DALI_OK) {
        return err;
    }
    if (reply.bit_length != 8u) {
        return DALI_ERR_MALFORMED;
    }

    *out = (uint8_t)(reply.data & 0xFFu);
    return DALI_OK;
}

static DaliError diag_query_instance_u8(DiagInputQueryBuilder builder,
                                        uint8_t addr,
                                        uint8_t instance,
                                        uint8_t *out)
{
    if (builder == NULL || out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliError err = builder(addr, instance, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return diag_query_u8(&frame, out);
}

static void diag_query_instance_optional_fields(uint8_t addr,
                                                DaliInputInstanceInfo *info)
{
    if (info == NULL) {
        return;
    }

    uint8_t raw = 0u;
    if (diag_query_instance_u8(dali_input_build_query_instance_enabled,
                               addr, info->instance, &raw) == DALI_OK) {
        info->has_enabled = true;
        info->enabled = dali_is_yes(raw);
    }
    if (diag_query_instance_u8(dali_input_build_query_resolution,
                               addr, info->instance, &raw) == DALI_OK) {
        info->has_resolution = true;
        info->resolution = raw;
    }
    if (diag_query_instance_u8(dali_input_build_query_instance_status,
                               addr, info->instance, &raw) == DALI_OK) {
        info->has_status = true;
        info->status = raw;
    }
    if (diag_query_instance_u8(dali_input_build_query_instance_error,
                               addr, info->instance, &raw) == DALI_OK) {
        info->has_error = true;
        info->error = raw;
    }
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
    DaliFrame frame;
    uint8_t count_raw = 0u;

    if (!diag_parse_short_addr_arg(args, &addr)) {
        printf("usage: instances <addr>\r\n");
        return;
    }

    DaliError err = dali_input_build_query_number_of_instances(addr, &frame);
    if (err == DALI_OK) {
        err = diag_query_u8(&frame, &count_raw);
    }
    if (err == DALI_ERR_TIMEOUT) {
        printf("instances: timeout querying device %u\r\n", (unsigned)addr);
        return;
    }
    if (err != DALI_OK) {
        printf("instances: ERR %d\r\n", (int)err);
        return;
    }

    uint8_t count = count_raw;
    printf("Input device %u:\r\n", (unsigned)addr);
    if (count > DALI_INPUT_MAX_INSTANCES) {
        printf("  instances: %u (showing first %u)\r\n",
               (unsigned)count_raw,
               (unsigned)DALI_INPUT_MAX_INSTANCES);
        count = DALI_INPUT_MAX_INSTANCES;
    } else {
        printf("  instances: %u\r\n", (unsigned)count);
    }

    for (uint8_t instance = 0u; instance < count; instance++) {
        uint8_t type = 0u;
        err = diag_query_instance_u8(dali_input_build_query_instance_type,
                                     addr, instance, &type);
        if (err == DALI_ERR_TIMEOUT) {
            printf("  %2u: type timeout\r\n", (unsigned)instance);
            continue;
        }
        if (err != DALI_OK) {
            printf("  %2u: type ERR %d\r\n", (unsigned)instance, (int)err);
            continue;
        }

        DaliInputInstanceInfo info;
        err = dali_input_classify_instance(instance, type, &info);
        if (err != DALI_OK) {
            printf("  %2u: classify ERR %d\r\n", (unsigned)instance, (int)err);
            continue;
        }

        diag_query_instance_optional_fields(addr, &info);
        diag_print_input_instance(&info);
    }
#else
    (void)args;
#endif
}

static uint8_t diag_discover_bus(bool detailed)
{
    uint8_t found = 0u;

    diag_inventory_reset();
    printf("Scanning short addresses 0-63...\r\n");

    for (uint8_t addr = 0u; addr < 64u; addr++) {
        DaliTarget target = { .type = DALI_ADDR_SHORT, .address = addr };
        DaliFrame reply = {0u, 0u};
        DaliError err = diag_query_status(target, &reply);
        if (err == DALI_OK) {
            uint8_t raw = (uint8_t)(reply.data & 0xFFu);
            diag_inventory_store(addr, raw);
            found++;
            if (detailed) {
                printf("%02u: present, control-gear candidate, status=0x%02X\r\n",
                       (unsigned)addr,
                       (unsigned)raw);
            } else {
                printf("Device %2u: present (status=0x%02X)\r\n",
                       (unsigned)addr,
                       (unsigned)raw);
            }
        }
    }

    diag_inventory_mark_valid();
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

    for (uint8_t addr = 0u; addr < 64u; addr++) {
        DiagInventoryEntry entry;
        bool valid = diag_inventory_get(addr, &entry);
        if (!valid) {
            printf("inventory: empty; run discover first\r\n");
            return;
        }
        if (entry.present) {
            printf("%02u: present, control-gear candidate, status=0x%02X\r\n",
                   (unsigned)addr,
                   (unsigned)entry.status);
            found++;
        }
    }

    printf("Inventory: %u device(s)\r\n", (unsigned)found);
#endif
}

static void cmd_identify(const char *args)
{
#ifndef DALI_HOST_BUILD
    uint8_t addr;
    DaliTarget target;
    DaliFrame max_frame;
    DaliFrame min_frame;

    if (!diag_parse_uint8_token(args, 63u, &addr)) {
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
    printf("  trace on|off\r\n");
    printf("  read\r\n");
    printf("  reset\r\n");
    printf("  raw <hex> len=<n> [wait]\r\n");
    printf("  level <addr|sN|gN|b> <0-254>\r\n");
    printf("  off <addr|sN|gN|b>\r\n");
    printf("  max <addr|sN|gN|b>\r\n");
    printf("  min <addr|sN|gN|b>\r\n");
    printf("  status <addr|sN|gN|b>\r\n");
    printf("  scan\r\n");
    printf("  discover\r\n");
    printf("  inventory\r\n");
    printf("  instances <addr>\r\n");
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
    } else if (strncmp(line, "trace ", 6) == 0) {
        cmd_trace(line + 6);
    } else if (strcmp(line, "read") == 0) {
        cmd_read();
    } else if (strcmp(line, "reset") == 0) {
        cmd_reset();
    } else if (strncmp(line, "raw ", 4) == 0) {
        cmd_raw(line + 4);
    } else if (strncmp(line, "level ", 6) == 0) {
        cmd_level(line + 6);
    } else if (strncmp(line, "off ", 4) == 0) {
        cmd_off(line + 4);
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
    } else if (strncmp(line, "instances ", 10) == 0) {
        cmd_instances(line + 10);
    } else if (strncmp(line, "identify ", 9) == 0) {
        cmd_identify(line + 9);
    } else if (strncmp(line, "query ", 6) == 0) {
        cmd_status(line + 6);
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
        if (n <= 0) { continue; }

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
    diag_last_rx_reset();
    diag_inventory_reset();

    DaliError err = dali_sched_set_trace_callback(diag_trace_cb, NULL);
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
