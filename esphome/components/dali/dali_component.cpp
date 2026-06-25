#include "dali_component.h"
#include "dali_scan.h"
#include "esphome/core/log.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <string>

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

extern "C" __attribute__((weak))
const DaliDispatchEntry *dali_headless_get_table(uint8_t *out_count)
{
    *out_count = 0;
    return nullptr;
}

static const DaliDispatchEntry *s_dispatch_table = nullptr;
static uint8_t                  s_dispatch_count  = 0;
static DaliDispatchToggleState  s_toggle_state    = {};
static DaliInputEventQueue      s_event_queue;

static std::atomic<bool> s_deferred_query_pending_{false};

/* ── Light registry ──────────────────────────────────────────────────────── */

static constexpr uint8_t MAX_LIGHT_ENTITIES = 32u;

struct LightEntry {
    uint8_t       target_type;
    uint8_t       target_address;
    uint16_t      member_groups;  /* bitmask: bit N = this entity is in group N */
    DaliBusLight *light;
};

static LightEntry s_light_registry[MAX_LIGHT_ENTITIES];
static uint8_t    s_light_count = 0u;

static void notify_lights(const DaliDispatchResult *res)
{
    if (!res->has_state) {
        if (res->matched)
            s_deferred_query_pending_.store(true, std::memory_order_release);
        return;
    }
    for (uint8_t i = 0u; i < s_light_count; i++) {
        LightEntry &e = s_light_registry[i];
        bool direct = (e.target_type == (uint8_t)res->target.type) &&
                      (e.target_address == res->target.address ||
                       res->target.type == DALI_ADDR_BROADCAST);
        bool via_group = (res->target.type == DALI_ADDR_GROUP) &&
                         (e.target_type == (uint8_t)DALI_ADDR_SHORT) &&
                         ((e.member_groups >> res->target.address) & 1u);
        if (direct || via_group) e.light->mark_state_from_bus(res->is_on, res->level);
    }
}

static void on_level_query_reply(DaliError result, const DaliFrame *reply, void *ctx)
{
    LightEntry *e     = static_cast<LightEntry *>(ctx);
    if (result != DALI_OK || reply == nullptr) {
        ESP_LOGD(TAG, "query actual level failed: light target type=%u addr=%u result=%d",
                 (unsigned)e->target_type, (unsigned)e->target_address, (int)result);
        return;
    }
    uint8_t     level = (uint8_t)(reply->data & 0xFFu);
    bool        is_on = (level != 0u);
    ESP_LOGD(TAG, "query actual level reply: light target type=%u addr=%u level=%u",
             (unsigned)e->target_type, (unsigned)e->target_address, (unsigned)level);
    e->light->mark_state_from_bus(is_on, level);
    DaliTarget output;
    output.type    = static_cast<DaliAddressType>(e->target_type);
    output.address = e->target_address;
    dali_dispatch_seed_toggle(&s_toggle_state, output, is_on);
}

/* ── Bus monitor (Core 1 → Core 0) ──────────────────────────────────────── */

static char              s_bus_monitor_str[48] = {};
static std::atomic<bool> s_bus_monitor_dirty_{false};

/* ── Diag level query result (Core 1 → Core 0) ──────────────────────────── */

static char              s_diag_level_str[24] = {};
static std::atomic<bool> s_diag_level_dirty_{false};

static void on_diag_refresh_reply(DaliError result, const DaliFrame *reply, void * /*ctx*/)
{
    if (result != DALI_OK || reply == nullptr) {
        strncpy(s_diag_level_str, "Level: no reply", sizeof(s_diag_level_str) - 1u);
        s_diag_level_str[sizeof(s_diag_level_str) - 1u] = '\0';
    } else {
        uint8_t level = (uint8_t)(reply->data & 0xFFu);
        if (level == 0u)
            snprintf(s_diag_level_str, sizeof(s_diag_level_str), "Level: 0 (off)");
        else
            snprintf(s_diag_level_str, sizeof(s_diag_level_str), "Level: %u", (unsigned)level);
    }
    s_diag_level_dirty_.store(true, std::memory_order_release);
}

/* ── Find-couplers recording (Core 1 writes, Core 0 reads) ──────────────── */

struct CouplerFrame {
    uint8_t frame_kind;
    uint8_t addr_kind;
    uint8_t address;
    uint8_t event_code;
};
static constexpr uint8_t    MAX_COUPLER_FRAMES = 32u;
static CouplerFrame          s_coupler_frames[MAX_COUPLER_FRAMES];
static std::atomic<uint8_t>  s_coupler_count_{0};
static std::atomic<bool>     s_find_couplers_active_{false};

/* ── Scan result pending (Core 1 → Core 0 via scan_done_ gate) ──────────── */

static char s_scan_result_str[128]  = {};
static char s_scan_yaml_str[2048]   = {};

/* ── Coupler group mask (set on Core 0 after coupler drain, read on Core 1) */

static std::atomic<uint16_t> s_coupler_group_mask_{0u};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static const char *opcode_name(uint8_t op)
{
    switch (op) {
        case 0x00u: return "off";
        case 0x01u: return "up";
        case 0x02u: return "down";
        case 0x03u: return "step-up";
        case 0x04u: return "step-down";
        case 0x05u: return "max";
        case 0x06u: return "min";
        case 0x07u: return "step-off";
        case 0x08u: return "on-step";
        case 0x0Au: return "go-last";
        default:
            if (op >= 0x10u && op <= 0x1Fu) return "scene";
            return "cmd";
    }
}

static void format_target(char *buf, size_t len, DaliTarget target)
{
    const char *pfx = target.type == DALI_ADDR_GROUP ? "g"
                    : target.type == DALI_ADDR_BROADCAST ? "bc"
                                                        : "a";
    if (target.type == DALI_ADDR_BROADCAST)
        snprintf(buf, len, "bc");
    else
        snprintf(buf, len, "%s%u", pfx, (unsigned)target.address);
}

static void format_event(char *buf, size_t len, const DaliInputEvent *e)
{
    const char *pfx = (e->address_kind == DALI_EVENT_ADDRESS_GROUP)     ? "g"
                    : (e->address_kind == DALI_EVENT_ADDRESS_BROADCAST)  ? "bc"
                                                                         : "a";
    if (e->frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT && !e->address_selector) {
        if (e->address_kind == DALI_EVENT_ADDRESS_BROADCAST)
            snprintf(buf, len, "bc dapc %u", (unsigned)e->event_code);
        else
            snprintf(buf, len, "%s%u dapc %u", pfx, (unsigned)e->address,
                     (unsigned)e->event_code);
        return;
    }
    if (e->address_kind == DALI_EVENT_ADDRESS_BROADCAST) {
        snprintf(buf, len, "bc %s", opcode_name(e->event_code));
    } else {
        snprintf(buf, len, "%s%u %s", pfx, (unsigned)e->address,
                 opcode_name(e->event_code));
    }
}

/* ── Unsolicited-RX callback (runs on Core 1, in the DALI task) ─────────── */

static void on_dali_unsolicited(const DaliFrame *frame, void * /*ctx*/)
{
    DaliInputEvent event;
    if (dali_event_parse_frame(frame, &event) != DALI_OK) return;

    /* Bus monitor: format and signal Core 0. */
    format_event(s_bus_monitor_str, sizeof(s_bus_monitor_str), &event);
    s_bus_monitor_dirty_.store(true, std::memory_order_release);
    ESP_LOGD(TAG, "rx %s", s_bus_monitor_str);

    /* Find-couplers: record unique frames while the window is open. */
    if (s_find_couplers_active_.load(std::memory_order_relaxed)) {
        uint8_t cnt = s_coupler_count_.load(std::memory_order_relaxed);
        bool dup = false;
        for (uint8_t i = 0u; i < cnt; i++) {
            if (s_coupler_frames[i].frame_kind == (uint8_t)event.frame_kind &&
                s_coupler_frames[i].addr_kind  == (uint8_t)event.address_kind &&
                s_coupler_frames[i].address    == event.address &&
                s_coupler_frames[i].event_code == event.event_code) {
                dup = true;
                break;
            }
        }
        if (!dup && cnt < MAX_COUPLER_FRAMES) {
            s_coupler_frames[cnt].frame_kind  = (uint8_t)event.frame_kind;
            s_coupler_frames[cnt].addr_kind   = (uint8_t)event.address_kind;
            s_coupler_frames[cnt].address     = event.address;
            s_coupler_frames[cnt].event_code  = event.event_code;
            /* Release ensures all field writes are visible before the count. */
            s_coupler_count_.store(cnt + 1u, std::memory_order_release);
        }
    }

    /* Push to event queue for headless dispatch. */
    DaliInputEventRecord rec;
    rec.event        = event;
    rec.timestamp_us = 0u;
    dali_event_queue_push(&s_event_queue, &rec);
}

/* ── DALI task (Core 1) ──────────────────────────────────────────────────── */

static void dali_task(void *)
{
    const TickType_t delay = pdMS_TO_TICKS(1);
    for (;;) {
        dali_phy_rx_process();
        dali_sched_run();

        /* Always drain the event queue to prevent overflow.
         * Only dispatch if a headless table is loaded. */
        DaliInputEventRecord rec;
        while (dali_event_queue_pop(&s_event_queue, &rec)) {
            if (s_dispatch_table != nullptr && s_dispatch_count > 0u) {
                char event_str[48];
                format_event(event_str, sizeof(event_str), &rec.event);
                DaliDispatchResult result = {};
                DaliError err = dali_dispatch(s_dispatch_table, s_dispatch_count,
                                              &rec.event, &s_toggle_state, &result);
                if (err != DALI_OK) {
                    ESP_LOGD(TAG, "dispatch err %d (fk=%d ak=%d a=%d op=0x%02x)",
                             (int)err,
                             (int)rec.event.frame_kind,
                             (int)rec.event.address_kind,
                             (int)rec.event.address,
                             (int)rec.event.event_code);
                } else if (result.matched) {
                    char tgt[8];
                    format_target(tgt, sizeof(tgt), result.target);
                    if (result.has_state) {
                        ESP_LOGD(TAG, "dispatch observed %s -> %s %s level=%u",
                                 event_str, tgt,
                                 result.is_on ? "on" : "off",
                                 (unsigned)result.level);
                    } else {
                        ESP_LOGD(TAG, "dispatch observed %s -> %s state=unknown",
                                 event_str, tgt);
                    }
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

    xTaskCreatePinnedToCore(dali_task, "dali", 4096, nullptr, 10, nullptr, 1);

    if (scan_status_) scan_status_->publish_state("Idle");

    ESP_LOGI(TAG, "DALI initialized (TX GPIO%d, RX GPIO%d)", tx_pin_, rx_pin_);
}

void DaliComponent::loop()
{
    /* ── Bus state from snooping / query replies ── */
    for (uint8_t i = 0u; i < s_light_count; i++)
        s_light_registry[i].light->apply_bus_state();

    /* ── Boot query ── */
    if (!boot_query_done_) {
        boot_query_done_ = true;
        start_refresh();
    }

    /* ── Periodic poll ── */
    if (poll_interval_s_ > 0u) {
        uint32_t now = millis();
        if ((uint32_t)(now - last_poll_ms_) >= poll_interval_s_ * 1000u) {
            last_poll_ms_ = now;
            start_refresh();
        }
    }

    /* ── Deferred query after dim/scene ── */
    if (s_deferred_query_pending_.load(std::memory_order_acquire)) {
        s_deferred_query_pending_.store(false, std::memory_order_relaxed);
        deferred_query_armed_  = true;
        deferred_query_arm_ms_ = millis();
        ESP_LOGD(TAG, "deferred level query armed");
    }
    if (deferred_query_armed_ &&
        (uint32_t)(millis() - deferred_query_arm_ms_) >= 600u) {
        deferred_query_armed_ = false;
        ESP_LOGD(TAG, "deferred level query firing");
        start_refresh();
    }

    /* ── Bus monitor ── */
    if (bus_monitor_ && s_bus_monitor_dirty_.load(std::memory_order_acquire)) {
        s_bus_monitor_dirty_.store(false, std::memory_order_relaxed);
        bus_monitor_->publish_state(s_bus_monitor_str);
    }

    /* ── Diag level query result ── */
    if (scan_status_ && s_diag_level_dirty_.load(std::memory_order_acquire)) {
        s_diag_level_dirty_.store(false, std::memory_order_relaxed);
        scan_status_->publish_state(s_diag_level_str);
    }

    /* ── Identify blink ── */
    if (identify_active_) {
        uint32_t now = millis();
        if ((uint32_t)(now - identify_last_ms_) >= 500u) {
            identify_last_ms_ = now;
            identify_phase_   = !identify_phase_;
            DaliTarget t;
            t.type    = DALI_ADDR_SHORT;
            t.address = diag_address_;
            if (identify_phase_) dali_control_recall_max(t);
            else                 dali_control_recall_min(t);
        }
        if ((uint32_t)(millis() - identify_start_ms_) >= 10000u)
            identify_active_ = false;
    }

    /* ── Find-couplers timer ── */
    /* Overflow-safe elapsed check: underflows to a large value before end_ms_
     * passes, wraps to a small value after — so < 0x80000000u means elapsed. */
    if (s_find_couplers_active_.load(std::memory_order_relaxed) &&
        (uint32_t)(millis() - find_couplers_end_ms_) < 0x80000000u) {
        /* Timer elapsed: stop recording, arm one-tick drain. */
        s_find_couplers_active_.store(false, std::memory_order_release);
        find_couplers_collect_ = true;
        if (scan_status_) scan_status_->publish_state("Coupler scan done");
    } else if (find_couplers_collect_) {
        /* One tick after clearing active — Core 1 has definitely seen the flag. */
        find_couplers_collect_ = false;
        uint8_t cnt = s_coupler_count_.load(std::memory_order_acquire);

        /* Build coupler group bitmask from captured frames. */
        uint16_t gmask = 0u;
        for (uint8_t i = 0u; i < cnt; i++) {
            if (s_coupler_frames[i].addr_kind == (uint8_t)DALI_EVENT_ADDRESS_GROUP)
                gmask |= (uint16_t)(1u << s_coupler_frames[i].address);
        }
        s_coupler_group_mask_.store(gmask, std::memory_order_release);

        if (couplers_result_) {
            if (cnt == 0u) {
                couplers_result_->publish_state("None");
            } else {
                char buf[128] = {};
                for (uint8_t i = 0u; i < cnt; i++) {
                    const CouplerFrame &cf = s_coupler_frames[i];
                    char entry[24];
                    const char *pfx = (cf.addr_kind == (uint8_t)DALI_EVENT_ADDRESS_GROUP) ? "g"
                                    : (cf.addr_kind == (uint8_t)DALI_EVENT_ADDRESS_BROADCAST) ? "bc"
                                                                                               : "a";
                    if (cf.addr_kind == (uint8_t)DALI_EVENT_ADDRESS_BROADCAST)
                        snprintf(entry, sizeof(entry), "bc/%s ", opcode_name(cf.event_code));
                    else
                        snprintf(entry, sizeof(entry), "%s%u/%s ", pfx,
                                 (unsigned)cf.address, opcode_name(cf.event_code));
                    strncat(buf, entry, sizeof(buf) - strlen(buf) - 1u);
                }
                couplers_result_->publish_state(buf);
            }
        }
    }

    /* ── Scan complete ── */
    if (!scan_done_.load()) return;
    scan_done_.store(false);
    scan_running_.store(false);

    /* Scan result summary (written from Core 1 before scan_done_ release). */
    if (scan_result_ && s_scan_result_str[0]) {
        scan_result_->publish_state(s_scan_result_str);
        s_scan_result_str[0] = '\0';
    }
    if (yaml_result_ && s_scan_yaml_str[0]) {
        yaml_result_->publish_state(s_scan_yaml_str);
        s_scan_yaml_str[0] = '\0';
    }

    if (scan_status_) {
        if (scan_success_) {
            scan_status_->publish_state(
                "Found " + std::to_string(scan_count_.load()) + " devices");
        } else {
            scan_status_->publish_state("Scan error");
        }
    }
}

/* ── Public methods ──────────────────────────────────────────────────────── */

void DaliComponent::register_light(uint8_t type, uint8_t address,
                                   uint16_t member_groups, DaliBusLight *light)
{
    if (s_light_count < MAX_LIGHT_ENTITIES) {
        s_light_registry[s_light_count++] = { type, address, member_groups, light };
    } else {
        ESP_LOGW(TAG, "light registry full — increase MAX_LIGHT_ENTITIES");
    }
}

void DaliComponent::start_refresh()
{
    for (uint8_t i = 0u; i < s_light_count; i++) {
        LightEntry *e  = &s_light_registry[i];
        uint8_t     qa = e->light->get_query_address();
        if (qa == 0xFFu) continue;
        ESP_LOGD(TAG, "query actual level: light target type=%u addr=%u via short %u",
                 (unsigned)e->target_type, (unsigned)e->target_address, (unsigned)qa);
        DaliTarget qt;
        qt.type    = DALI_ADDR_SHORT;
        qt.address = qa;
        dali_control_query(qt, DALI_CMD_QUERY_ACTUAL_LEVEL, 0u, on_level_query_reply, e);
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

void DaliComponent::start_identify()
{
    if (identify_active_) return;
    ESP_LOGI(TAG, "Identify: short address %u (10 s)", (unsigned)diag_address_);
    identify_active_   = true;
    identify_phase_    = true;
    identify_start_ms_ = millis();
    identify_last_ms_  = millis() - 500u;  // fire immediately on first loop tick
}

void DaliComponent::start_find_couplers()
{
    if (s_find_couplers_active_.load()) {
        ESP_LOGW(TAG, "Coupler scan already running");
        return;
    }
    ESP_LOGI(TAG, "Find couplers: listening for 30 s");
    s_coupler_count_.store(0u, std::memory_order_relaxed);
    find_couplers_collect_  = false;
    find_couplers_end_ms_   = millis() + 30000u;
    s_find_couplers_active_.store(true, std::memory_order_release);
    if (scan_status_) scan_status_->publish_state("Listening...");
}

void DaliComponent::send_diag_on()
{
    DaliTarget t{ DALI_ADDR_SHORT, diag_address_ };
    dali_control_go_to_last_active_level(t);
}

void DaliComponent::send_diag_off()
{
    DaliTarget t{ DALI_ADDR_SHORT, diag_address_ };
    dali_control_off(t);
}

void DaliComponent::send_diag_max()
{
    DaliTarget t{ DALI_ADDR_SHORT, diag_address_ };
    dali_control_recall_max(t);
}

void DaliComponent::send_diag_min()
{
    DaliTarget t{ DALI_ADDR_SHORT, diag_address_ };
    dali_control_recall_min(t);
}

void DaliComponent::send_diag_refresh()
{
    DaliTarget qt{ DALI_ADDR_SHORT, diag_address_ };
    dali_control_query(qt, DALI_CMD_QUERY_ACTUAL_LEVEL, 0u, on_diag_refresh_reply, nullptr);
}

void DaliComponent::set_scan_result_pending(const char *summary)
{
    strncpy(s_scan_result_str, summary, sizeof(s_scan_result_str) - 1u);
    s_scan_result_str[sizeof(s_scan_result_str) - 1u] = '\0';
}

void DaliComponent::set_scan_yaml_pending(const char *yaml)
{
    strncpy(s_scan_yaml_str, yaml, sizeof(s_scan_yaml_str) - 1u);
    s_scan_yaml_str[sizeof(s_scan_yaml_str) - 1u] = '\0';
}

uint16_t DaliComponent::get_coupler_group_mask() const
{
    return s_coupler_group_mask_.load(std::memory_order_acquire);
}

void DaliComponent::on_scan_complete(uint8_t count, bool success)
{
    scan_count_   = count;
    scan_success_ = success;
    scan_done_.store(true, std::memory_order_release);
}

}  // namespace dali
}  // namespace esphome
