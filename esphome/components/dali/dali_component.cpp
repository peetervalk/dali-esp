#include "dali_component.h"
#include "dali_scan.h"
#include "esphome/core/log.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <cctype>
#include <cstdlib>

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
#include "../../../components/dali/dali_input_device.h"
#include "../../../components/dali/dali_input_config.h"
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

/* ── Input sensor registry ───────────────────────────────────────────────── */

static constexpr uint8_t MAX_INPUT_SENSORS = 16u;

struct SensorEntry {
    DaliBusSensor *sensor;
    uint8_t        pending_msb; /* temp storage during 2-byte async read */
};

static SensorEntry s_sensor_registry[MAX_INPUT_SENSORS];
static uint8_t     s_sensor_count = 0u;

/* Async completion callbacks — run on Core 1 (DALI task). */

static void on_input_value_lsb(DaliError result, const DaliFrame *reply, void *ctx)
{
    SensorEntry *e = static_cast<SensorEntry *>(ctx);
    if (result != DALI_OK || reply == nullptr) return;
    uint8_t  lsb = (uint8_t)(reply->data & 0xFFu);
    uint16_t raw = ((uint16_t)e->pending_msb << 8) | lsb;
    e->sensor->mark_raw_value(raw);
}

static void on_input_value_msb(DaliError result, const DaliFrame *reply, void *ctx)
{
    SensorEntry *e = static_cast<SensorEntry *>(ctx);
    if (result != DALI_OK || reply == nullptr) return;
    e->pending_msb = (uint8_t)(reply->data & 0xFFu);
    DaliFrame frame;
    dali_build_instance_command(e->sensor->get_address(), e->sensor->get_instance(),
                                DALI_CMD_QUERY_INPUT_VALUE_LATCH, &frame);
    DaliTransaction txn = {};
    txn.frame       = frame;
    txn.needs_reply = true;
    txn.on_complete = on_input_value_lsb;
    txn.cb_ctx      = e;
    if (dali_sched_enqueue(&txn) != DALI_OK)
        ESP_LOGW(TAG, "sensor LSB enqueue failed; reading resumes next poll");
}

static void on_input_value_1byte(DaliError result, const DaliFrame *reply, void *ctx)
{
    SensorEntry *e = static_cast<SensorEntry *>(ctx);
    if (result != DALI_OK || reply == nullptr) return;
    e->sensor->mark_raw_value((uint16_t)(reply->data & 0xFFu));
}

static void enqueue_sensor_poll(SensorEntry *e, uint32_t now_ms)
{
    DaliFrame frame;
    dali_build_instance_command(e->sensor->get_address(), e->sensor->get_instance(),
                                DALI_CMD_QUERY_INPUT_VALUE, &frame);
    DaliTransaction txn = {};
    txn.frame       = frame;
    txn.needs_reply = true;
    txn.on_complete = (e->sensor->get_value_bytes() == 2u) ? on_input_value_msb
                                                            : on_input_value_1byte;
    txn.cb_ctx      = e;
    if (dali_sched_enqueue(&txn) == DALI_OK)
        e->sensor->set_last_poll_ms(now_ms);
}

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

/* ── Cross-core string handoff (Core 1 → Core 0) ────────────────────────── */
/* Shared spinlock protects all three string buffers below.
 * Write side: format to a local buffer, then memcpy + dirty=true under lock.
 * Read side:  memcpy to a local buffer + dirty=false under lock, then publish. */
static portMUX_TYPE s_string_mux = portMUX_INITIALIZER_UNLOCKED;

static char              s_bus_monitor_str[48] = {};
static std::atomic<bool> s_bus_monitor_dirty_{false};

/* ── Diag level query result (Core 1 → Core 0) ──────────────────────────── */

static char              s_diag_level_str[24] = {};
static std::atomic<bool> s_diag_level_dirty_{false};

static void on_diag_refresh_reply(DaliError result, const DaliFrame *reply, void * /*ctx*/)
{
    char tmp[sizeof(s_diag_level_str)];
    if (result != DALI_OK || reply == nullptr) {
        strncpy(tmp, "Level: no reply", sizeof(tmp) - 1u);
        tmp[sizeof(tmp) - 1u] = '\0';
    } else {
        uint8_t level = (uint8_t)(reply->data & 0xFFu);
        if (level == 0u)
            snprintf(tmp, sizeof(tmp), "Level: 0 (off)");
        else
            snprintf(tmp, sizeof(tmp), "Level: %u", (unsigned)level);
    }
    portENTER_CRITICAL(&s_string_mux);
    memcpy(s_diag_level_str, tmp, sizeof(s_diag_level_str));
    s_diag_level_dirty_.store(true, std::memory_order_relaxed);
    portEXIT_CRITICAL(&s_string_mux);
}

/* ── Command result (Core 1 → Core 0) ───────────────────────────────────── */

static char              s_cmd_result_str[64] = {};
static std::atomic<bool> s_cmd_result_dirty_{false};

static void set_cmd_result(const char *str)
{
    portENTER_CRITICAL(&s_string_mux);
    strncpy(s_cmd_result_str, str, sizeof(s_cmd_result_str) - 1u);
    s_cmd_result_str[sizeof(s_cmd_result_str) - 1u] = '\0';
    s_cmd_result_dirty_.store(true, std::memory_order_relaxed);
    portEXIT_CRITICAL(&s_string_mux);
}

static void on_cmd_query_reply(DaliError result, const DaliFrame *reply, void * /*ctx*/)
{
    if (result != DALI_OK || reply == nullptr) {
        set_cmd_result("no reply");
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u (0x%02X)", (unsigned)(reply->data & 0xFFu),
                 (unsigned)(reply->data & 0xFFu));
        set_cmd_result(buf);
    }
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

    /* Bus monitor: format locally, then copy under lock so Core 0 never sees a
     * partially-written buffer. Log from the local copy, not the shared buffer. */
    char monitor_copy[sizeof(s_bus_monitor_str)];
    format_event(monitor_copy, sizeof(monitor_copy), &event);
    portENTER_CRITICAL(&s_string_mux);
    memcpy(s_bus_monitor_str, monitor_copy, sizeof(s_bus_monitor_str));
    s_bus_monitor_dirty_.store(true, std::memory_order_relaxed);
    portEXIT_CRITICAL(&s_string_mux);
    ESP_LOGD(TAG, "rx %s", monitor_copy);

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

    /* ── Boot query (lights) ── */
    if (!boot_query_done_) {
        boot_query_done_ = true;
        start_refresh();
    }

    /* ── Periodic poll (lights) ── */
    if (poll_interval_s_ > 0u) {
        uint32_t now = millis();
        if ((uint32_t)(now - last_poll_ms_) >= poll_interval_s_ * 1000u) {
            last_poll_ms_ = now;
            start_refresh();
        }
    }

    /* ── Input sensor: boot query then periodic poll ── */
    {
        uint32_t now = millis();
        for (uint8_t i = 0u; i < s_sensor_count; i++) {
            SensorEntry &e = s_sensor_registry[i];
            bool due = !boot_sensor_query_done_ ||
                       (uint32_t)(now - e.sensor->get_last_poll_ms()) >=
                           e.sensor->get_poll_interval_s() * 1000u;
            if (due) enqueue_sensor_poll(&e, now);
        }
        boot_sensor_query_done_ = true;
    }

    /* ── Input sensor: publish dirty values ── */
    for (uint8_t i = 0u; i < s_sensor_count; i++)
        s_sensor_registry[i].sensor->apply_value();

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

    /* ── Command result ── */
    if (command_result_) {
        char tmp[sizeof(s_cmd_result_str)];
        bool dirty = false;
        portENTER_CRITICAL(&s_string_mux);
        dirty = s_cmd_result_dirty_.load(std::memory_order_relaxed);
        if (dirty) {
            memcpy(tmp, s_cmd_result_str, sizeof(tmp));
            s_cmd_result_dirty_.store(false, std::memory_order_relaxed);
        }
        portEXIT_CRITICAL(&s_string_mux);
        if (dirty) command_result_->publish_state(tmp);
    }

    /* ── Bus monitor ── */
    if (bus_monitor_) {
        char tmp[sizeof(s_bus_monitor_str)];
        bool dirty = false;
        portENTER_CRITICAL(&s_string_mux);
        dirty = s_bus_monitor_dirty_.load(std::memory_order_relaxed);
        if (dirty) {
            memcpy(tmp, s_bus_monitor_str, sizeof(tmp));
            s_bus_monitor_dirty_.store(false, std::memory_order_relaxed);
        }
        portEXIT_CRITICAL(&s_string_mux);
        if (dirty) bus_monitor_->publish_state(tmp);
    }

    /* ── Diag level query result ── */
    if (scan_status_) {
        char tmp[sizeof(s_diag_level_str)];
        bool dirty = false;
        portENTER_CRITICAL(&s_string_mux);
        dirty = s_diag_level_dirty_.load(std::memory_order_relaxed);
        if (dirty) {
            memcpy(tmp, s_diag_level_str, sizeof(tmp));
            s_diag_level_dirty_.store(false, std::memory_order_relaxed);
        }
        portEXIT_CRITICAL(&s_string_mux);
        if (dirty) scan_status_->publish_state(tmp);
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

/* ── execute_command ─────────────────────────────────────────────────────── */

/* Parse decimal integer in s into *out; return false if empty, non-numeric,
 * has trailing garbage, or is outside [lo, hi]. */
static bool parse_uint(const char *s, unsigned long lo, unsigned long hi,
                        unsigned long *out)
{
    if (!s || !*s) return false;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v < lo || v > hi) return false;
    *out = v;
    return true;
}

/* Parse "a0", "s3", "g5", "b" into a DaliTarget. Returns false on error. */
static bool parse_target(const char *tok, DaliTarget *out)
{
    if (!tok) return false;
    if (tok[0] == 'b' && tok[1] == '\0') {
        out->type    = DALI_ADDR_BROADCAST;
        out->address = 0u;
        return true;
    }
    const char *digits = nullptr;
    if ((tok[0] == 'a' || tok[0] == 's') && isdigit((unsigned char)tok[1])) {
        out->type = DALI_ADDR_SHORT;
        digits    = tok + 1;
    } else if (tok[0] == 'g' && isdigit((unsigned char)tok[1])) {
        out->type = DALI_ADDR_GROUP;
        digits    = tok + 1;
    } else {
        return false;
    }
    char *end;
    unsigned long val = strtoul(digits, &end, 10);
    if (*end != '\0') return false;
    uint8_t limit = (out->type == DALI_ADDR_GROUP) ? 15u : 63u;
    if (val > limit) return false;
    out->address = (uint8_t)val;
    return true;
}

/* Lookup table for gear queries (subset of DaliCommandId). */
struct QueryEntry { const char *name; DaliCommandId id; };
static const QueryEntry s_query_table[] = {
    { "actual-level",     DALI_CMD_QUERY_ACTUAL_LEVEL              },
    { "max-level",        DALI_CMD_QUERY_MAX_LEVEL                 },
    { "min-level",        DALI_CMD_QUERY_MIN_LEVEL                 },
    { "power-on-level",   DALI_CMD_QUERY_POWER_ON_LEVEL            },
    { "failure-level",    DALI_CMD_QUERY_SYSTEM_FAILURE_LEVEL      },
    { "status",           DALI_CMD_QUERY_STATUS                    },
    { "version",          DALI_CMD_QUERY_VERSION_NUMBER            },
    { "device-type",      DALI_CMD_QUERY_DEVICE_TYPE               },
    { "power-on-flag",    DALI_CMD_QUERY_LAMP_POWER_ON             },
    { "power-fail-flag",  DALI_CMD_QUERY_POWER_FAILURE             },
    { "groups-0-7",       DALI_CMD_QUERY_GROUPS_0_7                },
    { "groups-8-15",      DALI_CMD_QUERY_GROUPS_8_15               },
    { "physical-min",     DALI_CMD_QUERY_PHYSICAL_MINIMUM          },
    { "operating-mode",   DALI_CMD_QUERY_OPERATING_MODE            },
    { "fade",             DALI_CMD_QUERY_FADE_TIME_FADE_RATE       },
    { "content-dtr0",     DALI_CMD_QUERY_CONTENT_DTR0              },
    { "content-dtr1",     DALI_CMD_QUERY_CONTENT_DTR1              },
    { "content-dtr2",     DALI_CMD_QUERY_CONTENT_DTR2              },
};

/* Lookup table for 16-bit gear config commands (subset). */
struct ConfigEntry { const char *name; DaliCommandId id; bool uses_dtr0; };
static const ConfigEntry s_config_table[] = {
    { "reset",                DALI_CMD_RESET,                         false },
    { "set-max-dtr0",         DALI_CMD_SET_MAX_LEVEL_DTR0,            true  },
    { "set-min-dtr0",         DALI_CMD_SET_MIN_LEVEL_DTR0,            true  },
    { "set-power-on-dtr0",    DALI_CMD_SET_POWER_ON_LEVEL_DTR0,       true  },
    { "set-failure-dtr0",     DALI_CMD_SET_SYSTEM_FAILURE_LEVEL_DTR0, true  },
    { "set-fade-time-dtr0",   DALI_CMD_SET_FADE_TIME_DTR0,            true  },
    { "set-fade-rate-dtr0",   DALI_CMD_SET_FADE_RATE_DTR0,            true  },
    { "set-operating-mode",   DALI_CMD_SET_OPERATING_MODE_DTR0,       true  },
    { "add-group",            DALI_CMD_ADD_TO_GROUP,                   false },
    { "remove-group",         DALI_CMD_REMOVE_FROM_GROUP,              false },
    { "identify-device",      DALI_CMD_IDENTIFY_DEVICE,                false },
    { "save-persistent",      DALI_CMD_SAVE_PERSISTENT_VARIABLES,      false },
};

/* Lookup table for 24-bit instance config commands (send_twice; DTR0 optional). */
typedef DaliFrame (*IConfigBuilder)(uint8_t addr, uint8_t instance);
struct IConfigEntry { const char *name; IConfigBuilder builder; bool needs_dtr0; };
static const IConfigEntry s_iconfig_table[] = {
    { "set-hold-timer",    dali_input_occ_build_set_hold_timer,   true  },
    { "set-deadtime",      dali_input_occ_build_set_deadtime,     true  },
    { "set-hysteresis",    dali_input_build_set_hysteresis,       true  },
    { "set-report-timer",  dali_input_build_set_report_timer,     true  },
    { "set-deadtime-gen",  dali_input_build_set_deadtime_timer,   true  },
    { "enable-instance",   dali_input_build_enable_instance,      false },
    { "disable-instance",  dali_input_build_disable_instance,     false },
};

/* Adapters: dali_input_device.h uses DaliError+out-ptr; iquery table needs DaliFrame return. */
static DaliFrame s_qry_instance_type(uint8_t a, uint8_t i)    { DaliFrame f={}; dali_input_build_query_instance_type(a,i,&f);    return f; }
static DaliFrame s_qry_resolution(uint8_t a, uint8_t i)       { DaliFrame f={}; dali_input_build_query_resolution(a,i,&f);        return f; }
static DaliFrame s_qry_instance_status(uint8_t a, uint8_t i)  { DaliFrame f={}; dali_input_build_query_instance_status(a,i,&f);   return f; }
static DaliFrame s_qry_instance_enabled(uint8_t a, uint8_t i) { DaliFrame f={}; dali_input_build_query_instance_enabled(a,i,&f);  return f; }

/* Lookup table for 24-bit instance query commands (single send, expects reply). */
struct IQueryEntry { const char *name; IConfigBuilder builder; };
static const IQueryEntry s_iquery_table[] = {
    { "hold-timer",        dali_input_occ_build_query_hold_timer       },
    { "deadtime",          dali_input_occ_build_query_deadtime         },
    { "hysteresis",        dali_input_build_query_hysteresis           },
    { "deadtime-gen",      dali_input_build_query_deadtime_timer       },
    { "report-timer",      dali_input_build_query_report_timer         },
    { "instance-type",     s_qry_instance_type     },
    { "resolution",        s_qry_resolution        },
    { "instance-enabled",  s_qry_instance_enabled  },
    { "instance-status",   s_qry_instance_status   },
};

void DaliComponent::execute_command(const std::string &cmd_str)
{
    /* Tokenise into up to 5 tokens on whitespace. */
    char buf[96];
    strncpy(buf, cmd_str.c_str(), sizeof(buf) - 1u);
    buf[sizeof(buf) - 1u] = '\0';

    const char *tok[5] = {};
    uint8_t     ntok   = 0u;
    char       *p      = buf;
    while (*p && ntok < 5u) {
        while (*p == ' ') p++;
        if (!*p) break;
        tok[ntok++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    if (ntok == 0u) return;

    const char *verb = tok[0];

    /* ── Direct gear control ── */
    DaliTarget tgt{};
    if ((strcmp(verb, "off") == 0 || strcmp(verb, "max") == 0 ||
         strcmp(verb, "min") == 0) && ntok >= 2u) {
        if (!parse_target(tok[1], &tgt)) { set_cmd_result("bad target"); return; }
        if (strcmp(verb, "off") == 0) dali_control_off(tgt);
        else if (strcmp(verb, "max") == 0) dali_control_recall_max(tgt);
        else dali_control_recall_min(tgt);
        set_cmd_result("OK");
        return;
    }

    if (strcmp(verb, "level") == 0 && ntok >= 3u) {
        if (!parse_target(tok[1], &tgt)) { set_cmd_result("bad target"); return; }
        unsigned long lv;
        if (!parse_uint(tok[2], 0u, 254u, &lv)) { set_cmd_result("level 0-254"); return; }
        dali_control_set_level(tgt, (uint8_t)lv);
        set_cmd_result("OK");
        return;
    }

    /* ── Gear query ── */
    if (strcmp(verb, "query") == 0 && ntok >= 3u) {
        if (!parse_target(tok[1], &tgt)) { set_cmd_result("bad target"); return; }
        for (const auto &e : s_query_table) {
            if (strcmp(tok[2], e.name) == 0) {
                dali_control_query(tgt, e.id, 0u, on_cmd_query_reply, nullptr);
                return;
            }
        }
        set_cmd_result("unknown query");
        return;
    }

    /* ── Gear config ── */
    if (strcmp(verb, "config") == 0 && ntok >= 3u) {
        if (!parse_target(tok[1], &tgt)) { set_cmd_result("bad target"); return; }
        for (const auto &e : s_config_table) {
            if (strcmp(tok[2], e.name) == 0) {
                DaliError err;
                if (e.uses_dtr0 && ntok >= 4u) {
                    unsigned long dtr0v;
                    if (!parse_uint(tok[3], 0u, 255u, &dtr0v)) { set_cmd_result("bad dtr0 value"); return; }
                    err = dali_control_config_with_dtr0(tgt, e.id, (uint8_t)dtr0v, 0u);
                } else if (!e.uses_dtr0) {
                    unsigned long paramv = 0u;
                    if (ntok >= 4u && !parse_uint(tok[3], 0u, 255u, &paramv)) { set_cmd_result("bad param"); return; }
                    err = dali_control_config(tgt, e.id, (uint8_t)paramv);
                } else {
                    set_cmd_result("needs dtr0 value");
                    return;
                }
                set_cmd_result(err == DALI_OK ? "OK" : "err");
                return;
            }
        }
        set_cmd_result("unknown config");
        return;
    }

    /* ── Instance config: iconfig a<N>:<inst> <cmd> [<dtr0>] ── */
    if (strcmp(verb, "iconfig") == 0 && ntok >= 3u) {
        const char *at = tok[1];
        if (at[0] != 'a' && at[0] != 's') { set_cmd_result("iconfig: use a<N>:<inst>"); return; }
        char *colon = strchr(const_cast<char *>(at), ':');
        if (!colon) { set_cmd_result("iconfig: missing :<inst>"); return; }
        *colon      = '\0';
        unsigned long addrv, instv;
        if (!parse_uint(at + 1, 0u, 63u, &addrv)) { set_cmd_result("iconfig: addr 0-63"); return; }
        if (!parse_uint(colon + 1, 0u, 31u, &instv)) { set_cmd_result("iconfig: inst 0-31"); return; }
        uint8_t addr = (uint8_t)addrv;
        uint8_t inst = (uint8_t)instv;

        for (const auto &e : s_iconfig_table) {
            if (strcmp(tok[2], e.name) == 0) {
                if (e.needs_dtr0 && ntok < 4u) { set_cmd_result("needs dtr0 value"); return; }
                DaliFrame cfg_frame = e.builder(addr, inst);
                DaliSequence seq = {};
                if (e.needs_dtr0) {
                    unsigned long dtr0v;
                    if (!parse_uint(tok[3], 0u, 255u, &dtr0v)) { set_cmd_result("bad dtr0 value"); return; }
                    DaliFrame dtr_frame;
                    dali_control_build_dtr(DALI_DTR0, (uint8_t)dtr0v, &dtr_frame);
                    seq.steps[0] = { dtr_frame, false, false, 0u };
                    seq.steps[1] = { cfg_frame, false, true,  0u };
                    seq.step_count = 2u;
                } else {
                    seq.steps[0] = { cfg_frame, false, true, 0u };
                    seq.step_count = 1u;
                }
                DaliError err = dali_sched_enqueue_sequence(&seq);
                set_cmd_result(err == DALI_OK ? "OK" : "err");
                return;
            }
        }
        set_cmd_result("unknown iconfig cmd");
        return;
    }

    /* ── Instance query: iquery a<N>:<inst> <name> ── */
    if (strcmp(verb, "iquery") == 0 && ntok >= 3u) {
        const char *at = tok[1];
        if (at[0] != 'a' && at[0] != 's') { set_cmd_result("iquery: use a<N>:<inst>"); return; }
        char *colon = strchr(const_cast<char *>(at), ':');
        if (!colon) { set_cmd_result("iquery: missing :<inst>"); return; }
        *colon      = '\0';
        unsigned long addrv, instv;
        if (!parse_uint(at + 1, 0u, 63u, &addrv)) { set_cmd_result("iquery: addr 0-63"); return; }
        if (!parse_uint(colon + 1, 0u, 31u, &instv)) { set_cmd_result("iquery: inst 0-31"); return; }
        uint8_t addr = (uint8_t)addrv;
        uint8_t inst = (uint8_t)instv;

        for (const auto &e : s_iquery_table) {
            if (strcmp(tok[2], e.name) == 0) {
                DaliFrame frame = e.builder(addr, inst);
                DaliTransaction txn = {};
                txn.frame       = frame;
                txn.needs_reply = true;
                txn.on_complete = on_cmd_query_reply;
                txn.cb_ctx      = nullptr;
                DaliError err = dali_sched_enqueue(&txn);
                if (err != DALI_OK) set_cmd_result("queue full");
                return;
            }
        }
        set_cmd_result("unknown iquery");
        return;
    }

    set_cmd_result("unknown verb");
    ESP_LOGD(TAG, "execute_command: %s -> unknown verb", cmd_str.c_str());
}

void DaliComponent::register_light(uint8_t type, uint8_t address,
                                   uint16_t member_groups, DaliBusLight *light)
{
    if (s_light_count < MAX_LIGHT_ENTITIES) {
        s_light_registry[s_light_count++] = { type, address, member_groups, light };
    } else {
        ESP_LOGW(TAG, "light registry full — increase MAX_LIGHT_ENTITIES");
    }
}

void DaliComponent::register_input_sensor(DaliBusSensor *sensor)
{
    if (s_sensor_count < MAX_INPUT_SENSORS) {
        s_sensor_registry[s_sensor_count++] = { sensor, 0u };
    } else {
        ESP_LOGW(TAG, "sensor registry full — increase MAX_INPUT_SENSORS");
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
