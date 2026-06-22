#include "dali_component.h"
#include "dali_scan.h"
#include "esphome/core/log.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "../../../components/dali/dali_phy.h"
#include "../../../components/dali/dali_scheduler.h"
}

namespace esphome {
namespace dali {

static const char *TAG = "dali";

static void dali_task(void *) {
  const TickType_t delay = pdMS_TO_TICKS(1);
  for (;;) {
    dali_phy_rx_process();
    dali_sched_run();
    vTaskDelay(delay);
  }
}

void DaliComponent::setup() {
  DaliError err = dali_phy_init(tx_pin_, rx_pin_);
  if (err != DALI_OK) {
    ESP_LOGE(TAG, "dali_phy_init failed: %d", (int)err);
    mark_failed();
    return;
  }

  err = dali_sched_init_device();
  if (err != DALI_OK) {
    ESP_LOGE(TAG, "dali_sched_init failed: %d", (int)err);
    mark_failed();
    return;
  }

  // Pin to Core 1 (APP_CPU) so Wi-Fi / network stack on Core 0 doesn't
  // create scheduling pressure on the DALI bit-timing task.
  xTaskCreatePinnedToCore(dali_task, "dali", 4096, nullptr, 10, nullptr, 1);

  if (scan_status_) scan_status_->publish_state("Idle");

  ESP_LOGI(TAG, "DALI initialized (TX GPIO%d, RX GPIO%d)", tx_pin_, rx_pin_);
}

void DaliComponent::loop() {
  if (!scan_done_.load()) return;
  scan_done_.store(false);
  scan_running_.store(false);

  if (scan_status_) {
    if (scan_success_) {
      scan_status_->publish_state("Found " + std::to_string(scan_count_) + " devices");
    } else {
      scan_status_->publish_state("Scan error");
    }
  }
}

void DaliComponent::start_scan() {
  if (scan_running_.exchange(true)) {
    ESP_LOGW(TAG, "Scan already in progress");
    return;
  }
  if (scan_status_) scan_status_->publish_state("Scanning...");
  dali_scan_start(this);
}

void DaliComponent::on_scan_complete(uint8_t count, bool success) {
  scan_count_   = count;
  scan_success_ = success;
  scan_done_.store(true);  // loop() picks this up on the next ESPHome tick
}

}  // namespace dali
}  // namespace esphome
