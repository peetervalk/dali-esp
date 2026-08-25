#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dali_phy.h"
#include "dali_scheduler.h"
#include "dali_shell.h"
#include "dali_diag.h"

static const char *TAG = "main";

/* Datasheet-confirmed initial mikroBUS wiring:
 *   GPIO18 -> DALI-2 Click pin 2  Tx/RST
 *   GPIO19 -> DALI-2 Click pin 15 Rx/INT
 */
#define DALI_TX_GPIO  18
#define DALI_RX_GPIO  19

#if DALI_TX_GPIO == 16 || DALI_TX_GPIO == 17 || DALI_RX_GPIO == 16 || DALI_RX_GPIO == 17
#error "GPIO 16 and GPIO 17 are reserved for PSRAM on ESP32-WROVER-E"
#endif

/* DALI processing task — drives RX decode and scheduler state machine.
 * Higher priority than diag (2) and ESPHome/Wi-Fi tasks.
 * Pinning to Core 1 keeps Wi-Fi (Core 0) from causing timer jitter. */
#define DALI_TASK_STACK     4096u
#define DALI_TASK_PRIORITY    10u
#define DALI_TASK_CORE         1

static void dali_task(void *arg)
{
    (void)arg;
    TickType_t loop_delay_ticks = pdMS_TO_TICKS(1u);
    if (loop_delay_ticks == 0u) {
        loop_delay_ticks = 1u;
    }

    for (;;) {
        dali_phy_rx_process();
        dali_sched_run();
        vTaskDelay(loop_delay_ticks);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DALI-2 gateway starting");

    DaliError err = dali_phy_init(DALI_TX_GPIO, DALI_RX_GPIO);
    if (err != DALI_OK) {
        ESP_LOGE(TAG, "dali_phy_init failed: %d", (int)err);
        return;
    }

    err = dali_sched_init_device();
    if (err != DALI_OK) {
        ESP_LOGE(TAG, "dali_sched_init failed: %d", (int)err);
        return;
    }

    xTaskCreatePinnedToCore(dali_task, "dali", DALI_TASK_STACK, NULL,
                            DALI_TASK_PRIORITY, NULL, DALI_TASK_CORE);

    /* Shell state and its scheduler subscribers, before any front end attaches. */
    err = dali_shell_init();
    if (err != DALI_OK) {
        ESP_LOGE(TAG, "dali_shell_init failed: %d", (int)err);
        return;
    }

    err = dali_diag_init();
    if (err != DALI_OK) {
        ESP_LOGE(TAG, "dali_diag_init failed: %d", (int)err);
        return;
    }

    ESP_LOGI(TAG, "Initialization complete");
}
