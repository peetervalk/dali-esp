#include "dali_diag.h"
#include "dali_phy.h"

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
    } else {
        printf("unknown command: %s\r\n", line);
        printf("commands: stats, trace on|off, reset, raw <hex> len=<n>\r\n");
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
