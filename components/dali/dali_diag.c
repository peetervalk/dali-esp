#include "dali_diag.h"
#include "dali_phy.h"
#include "dali_scheduler.h"
#include "dali_protocol.h"

#ifndef DALI_HOST_BUILD
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

typedef struct {
    TaskHandle_t waiting_task;
    DaliError    result;
    DaliFrame    reply;
    bool         has_reply;
} DiagSyncCtx;

static void diag_sync_cb(DaliError result, const DaliFrame *reply, void *cb_ctx)
{
    DiagSyncCtx *ctx = (DiagSyncCtx *)cb_ctx;
    ctx->result    = result;
    ctx->has_reply = (reply != NULL);
    if (reply != NULL) {
        ctx->reply = *reply;
    }
    xTaskNotifyGive(ctx->waiting_task);
}

/*
 * Enqueue frame, wait up to 200 ms for completion.
 * reply_out may be NULL when the reply data is not needed.
 */
static DaliError diag_sched_sync(const DaliFrame *frame, bool needs_reply,
                                  DaliFrame *reply_out)
{
    DiagSyncCtx ctx = {
        .waiting_task = xTaskGetCurrentTaskHandle(),
        .result       = DALI_ERR_TIMEOUT,
        .has_reply    = false,
        .reply        = {0u, 0u},
    };
    DaliTransaction txn = {
        .frame        = *frame,
        .needs_reply  = needs_reply,
        .send_twice   = false,
        .retries_left = 1u,
        .on_complete  = diag_sync_cb,
        .cb_ctx       = &ctx,
    };
    DaliError err = dali_sched_enqueue(&txn);
    if (err != DALI_OK) {
        return err;
    }
    /* Block until the DALI task fires the callback, or 200 ms timeout. */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200u));
    if (reply_out != NULL && ctx.has_reply) {
        *reply_out = ctx.reply;
    }
    return ctx.result;
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
    printf("ISR overruns:     %" PRIu32 "\r\n", g_dali_stats.isr_overruns);
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
    printf("reset OK\r\n");
#endif
}

static void cmd_raw(const char *args)
{
#ifndef DALI_HOST_BUILD
    unsigned long hex_val = 0;
    int bit_len = 0;
    if (sscanf(args, "%lx len=%d", &hex_val, &bit_len) != 2 ||
        bit_len < 1 || bit_len > 24) {
        printf("usage: raw <hex> len=<bits>\r\n");
        return;
    }
    DaliFrame f;
    f.data       = (uint32_t)hex_val;
    f.bit_length = (uint8_t)bit_len;
    DaliError err = dali_phy_tx(&f);
    if (err == DALI_OK) {
        printf("TX: OK\r\n");
    } else {
        printf("TX: ERR %d\r\n", (int)err);
    }
#else
    (void)args;
#endif
}

static void cmd_scan(void)
{
#ifndef DALI_HOST_BUILD
    uint8_t found = 0u;
    printf("Scanning addresses 0-63...\r\n");
    for (uint8_t addr = 0u; addr < 64u; addr++) {
        DaliFrame f     = dali_cmd_query_status(addr);
        DaliFrame reply = {0u, 0u};
        DaliError err   = diag_sched_sync(&f, true, &reply);
        if (err == DALI_OK) {
            printf("Device %2u: present (status=0x%02X)\r\n",
                   (unsigned)addr, (unsigned)(reply.data & 0xFFu));
            found++;
        }
    }
    printf("Scan complete: %u device(s) found.\r\n", (unsigned)found);
#endif
}

static void cmd_query(const char *args)
{
#ifndef DALI_HOST_BUILD
    unsigned addr_val;
    if (sscanf(args, "%u", &addr_val) != 1 || addr_val > 63u) {
        printf("usage: query <addr>  (0-63)\r\n");
        return;
    }
    uint8_t addr    = (uint8_t)addr_val;
    DaliFrame f     = dali_cmd_query_status(addr);
    DaliFrame reply = {0u, 0u};
    DaliError err   = diag_sched_sync(&f, true, &reply);
    if (err == DALI_OK) {
        DaliStatus s;
        dali_parse_status((uint8_t)(reply.data & 0xFFu), &s);
        printf("Device %u status (0x%02X):\r\n",
               (unsigned)addr, (unsigned)(reply.data & 0xFFu));
        printf("  Ballast failure:       %s\r\n", s.ballast_failure       ? "YES" : "no");
        printf("  Lamp failure:          %s\r\n", s.lamp_failure          ? "YES" : "no");
        printf("  Lamp arc power on:     %s\r\n", s.lamp_arc_power_on     ? "YES" : "no");
        printf("  Limit error:           %s\r\n", s.limit_error           ? "YES" : "no");
        printf("  Fade running:          %s\r\n", s.fade_running          ? "YES" : "no");
        printf("  Reset state:           %s\r\n", s.reset_state           ? "YES" : "no");
        printf("  Missing short address: %s\r\n", s.missing_short_address ? "YES" : "no");
        printf("  Power failure:         %s\r\n", s.power_failure         ? "YES" : "no");
    } else if (err == DALI_ERR_TIMEOUT) {
        printf("Device %u: no response (not present or bus error)\r\n", (unsigned)addr);
    } else {
        printf("Device %u: error %d\r\n", (unsigned)addr, (int)err);
    }
#else
    (void)args;
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

    if (strcmp(line, "stats") == 0) {
        cmd_stats();
    } else if (strncmp(line, "trace ", 6) == 0) {
        cmd_trace(line + 6);
    } else if (strcmp(line, "reset") == 0) {
        cmd_reset();
    } else if (strncmp(line, "raw ", 4) == 0) {
        cmd_raw(line + 4);
    } else if (strcmp(line, "scan") == 0) {
        cmd_scan();
    } else if (strncmp(line, "query ", 6) == 0) {
        cmd_query(line + 6);
    } else {
        printf("unknown command: %s\r\n", line);
        printf("commands: stats, trace on|off, reset, raw <hex> len=<n>, scan, query <addr>\r\n");
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
    printf("\r\nDALI-2 diagnostic shell. Type 'stats' for counters.\r\n> ");

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
