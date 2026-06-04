#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dali_phy.h"
#include "dali_scheduler.h"
#include "dali_diag.h"

static const char *TAG = "main";

/* GPIO assignments — update to match your mikroBUS adapter wiring */
#define DALI_TX_GPIO  18
#define DALI_RX_GPIO  19

/* DALI processing task — drives RX decode and scheduler state machine.
 * Higher priority than diag (2) and ESPHome/Wi-Fi tasks.
 * Pinning to Core 1 keeps Wi-Fi (Core 0) from causing timer jitter. */
#define DALI_TASK_STACK     4096u
#define DALI_TASK_PRIORITY    10u
#define DALI_TASK_CORE         1

static void dali_task(void *arg)
{
    (void)arg;
    for (;;) {
        dali_phy_rx_process();
        dali_sched_run();
        vTaskDelay(pdMS_TO_TICKS(1u));
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

    dali_diag_init();

    ESP_LOGI(TAG, "Initialisation complete");
}
