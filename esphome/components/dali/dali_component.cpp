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
#include "../../../components/dali/dali_input_poll.h"
#include "../../../components/dali/dali_memory.h"
#include "../../../components/dali/dali_group_map.h"
}

namespace esphome {
namespace dali {

static const char *TAG = "dali";

/* ── Headless dispatch state ─────────────────────────────────────────────── */

static constexpr uint8_t MAX_DISPATCH_ENTRIES = 32u;
static DaliDispatchEntry s_dispatch_table[MAX_DISPATCH_ENTRIES];
static uint8_t           s_dispatch_count = 0u;
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
static std::atomic<bool> s_refresh_query_in_flight_{false};

/* ── Group membership (auto-selects query_address for group-type lights) ──── */
/* Pure decision logic lives in dali_group_map.c; this layer owns one instance,
 * guards it with a spinlock (scan writes on Core 1, poll/console on Core 0), and
 * handles logging + flash persistence. Seeded from each light's static
 * query_address at setup() (only when no persisted snapshot exists), replaced
 * wholesale by a bus scan, and updated live by add-group/remove-group console
 * commands. Persisted to flash so state survives reboots; see start_refresh(),
 * load/save_group_membership(), and DaliComponent::loop(). */
static DaliGroupMap    s_group_map = {};
static portMUX_TYPE    s_group_map_mux = portMUX_INITIALIZER_UNLOCKED;
/* Set whenever the map changes from an authoritative source (scan or console
 * edit); the main loop drains this and writes the snapshot to flash. Decouples
 * the Core 1 scan task from the Core 0-only preferences API. */
static std::atomic<bool> s_group_members_dirty_{false};

/* Persisted-to-flash layout (see load/save_group_membership). */
static constexpr uint32_t GROUP_MEMBERSHIP_MAGIC = 0x44475031u;  /* "DGP1" */
struct GroupMembershipPersist {
    uint32_t     magic;
    DaliGroupMap map;
};

static uint8_t pick_group_member(uint8_t group)
{
    portENTER_CRITICAL(&s_group_map_mux);
    uint8_t addr = dali_group_map_pick(&s_group_map, group);
    portEXIT_CRITICAL(&s_group_map_mux);
    return addr;
}

static void update_group_members_from_config(DaliTarget target,
                                             DaliCommandId id,
                                             uint8_t group)
{
    portENTER_CRITICAL(&s_group_map_mux);
    DaliGroupMapResult res = dali_group_map_apply_config(&s_group_map, target, id, group);
    portEXIT_CRITICAL(&s_group_map_mux);

    if (res == DALI_GROUP_MAP_NO_CHANGE) return;
    s_group_members_dirty_.store(true, std::memory_order_release);
    if (res == DALI_GROUP_MAP_UNVERIFIED_ADD || res == DALI_GROUP_MAP_UNVERIFIED_REMOVE) {
        ESP_LOGW(TAG, "config g%u %s %u: source group not scan-verified; g%u query cache %s until next scan",
                 (unsigned)target.address,
                 id == DALI_CMD_ADD_TO_GROUP ? "add-group" : "remove-group",
                 (unsigned)group,
                 (unsigned)group,
                 res == DALI_GROUP_MAP_UNVERIFIED_REMOVE ? "cleared" : "left partial");
    }
}

/* ── Input sensor registry ───────────────────────────────────────────────── */

static constexpr uint8_t MAX_INPUT_SENSORS = 16u;

struct SensorEntry {
    DaliBusSensor    *sensor;
    std::atomic<bool> seq_in_flight;
    std::atomic<bool> poll_requested;
};

static SensorEntry s_sensor_registry[MAX_INPUT_SENSORS];
static uint8_t     s_sensor_count = 0u;

/* Async completion callback — runs on Core 1 (DALI task). */

static void on_input_value_done(const DaliSequenceResult *result, void *ctx)
{
    SensorEntry *e = static_cast<SensorEntry *>(ctx);
    DaliInputValue value;
    /* Rejects a failed or partially executed sequence, so a reading is only
     * published once every byte of that one latched value arrived. */
    if (result != nullptr &&
        dali_input_poll_value_from_sequence(
            result, e->sensor->get_value_bytes(), &value) == DALI_OK) {
        e->sensor->mark_raw_value((uint16_t)value.value);
    }
    e->seq_in_flight.store(false, std::memory_order_release);
}

static bool enqueue_sensor_poll(SensorEntry *e, uint32_t now_ms)
{
    if (e->seq_in_flight.exchange(true, std::memory_order_acq_rel)) {
        ESP_LOGD(TAG, "sensor poll: previous query still in flight, skipping");
        return false;
    }

    /* One sequence keeps the bytes of a reading adjacent on the bus and makes
     * admission all-or-nothing, so no half-finished read can be left queued. */
    DaliSequence seq;
    DaliError err = dali_input_poll_build_value_sequence(
        e->sensor->get_address(), e->sensor->get_instance(),
        e->sensor->get_value_bytes(), &seq);
    if (err == DALI_OK) {
        seq.on_complete = on_input_value_done;
        seq.cb_ctx      = e;
        err = dali_sched_enqueue_sequence(&seq);
    }
    if (err == DALI_OK) {
        e->sensor->set_last_poll_ms(now_ms);
        return true;
    }

    e->seq_in_flight.store(false, std::memory_order_release);
    ESP_LOGW(TAG, "sensor poll enqueue failed (%d); will retry next interval",
             (int)err);
    return false;
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
        s_refresh_query_in_flight_.store(false, std::memory_order_release);
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
    s_refresh_query_in_flight_.store(false, std::memory_order_release);
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
static std::atomic<uint32_t> s_cmd_generation_{0u};

static void set_cmd_result_locked(const char *str)
{
    strncpy(s_cmd_result_str, str, sizeof(s_cmd_result_str) - 1u);
    s_cmd_result_str[sizeof(s_cmd_result_str) - 1u] = '\0';
    s_cmd_result_dirty_.store(true, std::memory_order_relaxed);
}

static void set_cmd_result(const char *str)
{
    portENTER_CRITICAL(&s_string_mux);
    set_cmd_result_locked(str);
    portEXIT_CRITICAL(&s_string_mux);
}

static const char *cmd_enqueue_result_text(DaliError result)
{
    if (result == DALI_OK) return "OK";
    if (result == DALI_ERR_QUEUE_FULL) return "queue full";
    return "err";
}

static void set_cmd_enqueue_result(DaliError result)
{
    set_cmd_result(cmd_enqueue_result_text(result));
}

static void set_cmd_enqueue_error(DaliError result)
{
    if (result != DALI_OK) set_cmd_enqueue_result(result);
}

static void *cmd_generation_ctx(uint32_t generation)
{
    return reinterpret_cast<void *>(static_cast<uintptr_t>(generation));
}

static uint32_t begin_cmd_generation()
{
    portENTER_CRITICAL(&s_string_mux);
    uint32_t generation = s_cmd_generation_.load(std::memory_order_relaxed) + 1u;
    if (generation == 0u) generation = 1u;
    s_cmd_generation_.store(generation, std::memory_order_relaxed);
    portEXIT_CRITICAL(&s_string_mux);
    return generation;
}

static void set_cmd_result_for_generation(const char *str, void *ctx)
{
    uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
    portENTER_CRITICAL(&s_string_mux);
    if (generation == s_cmd_generation_.load(std::memory_order_relaxed)) {
        set_cmd_result_locked(str);
    }
    portEXIT_CRITICAL(&s_string_mux);
}

static void on_cmd_query_reply(DaliError result, const DaliFrame *reply, void *ctx)
{
    if (result != DALI_OK || reply == nullptr) {
        set_cmd_result_for_generation(result == DALI_ERR_TIMEOUT ? "no reply" : "err", ctx);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u (0x%02X)", (unsigned)(reply->data & 0xFFu),
                 (unsigned)(reply->data & 0xFFu));
        set_cmd_result_for_generation(buf, ctx);
    }
}

static void on_memread_done(const DaliSequenceResult *result, void *ctx)
{
    DaliFrame reply;
    if (result == nullptr || result->result != DALI_OK ||
        !dali_sequence_result_last_reply(result, &reply)) {
        DaliError err = result != nullptr ? result->result : DALI_ERR_INVALID;
        set_cmd_result_for_generation(err == DALI_ERR_TIMEOUT ? "no reply" : "err", ctx);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u (0x%02X)", (unsigned)(reply.data & 0xFFu),
                 (unsigned)(reply.data & 0xFFu));
        set_cmd_result_for_generation(buf, ctx);
    }
}

static void set_raw_result(DaliError result, const DaliFrame *reply, bool sent_twice,
                           void *ctx)
{
    if (result == DALI_OK && reply != nullptr) {
        char buf[16];
        snprintf(buf, sizeof(buf), "RX %u (0x%02X)", (unsigned)(reply->data & 0xFFu),
                 (unsigned)(reply->data & 0xFFu));
        set_cmd_result_for_generation(buf, ctx);
        return;
    }

    if (result == DALI_ERR_TIMEOUT) {
        set_cmd_result_for_generation("RX timeout", ctx);
        return;
    }

    if (result == DALI_OK) {
        set_cmd_result_for_generation(sent_twice ? "TX2 OK" : "TX OK", ctx);
        return;
    }

    char buf[20];
    snprintf(buf, sizeof(buf), sent_twice ? "TX2 ERR %d" : "TX ERR %d", (int)result);
    set_cmd_result_for_generation(buf, ctx);
}

static void on_raw_done(DaliError result, const DaliFrame *reply, void *ctx)
{
    set_raw_result(result, reply, false, ctx);
}

static void on_raw2_done(DaliError result, const DaliFrame *reply, void *ctx)
{
    set_raw_result(result, reply, true, ctx);
}

/* ── Find-couplers recording (Core 1 writes, Core 0 reads) ──────────────── */

static constexpr uint8_t    MAX_COUPLER_FRAMES = 32u;
static DaliInputEvent        s_coupler_frames[MAX_COUPLER_FRAMES];
static std::atomic<uint8_t>  s_coupler_count_{0};
static std::atomic<bool>     s_find_couplers_active_{false};
static std::atomic<bool>     s_find_couplers_stopped_ack_{true};

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
    if (e->frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT) {
        const char *pfx = (e->address_kind == DALI_EVENT_ADDRESS_GROUP) ? "g"
                        : (e->address_kind == DALI_EVENT_ADDRESS_BROADCAST) ? "bc"
                                                                             : "a";
        if (!e->address_selector) {
            if (e->address_kind == DALI_EVENT_ADDRESS_BROADCAST)
                snprintf(buf, len, "bc dapc %u", (unsigned)e->legacy_data);
            else
                snprintf(buf, len, "%s%u dapc %u", pfx, (unsigned)e->address,
                         (unsigned)e->legacy_data);
        } else if (e->address_kind == DALI_EVENT_ADDRESS_BROADCAST) {
            snprintf(buf, len, "bc %s", opcode_name(e->legacy_data));
        } else {
            snprintf(buf, len, "%s%u %s", pfx, (unsigned)e->address,
                     opcode_name(e->legacy_data));
        }
        return;
    }

    if (e->frame_kind == DALI_EVENT_FRAME_POWER_NOTIFICATION_24BIT) {
        if (e->source.has_device_group && e->source.has_device_address) {
            snprintf(buf, len, "power dg%u a%u", (unsigned)e->source.device_group,
                     (unsigned)e->source.device_address);
        } else if (e->source.has_device_group) {
            snprintf(buf, len, "power dg%u", (unsigned)e->source.device_group);
        } else if (e->source.has_device_address) {
            snprintf(buf, len, "power a%u", (unsigned)e->source.device_address);
        } else {
            snprintf(buf, len, "power");
        }
        return;
    }

    if (e->frame_kind == DALI_EVENT_FRAME_INPUT_24BIT) {
        switch (e->source.scheme) {
            case DALI_EVENT_SOURCE_DEVICE:
                snprintf(buf, len, "a%u type=%u evt=%u",
                         (unsigned)e->source.device_address,
                         (unsigned)e->source.instance_type,
                         (unsigned)e->event_information);
                break;
            case DALI_EVENT_SOURCE_DEVICE_INSTANCE:
                snprintf(buf, len, "a%u inst=%u evt=%u",
                         (unsigned)e->source.device_address,
                         (unsigned)e->source.instance,
                         (unsigned)e->event_information);
                break;
            case DALI_EVENT_SOURCE_DEVICE_GROUP:
                snprintf(buf, len, "dg%u type=%u evt=%u",
                         (unsigned)e->source.device_group,
                         (unsigned)e->source.instance_type,
                         (unsigned)e->event_information);
                break;
            case DALI_EVENT_SOURCE_INSTANCE:
                snprintf(buf, len, "type=%u inst=%u evt=%u",
                         (unsigned)e->source.instance_type,
                         (unsigned)e->source.instance,
                         (unsigned)e->event_information);
                break;
            case DALI_EVENT_SOURCE_INSTANCE_GROUP:
                snprintf(buf, len, "ig%u type=%u evt=%u",
                         (unsigned)e->source.instance_group,
                         (unsigned)e->source.instance_type,
                         (unsigned)e->event_information);
                break;
            default:
                snprintf(buf, len, "event scheme=%u evt=%u",
                         (unsigned)e->source.scheme,
                         (unsigned)e->event_information);
                break;
        }
        return;
    }

    snprintf(buf, len, "invalid");
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
    if (s_find_couplers_active_.load(std::memory_order_acquire)) {
        uint8_t cnt = s_coupler_count_.load(std::memory_order_relaxed);
        bool dup = false;
        for (uint8_t i = 0u; i < cnt; i++) {
            if (s_coupler_frames[i].raw.bit_length == event.raw.bit_length &&
                s_coupler_frames[i].raw.data == event.raw.data) {
                dup = true;
                break;
            }
        }
        if (!dup && cnt < MAX_COUPLER_FRAMES) {
            s_coupler_frames[cnt] = event;
            /* Release ensures all field writes are visible before the count. */
            s_coupler_count_.store(cnt + 1u, std::memory_order_release);
        }
    }

    /* Push to event queue for headless dispatch. */
    DaliInputEventRecord rec;
    rec.event        = event;
    rec.timestamp_us = 0u;
    dali_event_queue_push(&s_event_queue, &rec);

    /* Device/instance events identify one configured query target exactly.
     * Treat the event as a change notification only: QUERY INPUT VALUE remains
     * authoritative for both one- and two-byte sensor entities. Other Part-103
     * source schemes need type/group metadata that the current YAML does not
     * carry, so do not guess a target for them. */
    if (event.frame_kind == DALI_EVENT_FRAME_INPUT_24BIT &&
        event.source.scheme == DALI_EVENT_SOURCE_DEVICE_INSTANCE &&
        event.source.has_device_address && event.source.has_instance) {
        for (uint8_t i = 0u; i < s_sensor_count; i++) {
            SensorEntry &e = s_sensor_registry[i];
            if (e.sensor->get_address()  == event.source.device_address &&
                e.sensor->get_instance() == event.source.instance) {
                e.poll_requested.store(true, std::memory_order_release);
                ESP_LOGD(TAG, "event poll requested: addr=%u inst=%u info=%u raw=%06X",
                         (unsigned)event.source.device_address,
                         (unsigned)event.source.instance,
                         (unsigned)event.event_information,
                         (unsigned)event.raw.data);
            }
        }
    }
}

/* ── DALI task (Core 1) ──────────────────────────────────────────────────── */

static void dali_task(void *)
{
    const TickType_t delay = pdMS_TO_TICKS(1);
    for (;;) {
        dali_phy_rx_process();
        dali_sched_run();

        /* Acknowledge a stop only after scheduler callbacks from this task have
         * finished, so Core 0 cannot drain while a capture write is pending. */
        if (!s_find_couplers_active_.load(std::memory_order_acquire)) {
            s_find_couplers_stopped_ack_.store(true, std::memory_order_release);
        }

        /* Always drain the event queue to prevent overflow.
         * Only dispatch if a headless table is loaded. */
        DaliInputEventRecord rec;
        while (dali_event_queue_pop(&s_event_queue, &rec)) {
            if (s_dispatch_count > 0u) {
                char event_str[48];
                format_event(event_str, sizeof(event_str), &rec.event);
                DaliDispatchResult result = {};
                DaliError err = dali_dispatch(s_dispatch_table, s_dispatch_count,
                                              &rec.event, &s_toggle_state, &result);
                if (err != DALI_OK) {
                    ESP_LOGD(TAG, "dispatch err %d (%s)", (int)err, event_str);
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

    if (s_dispatch_count > 0u) {
        ESP_LOGI(TAG, "headless dispatch: %u entries loaded", (unsigned)s_dispatch_count);
    }

    /* Group-membership table: prefer the persisted snapshot (from a prior scan
     * or console edits); it fully describes reality including empty groups. Only
     * when nothing is persisted (first boot, flash erase, format change) do we
     * fall back to seeding from each group light's static query_address.
     *
     * Reading get_query_address() is safe here (unlike inside register_light()):
     * codegen emits set_query_address() after set_dali_component(), so the value
     * is only final once every light's setup calls have run — guaranteed by the
     * time Component::setup() fires.
     *
     * Note: a group light added to YAML after the last scan won't be in the
     * persisted snapshot and so won't be polled until the next scan. */
    group_pref_ = global_preferences->make_preference<GroupMembershipPersist>(GROUP_MEMBERSHIP_MAGIC);
    if (load_group_membership()) {
        ESP_LOGI(TAG, "group membership restored from flash");
    } else {
        /* No cross-core contention yet — dali_task is not started until below. */
        portENTER_CRITICAL(&s_group_map_mux);
        for (uint8_t i = 0u; i < s_light_count; i++) {
            LightEntry &e = s_light_registry[i];
            if (e.target_type != (uint8_t)DALI_ADDR_GROUP || e.target_address >= 16u) continue;
            uint8_t qa = e.light->get_query_address();
            if (qa == 0xFFu) continue;
            dali_group_map_seed(&s_group_map, e.target_address, qa);
        }
        portEXIT_CRITICAL(&s_group_map_mux);
    }

    xTaskCreatePinnedToCore(dali_task, "dali", 4096, nullptr, 10, nullptr, 1);

    if (scan_status_) scan_status_->publish_state("Idle");
    if (bus_fault_)   bus_fault_->publish_state("OK");

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
            bool requested = e.poll_requested.load(std::memory_order_acquire);
            if ((!due && !requested) ||
                e.seq_in_flight.load(std::memory_order_acquire)) {
                continue;
            }

            bool consumed_request =
                e.poll_requested.exchange(false, std::memory_order_acq_rel);
            if (!enqueue_sensor_poll(&e, now) && consumed_request) {
                e.poll_requested.store(true, std::memory_order_release);
            }
        }
        boot_sensor_query_done_ = true;
    }

    /* ── Persist group membership when a scan/console edit dirtied it ── */
    if (s_group_members_dirty_.exchange(false, std::memory_order_acq_rel))
        save_group_membership();

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

    /* Admit full-refresh queries one at a time. Queue pressure and scans retain
     * the cursor, so no later light is silently dropped. */
    pump_refresh();

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

    /* ── Bus fault detection ── */
    if (bus_fault_) {
        uint32_t count = g_dali_stats.bus_idle_failures;
        if (count != last_bus_fault_count_) {
            last_bus_fault_count_ = count;
            char buf[32];
            snprintf(buf, sizeof(buf), "Bus stuck: %u", (unsigned)count);
            ESP_LOGE(TAG, "DALI bus stuck — %u total occurrence(s)", (unsigned)count);
            bus_fault_->publish_state(buf);
        }
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
    if (s_find_couplers_active_.load(std::memory_order_acquire) &&
        (uint32_t)(millis() - find_couplers_end_ms_) < 0x80000000u) {
        /* Timer elapsed: request stop; drain only after the DALI task acks it. */
        s_find_couplers_stopped_ack_.store(false, std::memory_order_relaxed);
        s_find_couplers_active_.store(false, std::memory_order_release);
        find_couplers_collect_ = true;
        if (scan_status_) scan_status_->publish_state("Coupler scan done");
    } else if (find_couplers_collect_ &&
               s_find_couplers_stopped_ack_.load(std::memory_order_acquire)) {
        find_couplers_collect_ = false;
        uint8_t cnt = s_coupler_count_.load(std::memory_order_acquire);

        /* Build coupler group bitmask from captured frames. */
        uint16_t gmask = 0u;
        for (uint8_t i = 0u; i < cnt; i++) {
            const DaliInputEvent &event = s_coupler_frames[i];
            if (event.frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT &&
                event.address_kind == DALI_EVENT_ADDRESS_GROUP) {
                gmask |= (uint16_t)(1u << event.address);
            }
        }
        s_coupler_group_mask_.store(gmask, std::memory_order_release);

        if (couplers_result_) {
            if (cnt == 0u) {
                couplers_result_->publish_state("None");
            } else {
                char buf[128] = {};
                for (uint8_t i = 0u; i < cnt; i++) {
                    const DaliInputEvent &event = s_coupler_frames[i];
                    char entry[48];
                    format_event(entry, sizeof(entry), &event);
                    ESP_LOGI(TAG, "coupler[%u] %s raw=%0*X", (unsigned)i,
                             entry, event.raw.bit_length == 24u ? 6 : 4,
                             (unsigned)event.raw.data);
                    if (i > 0u)
                        strncat(buf, "; ", sizeof(buf) - strlen(buf) - 1u);
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

    bool scan_success = scan_success_.load();
    if (scan_status_) {
        if (scan_success) {
            scan_status_->publish_state(
                "Found " + std::to_string(scan_count_.load()) + " devices");
        } else {
            scan_status_->publish_state("Scan error");
        }
    }
    if (scan_success) start_refresh();
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

static bool parse_raw_len(const char *s, uint8_t *out)
{
    if (s == nullptr || out == nullptr || strncmp(s, "len=", 4) != 0) return false;
    unsigned long bits;
    if (!parse_uint(s + 4, 16u, 24u, &bits)) return false;
    if (bits != 16u && bits != 24u) return false;
    *out = (uint8_t)bits;
    return true;
}

static bool parse_hex_frame(const char *s, uint8_t bit_len, DaliFrame *out)
{
    if (s == nullptr || out == nullptr || (bit_len != 16u && bit_len != 24u)) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (*s == '\0') return false;

    for (const char *p = s; *p != '\0'; p++) {
        if (!isxdigit((unsigned char)*p)) return false;
    }

    char *end;
    unsigned long value = strtoul(s, &end, 16);
    if (*end != '\0') return false;

    unsigned long max_value = (1UL << bit_len) - 1UL;
    if (value > max_value) return false;

    out->data       = (uint32_t)value;
    out->bit_length = bit_len;
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

/* Lookup table for 24-bit instance configuration and control commands.
 *
 * Type-specific names are deliberately explicit except for the three Part 303
 * timer names retained for backwards compatibility with the installed site.
 * DTR0 ranges come from the applicable Part 103/3xx command definition. */
typedef DaliFrame (*IConfigBuilder)(uint8_t addr, uint8_t instance);
enum IConfigParamKind : uint8_t {
    ICONFIG_NO_PARAM = 0,
    ICONFIG_DTR0,
};
struct IConfigEntry {
    const char       *name;
    IConfigBuilder    builder;
    IConfigParamKind  param_kind;
    uint8_t           min_value;
    uint8_t           max_value;
    bool              allow_zero;
    bool              allow_mask;
    bool              send_twice;
};
static const IConfigEntry s_iconfig_table[] = {
    { "set-event-priority",   dali_input_build_set_event_priority,         ICONFIG_DTR0,     2u,   5u, false, false, true  },
    { "set-primary-group",    dali_input_build_set_primary_group,          ICONFIG_DTR0,     0u,  31u, false, true,  true  },
    { "set-instance-group-1", dali_input_build_set_instance_group1,        ICONFIG_DTR0,     0u,  31u, false, true,  true  },
    { "set-instance-group-2", dali_input_build_set_instance_group2,        ICONFIG_DTR0,     0u,  31u, false, true,  true  },
    { "set-event-scheme",     dali_input_build_set_event_scheme,           ICONFIG_DTR0,     0u,   4u, false, false, true  },
    { "enable-instance",      dali_input_build_enable_instance,            ICONFIG_NO_PARAM, 0u,   0u, false, false, true  },
    { "disable-instance",     dali_input_build_disable_instance,           ICONFIG_NO_PARAM, 0u,   0u, false, false, true  },

    { "pb-set-short-timer",   dali_input_pb_build_set_short_timer,         ICONFIG_DTR0,    10u, 255u, false, false, true  },
    { "pb-set-double-timer",  dali_input_pb_build_set_double_timer,        ICONFIG_DTR0,    10u, 100u, true,  false, true  },
    { "pb-set-repeat-timer",  dali_input_pb_build_set_repeat_timer,        ICONFIG_DTR0,     5u, 100u, false, false, true  },
    { "pb-set-stuck-timer",   dali_input_pb_build_set_stuck_timer,         ICONFIG_DTR0,     5u, 255u, false, false, true  },

    { "catch-movement",       dali_input_occ_build_catch_movement,         ICONFIG_NO_PARAM, 0u,   0u, false, false, false },
    { "set-hold-timer",       dali_input_occ_build_set_hold_timer,         ICONFIG_DTR0,     0u, 254u, false, false, true  },
    { "set-report-timer",     dali_input_occ_build_set_report_timer,       ICONFIG_DTR0,     0u, 255u, false, false, true  },
    { "set-deadtime",         dali_input_occ_build_set_deadtime,           ICONFIG_DTR0,     0u, 255u, false, false, true  },
    { "cancel-hold-timer",    dali_input_occ_build_cancel_hold_timer,      ICONFIG_NO_PARAM, 0u,   0u, false, false, false },
    { "set-detection-range",  dali_input_occ_build_set_detection_range,    ICONFIG_DTR0,     0u, 100u, false, false, true  },
    { "set-sensitivity",      dali_input_occ_build_set_sensitivity,        ICONFIG_DTR0,     0u, 100u, false, false, true  },

    { "light-set-report-timer",   dali_input_light_build_set_report_timer,   ICONFIG_DTR0, 0u, 255u, false, false, true },
    { "light-set-hysteresis",     dali_input_light_build_set_hysteresis,     ICONFIG_DTR0, 0u,  25u, false, false, true },
    { "light-set-deadtime",       dali_input_light_build_set_deadtime,       ICONFIG_DTR0, 0u, 255u, false, false, true },
    { "light-set-hysteresis-min", dali_input_light_build_set_hysteresis_min, ICONFIG_DTR0, 0u, 255u, false, false, true },
};

static bool iconfig_value_valid(const IConfigEntry &entry, unsigned long value)
{
    if (value >= entry.min_value && value <= entry.max_value) return true;
    if (entry.allow_zero && value == 0u) return true;
    if (entry.allow_mask && value == 0xFFu) return true;
    return false;
}

/* Adapters: dali_input_device.h uses DaliError+out-ptr; iquery table needs DaliFrame return. */
static DaliFrame s_qry_instance_type(uint8_t a, uint8_t i)    { DaliFrame f={}; dali_input_build_query_instance_type(a,i,&f);    return f; }
static DaliFrame s_qry_resolution(uint8_t a, uint8_t i)       { DaliFrame f={}; dali_input_build_query_resolution(a,i,&f);        return f; }
static DaliFrame s_qry_instance_error(uint8_t a, uint8_t i)   { DaliFrame f={}; dali_input_build_query_instance_error(a,i,&f);   return f; }
static DaliFrame s_qry_instance_status(uint8_t a, uint8_t i)  { DaliFrame f={}; dali_input_build_query_instance_status(a,i,&f);   return f; }
static DaliFrame s_qry_instance_enabled(uint8_t a, uint8_t i) { DaliFrame f={}; dali_input_build_query_instance_enabled(a,i,&f);  return f; }
static DaliFrame s_qry_event_priority(uint8_t a, uint8_t i)   { DaliFrame f={}; dali_input_build_query_event_priority(a,i,&f);   return f; }
static DaliFrame s_qry_primary_group(uint8_t a, uint8_t i)    { DaliFrame f={}; dali_input_build_query_primary_instance_group(a,i,&f); return f; }
static DaliFrame s_qry_instance_group1(uint8_t a, uint8_t i)  { DaliFrame f={}; dali_input_build_query_instance_group1(a,i,&f); return f; }
static DaliFrame s_qry_instance_group2(uint8_t a, uint8_t i)  { DaliFrame f={}; dali_input_build_query_instance_group2(a,i,&f); return f; }
static DaliFrame s_qry_event_scheme(uint8_t a, uint8_t i)     { DaliFrame f={}; dali_input_build_query_event_scheme(a,i,&f);     return f; }
static DaliFrame s_qry_event_filter0(uint8_t a, uint8_t i)    { DaliFrame f={}; dali_input_build_query_event_filter_zero(a,i,&f); return f; }
static DaliFrame s_qry_event_filter1(uint8_t a, uint8_t i)    { DaliFrame f={}; dali_input_build_query_event_filter_one(a,i,&f); return f; }
static DaliFrame s_qry_event_filter2(uint8_t a, uint8_t i)    { DaliFrame f={}; dali_input_build_query_event_filter_two(a,i,&f); return f; }
static DaliFrame s_qry_input_value(uint8_t a, uint8_t i)      { DaliFrame f={}; dali_build_instance_command(a,i,DALI_CMD_QUERY_INPUT_VALUE,&f);       return f; }
static DaliFrame s_qry_input_value_latch(uint8_t a, uint8_t i){ DaliFrame f={}; dali_build_instance_command(a,i,DALI_CMD_QUERY_INPUT_VALUE_LATCH,&f); return f; }

/* Lookup table for 24-bit instance query commands (single send, expects reply). */
struct IQueryEntry { const char *name; IConfigBuilder builder; };
static const IQueryEntry s_iquery_table[] = {
    { "instance-type",       s_qry_instance_type     },
    { "resolution",          s_qry_resolution        },
    { "instance-error",      s_qry_instance_error    },
    { "instance-status",     s_qry_instance_status   },
    { "instance-enabled",    s_qry_instance_enabled  },
    { "event-priority",      s_qry_event_priority    },
    { "primary-group",       s_qry_primary_group     },
    { "instance-group-1",    s_qry_instance_group1   },
    { "instance-group-2",    s_qry_instance_group2   },
    { "event-scheme",        s_qry_event_scheme      },
    { "event-filter-0",      s_qry_event_filter0     },
    { "event-filter-1",      s_qry_event_filter1     },
    { "event-filter-2",      s_qry_event_filter2     },
    { "input-value",         s_qry_input_value       },
    { "input-value-latch",   s_qry_input_value_latch },

    { "pb-short-timer",      dali_input_pb_build_query_short_timer      },
    { "pb-short-timer-min",  dali_input_pb_build_query_short_timer_min  },
    { "pb-double-timer",     dali_input_pb_build_query_double_timer     },
    { "pb-double-timer-min", dali_input_pb_build_query_double_timer_min },
    { "pb-repeat-timer",     dali_input_pb_build_query_repeat_timer     },
    { "pb-stuck-timer",      dali_input_pb_build_query_stuck_timer      },

    { "hold-timer",          dali_input_occ_build_query_hold_timer       },
    { "deadtime",            dali_input_occ_build_query_deadtime         },
    { "report-timer",        dali_input_occ_build_query_report_timer     },
    { "occupancy-capabilities", dali_input_occ_build_query_capabilities   },
    { "detection-range",     dali_input_occ_build_query_detection_range  },
    { "sensitivity",         dali_input_occ_build_query_sensitivity      },
    { "catching",            dali_input_occ_build_query_catching         },

    { "light-hysteresis-min", dali_input_light_build_query_hysteresis_min },
    { "light-deadtime",       dali_input_light_build_query_deadtime       },
    { "light-report-timer",   dali_input_light_build_query_report_timer   },
    { "light-hysteresis",     dali_input_light_build_query_hysteresis     },
};

void DaliComponent::execute_command(const std::string &cmd_str)
{
    /* Tokenise into 5 command tokens plus one overflow sentinel. */
    char buf[96];
    if (cmd_str.size() >= sizeof(buf)) {
        begin_cmd_generation();
        set_cmd_result("command too long");
        return;
    }
    strncpy(buf, cmd_str.c_str(), sizeof(buf) - 1u);
    buf[sizeof(buf) - 1u] = '\0';

    const char *tok[6] = {};
    uint8_t     ntok   = 0u;
    char       *p      = buf;
    while (*p && ntok < 6u) {
        while (*p == ' ') p++;
        if (!*p) break;
        tok[ntok++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    if (ntok == 0u) return;

    const char *verb = tok[0];
    uint32_t cmd_generation = begin_cmd_generation();
    void *cmd_ctx = cmd_generation_ctx(cmd_generation);

    /* Raw frame: raw/raw2 <hex> len=<16|24> [wait]. */
    if (strcmp(verb, "raw") == 0 || strcmp(verb, "raw2") == 0) {
        if (ntok < 3u) { set_cmd_result("bad raw syntax"); return; }
        bool send_twice = (strcmp(verb, "raw2") == 0);
        bool wait_reply = false;
        if (ntok == 4u) {
            if (strcmp(tok[3], "wait") != 0) { set_cmd_result("bad raw syntax"); return; }
            wait_reply = true;
        } else if (ntok > 4u) {
            set_cmd_result("bad raw syntax");
            return;
        }

        uint8_t bit_len;
        DaliFrame frame;
        if (!parse_raw_len(tok[2], &bit_len) || !parse_hex_frame(tok[1], bit_len, &frame)) {
            set_cmd_result("bad raw syntax");
            return;
        }

        DaliTransaction txn = {};
        txn.frame       = frame;
        txn.needs_reply = wait_reply;
        txn.send_twice  = send_twice;
        txn.on_complete = send_twice ? on_raw2_done : on_raw_done;
        txn.cb_ctx      = cmd_ctx;
        set_cmd_result("pending");
        set_cmd_enqueue_error(dali_sched_enqueue(&txn));
        return;
    }

    /* ── Direct gear control ── */
    DaliTarget tgt{};
    if ((strcmp(verb, "off") == 0 || strcmp(verb, "max") == 0 ||
         strcmp(verb, "min") == 0) && ntok >= 2u) {
        if (!parse_target(tok[1], &tgt)) { set_cmd_result("bad target"); return; }
        DaliError err;
        if (strcmp(verb, "off") == 0) err = dali_control_off(tgt);
        else if (strcmp(verb, "max") == 0) err = dali_control_recall_max(tgt);
        else err = dali_control_recall_min(tgt);
        set_cmd_enqueue_result(err);
        return;
    }

    if (strcmp(verb, "level") == 0 && ntok >= 3u) {
        if (!parse_target(tok[1], &tgt)) { set_cmd_result("bad target"); return; }
        unsigned long lv;
        if (!parse_uint(tok[2], 0u, 254u, &lv)) { set_cmd_result("level 0-254"); return; }
        set_cmd_enqueue_result(dali_control_set_level(tgt, (uint8_t)lv));
        return;
    }

    /* ── Gear query ── */
    if (strcmp(verb, "query") == 0 && ntok >= 3u) {
        if (!parse_target(tok[1], &tgt)) { set_cmd_result("bad target"); return; }
        for (const auto &e : s_query_table) {
            if (strcmp(tok[2], e.name) == 0) {
                set_cmd_result("pending");
                set_cmd_enqueue_error(
                    dali_control_query(tgt, e.id, 0u, on_cmd_query_reply, cmd_ctx));
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
                    bool group_edit = (e.id == DALI_CMD_ADD_TO_GROUP ||
                                       e.id == DALI_CMD_REMOVE_FROM_GROUP);
                    if (group_edit) {
                        if (ntok < 4u) { set_cmd_result("needs group 0-15"); return; }
                        if (!parse_uint(tok[3], 0u, 15u, &paramv)) { set_cmd_result("group 0-15"); return; }
                        if (tgt.type == DALI_ADDR_BROADCAST) { set_cmd_result("no broadcast group config"); return; }
                    } else if (ntok >= 4u && !parse_uint(tok[3], 0u, 255u, &paramv)) {
                        set_cmd_result("bad param");
                        return;
                    }
                    err = dali_control_config(tgt, e.id, (uint8_t)paramv);
                    if (err == DALI_OK && group_edit)
                        update_group_members_from_config(tgt, e.id, (uint8_t)paramv);
                } else {
                    set_cmd_result("needs dtr0 value");
                    return;
                }
                set_cmd_enqueue_result(err);
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
                DaliFrame cfg_frame = e.builder(addr, inst);
                DaliSequence seq = {};
                if (e.param_kind == ICONFIG_DTR0) {
                    if (ntok != 4u) { set_cmd_result("iconfig: needs one value"); return; }
                    unsigned long dtr0v;
                    if (!parse_uint(tok[3], 0u, 255u, &dtr0v) ||
                        !iconfig_value_valid(e, dtr0v)) {
                        set_cmd_result("iconfig: value out of range");
                        return;
                    }
                    DaliFrame dtr_frame = dali_cmd_control_device_dtr0_data((uint8_t)dtr0v);
                    seq.steps[0] = { dtr_frame, false, false, 0u };
                    seq.steps[1] = { cfg_frame, false, e.send_twice, 0u };
                    seq.step_count = 2u;
                } else {
                    if (ntok != 3u) { set_cmd_result("iconfig: unexpected value"); return; }
                    seq.steps[0] = { cfg_frame, false, e.send_twice, 0u };
                    seq.step_count = 1u;
                }
                set_cmd_enqueue_result(dali_sched_enqueue_sequence(&seq));
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
                txn.cb_ctx      = cmd_ctx;
                set_cmd_result("pending");
                set_cmd_enqueue_error(dali_sched_enqueue(&txn));
                return;
            }
        }
        set_cmd_result("unknown iquery");
        return;
    }

    /* ── Memory read: memread a<N> <bank> <offset> ── */
    if (strcmp(verb, "memread") == 0 && ntok >= 4u) {
        DaliTarget memtgt{};
        if (!parse_target(tok[1], &memtgt) || memtgt.type != DALI_ADDR_SHORT) {
            set_cmd_result("memread: use a<N>");
            return;
        }
        unsigned long bankv, offv;
        if (!parse_uint(tok[2], 0u, 255u, &bankv)) { set_cmd_result("bad bank");   return; }
        if (!parse_uint(tok[3], 0u, 255u, &offv))  { set_cmd_result("bad offset"); return; }
        /* READ MEMORY LOCATION for a 103 control device uses 24-bit control-device
         * DTR setup plus a 24-bit device command. */
        DaliSequence seq = {};
        seq.steps[0] = { dali_cmd_control_device_dtr1_data((uint8_t)bankv), false, false, 0u };
        seq.steps[1] = { dali_cmd_control_device_dtr0_data((uint8_t)offv),  false, false, 0u };
        seq.steps[2] = { dali_cmd_device(memtgt.address, 0x3Cu),            true,  false, 0u };
        seq.step_count  = 3u;
        seq.on_complete = on_memread_done;
        seq.cb_ctx      = cmd_ctx;
        set_cmd_result("pending");
        set_cmd_enqueue_error(dali_sched_enqueue_sequence(&seq));
        return;
    }

    /* ── Raw device query: devquery a<N> <opcode>
     * Sends a single 24-bit device-level command (addr FE opcode) and returns
     * the 8-bit reply. Opcode in decimal (e.g. 48=0x30=QUERY DEVICE STATUS,
     * 53=0x35=QUERY NUMBER OF INSTANCES, 60=0x3C=READ MEM LOCATION without DTR setup).
     *   devquery a0 54   → 01 FE 36, returns Steinel's current DTR0
     *   devquery a0 55   → 01 FE 37, returns Steinel's current DTR1 ── */
    if (strcmp(verb, "devquery") == 0 && ntok >= 3u) {
        DaliTarget tgt{};
        if (!parse_target(tok[1], &tgt) || tgt.type != DALI_ADDR_SHORT) {
            set_cmd_result("devquery: use a<N>");
            return;
        }
        unsigned long opcv;
        if (!parse_uint(tok[2], 0u, 255u, &opcv)) { set_cmd_result("bad opcode"); return; }
        DaliTransaction txn = {};
        txn.frame       = dali_cmd_device(tgt.address, (uint8_t)opcv);
        txn.needs_reply = true;
        txn.on_complete = on_cmd_query_reply;
        txn.cb_ctx      = cmd_ctx;
        set_cmd_result("pending");
        set_cmd_enqueue_error(dali_sched_enqueue(&txn));
        return;
    }

    /* ── DTR check: dtrcheck a<N> <reg> <value>
     * Sends SET DTR0 (reg=0) or SET DTR1 (reg=1), then immediately queries
     * that register from the addressed 103 device.
     *   dtrcheck a0 0 66  → set DTR0=0x42, then query it back.
     *                        If Steinel replies 0x42 the SET works. ── */
    if (strcmp(verb, "dtrcheck") == 0 && ntok >= 4u) {
        DaliTarget tgt{};
        if (!parse_target(tok[1], &tgt) || tgt.type != DALI_ADDR_SHORT) {
            set_cmd_result("dtrcheck: use a<N>");
            return;
        }
        unsigned long regv, valv;
        if (!parse_uint(tok[2], 0u, 1u,   &regv)) { set_cmd_result("dtrcheck: reg 0 or 1"); return; }
        if (!parse_uint(tok[3], 0u, 255u,  &valv)) { set_cmd_result("bad value");            return; }
        DaliFrame set_frame  = (regv == 0u) ? dali_cmd_control_device_dtr0_data((uint8_t)valv)
                                            : dali_cmd_control_device_dtr1_data((uint8_t)valv);
        uint8_t   query_opc  = (regv == 0u) ? 0x36u : 0x37u;
        DaliSequence seq = {};
        seq.steps[0] = { set_frame,                                        false, false, 0u };
        seq.steps[1] = { dali_cmd_device(tgt.address, query_opc),         true,  false, 0u };
        seq.step_count  = 2u;
        seq.on_complete = on_memread_done;
        seq.cb_ctx      = cmd_ctx;
        set_cmd_result("pending");
        set_cmd_enqueue_error(dali_sched_enqueue_sequence(&seq));
        return;
    }

    /* ── Memory write: memwrite a<N> <bank> <offset> <value> ──
     * Implements the IEC 62386-103 write sequence as one scheduler sequence
     * (seven logical steps, nine forward frames after send-twice expansion):
     *   unlock bank: DTR1=bank, DTR0=0x02(lock), ENABLE_WRITE(×2), WRITE=0x55
     *   write value: DTR0=offset, ENABLE_WRITE(×2), WRITE=value
     * ENABLE WRITE MEMORY for a control device is a 24-bit device command (FE 0x15),
     * distinct from the 16-bit gear command (0x81). ── */
    if (strcmp(verb, "memwrite") == 0) {
        if (ntok != 5u) { set_cmd_result("memwrite: needs 4 args"); return; }
        DaliTarget memtgt{};
        if (!parse_target(tok[1], &memtgt) || memtgt.type != DALI_ADDR_SHORT) {
            set_cmd_result("memwrite: use a<N>");
            return;
        }
        unsigned long bankv, offv, valv;
        if (!parse_uint(tok[2], 1u, 255u, &bankv)) { set_cmd_result("bank 1-255"); return; }
        if (!parse_uint(tok[3], 0u, 255u, &offv))  { set_cmd_result("bad offset"); return; }
        if (!parse_uint(tok[4], 0u, 255u, &valv))  { set_cmd_result("bad value");  return; }

        DaliSequence seq = {};
        DaliError err = dali_memory_build_control_device_write_sequence(
            memtgt.address, (uint8_t)bankv, (uint8_t)offv, (uint8_t)valv, &seq);
        if (err == DALI_OK) {
            err = dali_sched_enqueue_sequence(&seq);
        }
        set_cmd_enqueue_result(err);
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

void DaliComponent::set_group_membership_snapshot(const uint64_t masks[16])
{
    /* Runs on the scan task (Core 1); flash write is deferred to loop() (Core 0). */
    portENTER_CRITICAL(&s_group_map_mux);
    for (uint8_t g = 0u; g < 16u; g++)
        s_group_map.members[g] = masks[g];
    s_group_map.verified = 0xFFFFu;
    portEXIT_CRITICAL(&s_group_map_mux);
    s_group_members_dirty_.store(true, std::memory_order_release);
}

bool DaliComponent::load_group_membership()
{
    GroupMembershipPersist st{};
    if (!group_pref_.load(&st) || st.magic != GROUP_MEMBERSHIP_MAGIC) return false;
    portENTER_CRITICAL(&s_group_map_mux);
    s_group_map = st.map;
    portEXIT_CRITICAL(&s_group_map_mux);
    return true;
}

void DaliComponent::save_group_membership()
{
    GroupMembershipPersist st{};
    st.magic = GROUP_MEMBERSHIP_MAGIC;
    portENTER_CRITICAL(&s_group_map_mux);
    st.map = s_group_map;
    portEXIT_CRITICAL(&s_group_map_mux);
    group_pref_.save(&st);
}

void DaliComponent::register_input_sensor(DaliBusSensor *sensor)
{
    if (s_sensor_count < MAX_INPUT_SENSORS) {
        SensorEntry &e = s_sensor_registry[s_sensor_count++];
        e.sensor      = sensor;
        e.seq_in_flight.store(false, std::memory_order_relaxed);
        e.poll_requested.store(false, std::memory_order_relaxed);
    } else {
        ESP_LOGW(TAG, "sensor registry full — increase MAX_INPUT_SENSORS");
    }
}

void DaliComponent::add_dispatch_entry(uint8_t frame_kind, uint8_t address_kind,
                                       uint8_t address, uint16_t event_information,
                                       uint8_t instance, uint8_t output_type,
                                       uint8_t output_address, uint8_t action,
                                       uint8_t scene)
{
    if (s_dispatch_count >= MAX_DISPATCH_ENTRIES) {
        ESP_LOGW(TAG, "dispatch table full — increase MAX_DISPATCH_ENTRIES");
        return;
    }
    DaliDispatchEntry &e = s_dispatch_table[s_dispatch_count++];
    e.key.frame_kind   = static_cast<DaliEventFrameKind>(frame_kind);
    e.key.address_kind = static_cast<DaliEventAddressKind>(address_kind);
    e.key.address      = address;
    e.key.event_information = event_information;
    e.key.instance     = instance;
    e.output.type      = static_cast<DaliAddressType>(output_type);
    e.output.address   = output_address;
    e.action           = static_cast<DaliDispatchAction>(action);
    e.scene            = scene;
}

void DaliComponent::start_refresh()
{
    dali_refresh_cursor_request(&refresh_cursor_);
}

void DaliComponent::pump_refresh()
{
    if (scan_running_.load(std::memory_order_acquire) ||
        s_refresh_query_in_flight_.load(std::memory_order_acquire)) {
        return;
    }

    uint8_t i;
    while (dali_refresh_cursor_current(&refresh_cursor_, s_light_count, &i)) {
        LightEntry *e = &s_light_registry[i];
        uint8_t     qa = 0xFFu;
        if (e->target_type == (uint8_t)DALI_ADDR_GROUP) {
            qa = pick_group_member(e->target_address);
        } else {
            qa = e->light->get_query_address();
        }
        if (qa == 0xFFu) {
            dali_refresh_cursor_complete(&refresh_cursor_, s_light_count,
                                         DALI_REFRESH_ADVANCE);
            continue;
        }

        DaliTarget qt;
        qt.type    = DALI_ADDR_SHORT;
        qt.address = qa;
        s_refresh_query_in_flight_.store(true, std::memory_order_release);
        DaliError err =
            dali_control_query(qt, DALI_CMD_QUERY_ACTUAL_LEVEL, 0u,
                               on_level_query_reply, e);
        if (err == DALI_OK) {
            refresh_queue_blocked_ = false;
            dali_refresh_cursor_complete(&refresh_cursor_, s_light_count,
                                         DALI_REFRESH_ADVANCE);
            ESP_LOGD(TAG, "query actual level: light target type=%u addr=%u via short %u",
                     (unsigned)e->target_type, (unsigned)e->target_address,
                     (unsigned)qa);
            return;
        }

        s_refresh_query_in_flight_.store(false, std::memory_order_release);
        if (err == DALI_ERR_QUEUE_FULL) {
            dali_refresh_cursor_complete(&refresh_cursor_, s_light_count,
                                         DALI_REFRESH_RETRY);
            if (!refresh_queue_blocked_) {
                ESP_LOGD(TAG, "light refresh paused: scheduler queue full");
                refresh_queue_blocked_ = true;
            }
            return;
        }

        refresh_queue_blocked_ = false;
        dali_refresh_cursor_complete(&refresh_cursor_, s_light_count,
                                     DALI_REFRESH_ADVANCE);
        ESP_LOGW(TAG, "light refresh rejected: target type=%u addr=%u result=%d",
                 (unsigned)e->target_type, (unsigned)e->target_address, (int)err);
        return;
    }
    refresh_queue_blocked_ = false;
}

void DaliComponent::start_scan()
{
    if (scan_running_.exchange(true)) {
        ESP_LOGW(TAG, "Scan already in progress");
        return;
    }
    if (scan_status_) scan_status_->publish_state("Scanning...");
    if (!dali_scan_start(this)) {
        scan_running_.store(false);
        if (scan_status_) scan_status_->publish_state("Scan start failed");
    }
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
    s_find_couplers_stopped_ack_.store(true, std::memory_order_relaxed);
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
