#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dali_phy.h"
#include "dali_diag.h"

static const char *TAG = "main";

/* GPIO assignments — update to match your mikroBUS adapter wiring */
#define DALI_TX_GPIO  18
#define DALI_RX_GPIO  19

void app_main(void)
{
    ESP_LOGI(TAG, "DALI-2 gateway starting");

    DaliError err = dali_phy_init(DALI_TX_GPIO, DALI_RX_GPIO);
    if (err != DALI_OK) {
        ESP_LOGE(TAG, "dali_phy_init failed: %d", (int)err);
        return;
    }

    dali_diag_init();

    ESP_LOGI(TAG, "Initialisation complete");
}
