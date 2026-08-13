/*
 * dali_diag.c — UART0 front end for the diagnostic shell
 *
 * All this does is move bytes. It reads a byte at a time off UART0, hands each
 * to dali_shell_feed_byte(), and writes whatever the shell emits back to
 * stdout. Every verb, every workflow, and the blocking transport they run on
 * belong to components/dali/dali_shell.c, so a second front end on a different
 * transport presents exactly the same shell rather than an approximation of it.
 *
 * The session is attached once and never released: on a serial console the
 * operator already has physical access to the bus, so there is nothing an idle
 * timeout would protect and no second front end competing for the session.
 */

#include "dali_diag.h"

#include <stdio.h>

#include "dali_shell.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DALI-DIAG";

#define DIAG_TASK_STACK   8192u
#define DIAG_TASK_PRIORITY  2u
#define DIAG_UART_NUM       0u   /* UART0 = default serial monitor port */
#define DIAG_UART_BAUD 115200u
#define DIAG_UART_RX_BUFFER_SIZE 1024u
#define DIAG_UART_TX_BUFFER_SIZE 1024u

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

static void diag_write(void *ctx, const char *text)
{
    (void)ctx;
    fputs(text, stdout);
}

static void diag_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Diagnostic CLI ready (115200 baud)");
    dali_shell_write_banner();
    dali_shell_write_prompt();
    fflush(stdout);

    for (;;) {
        uint8_t ch;
        int n = uart_read_bytes(DIAG_UART_NUM, &ch, 1, pdMS_TO_TICKS(10));
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(100u));
            continue;
        }
        if (n == 0) { continue; }

        if (dali_shell_feed_byte(ch)) {
            dali_shell_write_prompt();
            fflush(stdout);
        }
    }
}

DaliError dali_diag_init(void)
{
    DaliError err = diag_uart_init();
    if (err != DALI_OK) {
        return err;
    }

    /*
     * The serial console runs with no policy restrictions: `commission` and
     * `reset` are exactly what this front end exists for, and the operator
     * holding the cable can already reach the bus directly.
     */
    DaliShellSession session = {
        .out       = { .write = diag_write, .ctx = NULL },
        .transport = *dali_shell_device_transport(),
        .hooks     = { 0 },  /* nothing to coordinate with in this build */
        .policy    = DALI_SHELL_ALLOW_ALL,
        .aborted   = NULL,   /* a UART peer never goes away */
        .abort_ctx = NULL,
    };

    err = dali_shell_attach(&session);
    if (err != DALI_OK) {
        ESP_LOGE(TAG, "dali_shell_attach failed: %d", (int)err);
        return err;
    }

    xTaskCreate(diag_task, "dali_diag", DIAG_TASK_STACK, NULL,
                DIAG_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Diagnostic task started");

    return DALI_OK;
}
