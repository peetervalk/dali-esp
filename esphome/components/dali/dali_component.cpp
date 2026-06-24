#include "dali_component.h"
#include "dali_scan.h"
#include "light/dali_light_output.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "../../../components/dali/dali_phy.h"
#include "../../../components/dali/dali_scheduler.h"
#include "../../../components/dali/dali_control.h"
#include "../../../components/dali/dali_event.h"
#include "../../../components/dali/dali_dispatch.h"
#include "../../../components/dali/dali_protocol.h"
}

namespace esphome {
namespace dali {

static const char *TAG = "dali";

/* ── Headless dispatch state ─────────────────────────────────────────────── */

/*
 * Weak default: no dispatch table.  dali_headless.cpp provides the strong
 * override with the installation-specific mapping entries.
 */
extern "C" __attribute__((weak))
const DaliDispatchEntry *dali_headless_get_table(uint8_t *out_count)
{
    *out_count = 0;
    return nullptr;
}

static const DaliDispatchEntry *s_dispatch_table = nullptr;
static uint8_t                  s_dispatch_count  = 0;
static DaliDispatchToggleState  s_toggle_state    = {};

/*
 * Raw input-event queue — populated by the unsolicited-RX callback (still in
 * the DALI task) and drained after dali_sched_run() each tick.  Both sides
 * run on the same task (Core 1), so no mutex is needed.
 */
static DaliInputEventQueue s_event_queue;

/* ── Light registry ──────────────────────────────────────────────────────── */

static constexpr uint8_t MAX_LIGHT_ENTITIES = 32u;

struct LightEntry {
    uint8_t          target_type;    /* DaliAddressType cast to uint8_t */
    uint8_t          target_address;
    DaliLightOutput *light;
};

static LightEntry s_light_registry[MAX_LIGHT_ENTITIES];
static uint8_t    s_light_count = 0u;

static void notify_lights(const DaliDispatchResult *res)
{
    if (!res->has_state) return;
    for (uint8_t i = 0u; i < s_light_count; i++) {
        LightEntry &e = s_light_registry[i];
        bool match = (e.target_type == (uint8_t)res->target.type) &&
                     (e.target_address == res->target.address ||
                      res->target.type == DALI_ADDR_BROADCAST);
        if (match) e.light->mark_state_from_bus(res->is_on, res->level);
    }
}

static void on_level_query_reply(DaliError result, const DaliFrame *reply, void *ctx)
{
    if (result != DALI_OK || reply == nullptr) return;
    uint8_t level = (uint8_t)(reply->data & 0xFFu);
    static_cast<DaliLightOutput *>(ctx)->mark_state_from_bus(level != 0u, level);
}

static void on_dali_unsolicited(const DaliFrame *frame, void * /*ctx*/)
{
    DaliInputEvent event;
    if (dali_event_parse_frame(frame, &event) != DALI_OK) return;

    DaliInputEventRecord rec;
    rec.event        = event;
    rec.timestamp_us = 0; /* timestamp not critical for dispatch */
    dali_event_queue_push(&s_event_queue, &rec);
}

/* ── DALI task ───────────────────────────────────────────────────────────── */

static void dali_task(void *)
{
    const TickType_t delay = pdMS_TO_TICKS(1);
    for (;;) {
        dali_phy_rx_process();
        dali_sched_run();

        /* Drain event queue and dispatch — runs after dali_sched_run() so we
         * never call dali_sched_enqueue() re-entrantly from inside it. */
        if (s_dispatch_table != nullptr) {
            DaliInputEventRecord rec;
            while (dali_event_queue_pop(&s_event_queue, &rec)) {
                DaliDispatchResult result = {};
                DaliError err = dali_dispatch(s_dispatch_table, s_dispatch_count,
                                              &rec.event, &s_toggle_state, &result);
                if (err != DALI_OK) {
                    ESP_LOGD(TAG, "dispatch err %d (frame_kind=%d addr_kind=%d addr=%d op=0x%02x)",
                             (int)err,
                             (int)rec.event.frame_kind,
                             (int)rec.event.address_kind,
                             (int)rec.event.address,
                             (int)rec.event.event_code);
                }
                notify_lights(&result);
            }
        }

        vTaskDelay(delay);
    }
}

/* ── DaliComponent ───────────────────────────────────────────────────────── */

void DaliComponent::setup()
{
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

    dali_event_queue_init(&s_event_queue);
    dali_sched_set_event_callback(on_dali_unsolicited, nullptr);

    s_dispatch_table = dali_headless_get_table(&s_dispatch_count);
    if (s_dispatch_table != nullptr && s_dispatch_count > 0) {
        ESP_LOGI(TAG, "headless dispatch: %u entries loaded", (unsigned)s_dispatch_count);
    }

    // Pin to Core 1 (APP_CPU) so Wi-Fi / network stack on Core 0 doesn't
    // create scheduling pressure on the DALI bit-timing task.
    xTaskCreatePinnedToCore(dali_task, "dali", 4096, nullptr, 10, nullptr, 1);

    if (scan_status_) scan_status_->publish_state("Idle");

    ESP_LOGI(TAG, "DALI initialized (TX GPIO%d, RX GPIO%d)", tx_pin_, rx_pin_);
}

void DaliComponent::loop()
{
    /* Drain bus state updates from dispatch / query callbacks (Core 1 → Core 0). */
    for (uint8_t i = 0u; i < s_light_count; i++) {
        s_light_registry[i].light->apply_bus_state();
    }

    /* Issue boot queries once, after all entity setup has completed. */
    if (!boot_query_done_) {
        boot_query_done_ = true;
        start_refresh();
    }

    /* Periodic state poll. */
    if (poll_interval_s_ > 0u) {
        uint32_t now = millis();
        if ((uint32_t)(now - last_poll_ms_) >= poll_interval_s_ * 1000u) {
            last_poll_ms_ = now;
            start_refresh();
        }
    }

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

void DaliComponent::register_light(uint8_t type, uint8_t address, DaliLightOutput *light)
{
    if (s_light_count < MAX_LIGHT_ENTITIES) {
        s_light_registry[s_light_count++] = { type, address, light };
    } else {
        ESP_LOGW(TAG, "light registry full — increase MAX_LIGHT_ENTITIES");
    }
}

void DaliComponent::start_refresh()
{
    for (uint8_t i = 0u; i < s_light_count; i++) {
        DaliLightOutput *light = s_light_registry[i].light;
        uint8_t qa = light->get_query_address();
        if (qa == 0xFFu) continue;
        DaliTarget qt;
        qt.type    = DALI_ADDR_SHORT;
        qt.address = qa;
        dali_control_query(qt, DALI_CMD_QUERY_ACTUAL_LEVEL, 0u, on_level_query_reply, light);
    }
}

void DaliComponent::start_scan()
{
    if (scan_running_.exchange(true)) {
        ESP_LOGW(TAG, "Scan already in progress");
        return;
    }
    if (scan_status_) scan_status_->publish_state("Scanning...");
    dali_scan_start(this);
}

void DaliComponent::on_scan_complete(uint8_t count, bool success)
{
    scan_count_   = count;
    scan_success_ = success;
    scan_done_.store(true); // loop() picks this up on the next ESPHome tick
}

}  // namespace dali
}  // namespace esphome
