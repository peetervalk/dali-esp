#include "dali_scan.h"
#include "dali_component.h"
#include "dali_core_affinity.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

extern "C" {
#include "../../../components/dali/dali_discovery.h"
#include "../../../components/dali/dali_scheduler.h"
#include "../../../components/dali/dali_transport.h"
#include "../../../components/dali/dali_group_map.h"
}

namespace esphome {
namespace dali {

static const char *TAG = "dali.scan";

// ---------------------------------------------------------------------------
// Sync transport — mirrors diag_sched_sync() in dali_diag.c.
//
// Two-phase slot release prevents use-after-timeout corruption:
//   • On timeout the caller clears waiting_task (under the spinlock) but
//     leaves in_use = true so the slot cannot be reused.
//   • The callback checks waiting_task under the spinlock: if it is null the
//     caller already timed out, so the callback just clears in_use and returns
//     without calling xTaskNotifyGive.
//   • Only when both sides agree (callback sets complete = true while
//     waiting_task is still set) does the caller read the result and clear
//     in_use on its own.
//
// This guarantees the callback never writes into a slot that a new transaction
// is already using.
// ---------------------------------------------------------------------------

struct ScanSyncCtx {
    TaskHandle_t waiting_task;
    DaliError    result;
    DaliFrame    reply;
    bool         has_reply;
    DaliSequenceResult sequence;
    bool         has_sequence;
    bool         in_use;
    bool         complete;
};

static portMUX_TYPE  s_scan_mux  = portMUX_INITIALIZER_UNLOCKED;
static ScanSyncCtx   s_scan_slot = {};

static void scan_sync_cb(DaliError result, const DaliFrame *reply, void *raw_ctx) {
    ScanSyncCtx *ctx = static_cast<ScanSyncCtx *>(raw_ctx);
    TaskHandle_t notify_task = nullptr;

    taskENTER_CRITICAL(&s_scan_mux);
    ctx->result       = result;
    ctx->has_reply    = (reply != nullptr);
    ctx->has_sequence = false;
    if (reply) ctx->reply = *reply;
    ctx->complete  = true;
    if (ctx->waiting_task != nullptr) {
        notify_task = ctx->waiting_task;
    } else {
        // Caller timed out and cleared waiting_task; release slot here.
        ctx->in_use = false;
    }
    taskEXIT_CRITICAL(&s_scan_mux);

    if (notify_task) xTaskNotifyGive(notify_task);
}

static void scan_sync_sequence_cb(const DaliSequenceResult *result, void *raw_ctx) {
    ScanSyncCtx *ctx = static_cast<ScanSyncCtx *>(raw_ctx);
    TaskHandle_t notify_task = nullptr;

    taskENTER_CRITICAL(&s_scan_mux);
    ctx->result       = (result != nullptr) ? result->result : DALI_ERR_INVALID;
    ctx->has_sequence = (result != nullptr);
    if (result != nullptr) ctx->sequence = *result;
    ctx->has_reply = false;
    ctx->complete  = true;
    if (ctx->waiting_task != nullptr) {
        notify_task = ctx->waiting_task;
    } else {
        ctx->in_use = false;
    }
    taskEXIT_CRITICAL(&s_scan_mux);

    if (notify_task) xTaskNotifyGive(notify_task);
}

static bool scan_sync_complete() {
    bool done;
    taskENTER_CRITICAL(&s_scan_mux);
    done = s_scan_slot.complete;
    taskEXIT_CRITICAL(&s_scan_mux);
    return done;
}

// Reserve the shared slot for this task. False when a previous transaction's
// callback has not released it yet.
static bool scan_sync_acquire() {
    taskENTER_CRITICAL(&s_scan_mux);
    if (s_scan_slot.in_use) {
        taskEXIT_CRITICAL(&s_scan_mux);
        return false;
    }
    s_scan_slot.waiting_task = xTaskGetCurrentTaskHandle();
    s_scan_slot.result       = DALI_ERR_TIMEOUT;
    s_scan_slot.reply        = {};
    s_scan_slot.has_reply    = false;
    s_scan_slot.sequence     = {};
    s_scan_slot.sequence.failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
    s_scan_slot.has_sequence = false;
    s_scan_slot.in_use       = true;
    s_scan_slot.complete     = false;
    taskEXIT_CRITICAL(&s_scan_mux);

    // Flush any stale task notification before we start waiting.
    ulTaskNotifyTake(pdTRUE, 0);
    return true;
}

// Give the slot back when the work was never handed to the scheduler.
static void scan_sync_release_unused() {
    taskENTER_CRITICAL(&s_scan_mux);
    s_scan_slot.in_use = false;
    taskEXIT_CRITICAL(&s_scan_mux);
}

// Block until the callback runs or the budget expires, then take the slot's
// contents. Returns false on timeout, leaving the slot reserved for the late
// callback to release.
static bool scan_sync_wait(uint32_t wait_ms, ScanSyncCtx *taken) {
    const TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
    const TickType_t start      = xTaskGetTickCount();
    while (!scan_sync_complete()) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= wait_ticks) break;
        ulTaskNotifyTake(pdTRUE, wait_ticks - elapsed);
    }

    bool completed = false;
    taskENTER_CRITICAL(&s_scan_mux);
    completed = s_scan_slot.complete;
    if (completed) {
        *taken = s_scan_slot;
        s_scan_slot.waiting_task = nullptr;
        s_scan_slot.in_use       = false;
        s_scan_slot.complete     = false;
    } else {
        // Clear waiting_task so the callback knows not to notify us and will
        // clear in_use itself when it eventually fires.
        s_scan_slot.waiting_task = nullptr;
    }
    taskEXIT_CRITICAL(&s_scan_mux);
    return completed;
}

static void scan_sequence_result_init(DaliSequenceResult *result, DaliError error) {
    if (result == nullptr) return;
    *result = {};
    result->result      = error;
    result->failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
}

// One DALI round-trip is ~50 ms; 500 ms leaves ample headroom for a single frame.
static constexpr uint32_t SCAN_SYNC_FRAME_WAIT_MS = 500u;
static constexpr uint32_t SCAN_DRAIN_WAIT_MS = 5000u;

static bool scan_wait_for_quiescent_scheduler() {
    uint32_t started_ms = millis();
    while (!dali_sched_is_quiescent()) {
        if ((uint32_t)(millis() - started_ms) >= SCAN_DRAIN_WAIT_MS) {
            return false;
        }
        vTaskDelay(1u);
    }
    return true;
}

static DaliError scan_sync_transact(const DaliFrame *frame,
                                    bool             needs_reply,
                                    uint8_t          retries_left,
                                    bool             send_twice,
                                    DaliFrame       *reply_out,
                                    void *           /*transport_ctx*/) {
    if (frame == nullptr) return DALI_ERR_INVALID;

    // Guard against a previous transaction's callback not having fired yet.
    // Under normal conditions in_use clears within microseconds of a timeout.
    if (!scan_sync_acquire()) return DALI_ERR_BUSY;

    DaliTransaction txn = {
        .frame        = *frame,
        .needs_reply  = needs_reply,
        .send_twice   = send_twice,
        .retries_left = retries_left,
        .on_complete  = scan_sync_cb,
        .cb_ctx       = &s_scan_slot,
    };

    DaliError enq = dali_sched_enqueue(&txn);
    if (enq != DALI_OK) {
        scan_sync_release_unused();
        return enq;
    }

    ScanSyncCtx taken = {};
    if (!scan_sync_wait(SCAN_SYNC_FRAME_WAIT_MS, &taken)) return DALI_ERR_TIMEOUT;

    if (reply_out != nullptr && taken.has_reply) *reply_out = taken.reply;
    return taken.result;
}

// Atomic-group half of the transport: the whole sequence occupies the scheduler
// with nothing interleaved, which is what discovery and memory reads need so a
// DTR setup cannot be redirected before the command that consumes it.
static DaliError scan_sync_sequence_transact(const DaliSequence *seq,
                                             DaliSequenceResult *result_out,
                                             void *              /*transport_ctx*/) {
    scan_sequence_result_init(result_out, DALI_ERR_INVALID);
    if (seq == nullptr) return DALI_ERR_INVALID;

    if (!scan_sync_acquire()) {
        scan_sequence_result_init(result_out, DALI_ERR_BUSY);
        return DALI_ERR_BUSY;
    }

    DaliSequence local = *seq;
    local.on_complete  = scan_sync_sequence_cb;
    local.cb_ctx       = &s_scan_slot;

    DaliError enq = dali_sched_enqueue_sequence(&local);
    if (enq != DALI_OK) {
        scan_sync_release_unused();
        scan_sequence_result_init(result_out, enq);
        return enq;
    }

    ScanSyncCtx taken = {};
    if (!scan_sync_wait(dali_transport_sequence_timeout_ms(seq), &taken)) {
        scan_sequence_result_init(result_out, DALI_ERR_TIMEOUT);
        return DALI_ERR_TIMEOUT;
    }

    if (!taken.has_sequence) {
        scan_sequence_result_init(result_out, DALI_ERR_INVALID);
        return DALI_ERR_INVALID;
    }
    if (result_out != nullptr) *result_out = taken.sequence;
    return taken.sequence.result;
}

// ---------------------------------------------------------------------------
// JSON formatter — one device per log line to stay under the ~256-char limit.
// ---------------------------------------------------------------------------

static void log_inventory_json(const DaliDiscoveryInventory *inv) {
    ESP_LOGD(TAG, "---[ Inventory JSON ]---");
    ESP_LOGD(TAG, "{ \"schema_version\": 1, \"devices\": [");

    bool first = true;
    for (uint8_t addr = 0; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *d = dali_discovery_inventory_get(inv, addr);
        if (!d || !d->present) continue;

        char groups_str[64] = "[";
        bool gfirst = true;
        for (uint8_t g = 0; g < 16; g++) {
            if (d->has_groups && (d->groups & (1u << g))) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), gfirst ? "%u" : ", %u", (unsigned)g);
                strncat(groups_str, tmp, sizeof(groups_str) - strlen(groups_str) - 1);
                gfirst = false;
            }
        }
        strncat(groups_str, "]", sizeof(groups_str) - strlen(groups_str) - 1);

        const char *kind = (d->has_control_gear && d->has_input_device) ? "hybrid"
                         :  d->has_control_gear                         ? "control_gear"
                         :  d->has_input_device                         ? "input_device"
                         :                                                 "unknown";

        ESP_LOGD(TAG, "  %s{ \"address\": %u, \"groups\": %s, \"kind\": \"%s\","
                      " \"device_type\": %u, \"actual_level\": %u }",
                 first ? "" : ",",
                 (unsigned)addr, groups_str, kind,
                 (unsigned)(d->has_device_type ? d->device_type : 0xFFu),
                 (unsigned)(d->has_actual_level ? d->actual_level : 0u));
        first = false;
    }
    ESP_LOGD(TAG, "] }");
}

// ---------------------------------------------------------------------------
// Group-membership snapshot — rebuilds DaliComponent's runtime table
// (used by start_refresh() to auto-select query_address for group lights)
// from bus-verified data, superseding any YAML-seeded or console-edited state.
// ---------------------------------------------------------------------------

static bool rebuild_group_membership(const DaliDiscoveryInventory *inv,
                                     DaliComponent *component) {
    DaliGroupMap map;
    bool complete = dali_group_map_rebuild_from_inventory(&map, inv);
    if (!complete) return false;
    uint64_t observed_gear = 0u;
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *device =
            dali_discovery_inventory_get(inv, addr);
        if (device != nullptr && device->present &&
            device->has_control_gear && device->has_groups) {
            observed_gear |= (uint64_t)1u << addr;
        }
    }
    return component->set_group_membership_snapshot(map.members,
                                                     map.verified,
                                                     observed_gear);
}

// ---------------------------------------------------------------------------
// YAML snippet generator — builds string for the yaml_result sensor and logs.
// ---------------------------------------------------------------------------

static void build_and_publish_yaml(const DaliDiscoveryInventory *inv,
                                   uint16_t coupler_mask,
                                   DaliComponent *component)
{
    static char buf[2048];
    size_t pos = 0u;

#define Y(...) do { \
    int _n = snprintf(buf + pos, sizeof(buf) - pos, __VA_ARGS__); \
    if (_n > 0 && pos + (size_t)_n < sizeof(buf)) pos += (size_t)_n; \
} while (0)

    Y("light:\n");

    for (uint8_t g = 0u; g < 16u; g++) {
        char addr_list[64] = "";
        uint8_t query_addr = 0xFFu;
        bool has_gear = false;

        for (uint8_t a = 0u; a < DALI_SHORT_ADDRESS_COUNT; a++) {
            const DaliDiscoveryDeviceInfo *d = dali_discovery_inventory_get(inv, a);
            if (!d || !d->present || !d->has_groups) continue;
            if (!(d->groups & (1u << g))) continue;
            if (d->has_input_device && !d->has_control_gear) continue;
            if (query_addr == 0xFFu) query_addr = a;
            char tmp[8];
            snprintf(tmp, sizeof(tmp), has_gear ? ", %u" : "%u", (unsigned)a);
            strncat(addr_list, tmp, sizeof(addr_list) - strlen(addr_list) - 1u);
            has_gear = true;
        }

        if (!has_gear) continue;

        const char *coupler = ((coupler_mask >> g) & 1u) ? " | coupler" : "";
        Y("  - platform: dali\n");
        Y("    name: \"Group %u\"  # addresses: %s%s\n", (unsigned)g, addr_list, coupler);
        Y("    target_type: group\n");
        Y("    target_address: %u\n", (unsigned)g);
        if (query_addr != 0xFFu)
            Y("    query_address: %u\n", (unsigned)query_addr);
    }

    bool has_input = false;
    for (uint8_t a = 0u; a < DALI_SHORT_ADDRESS_COUNT; a++) {
        const DaliDiscoveryDeviceInfo *d = dali_discovery_inventory_get(inv, a);
        if (!d || !d->present || !d->has_input_device) continue;
        if (!has_input) { Y("# Input devices:\n"); has_input = true; }
        Y("#   addr %u: %u instance(s)\n",
          (unsigned)a,
          (unsigned)(d->has_instance_count ? d->instance_count : 0u));
    }

#undef Y

    /* Log line-by-line so each fits the ESP32 log buffer and can be
     * extracted with:  esphome logs ... | grep "YAML|"
     * Brief delay between lines lets the WiFi logger forward each message
     * before the next arrives — prevents the API log queue from backing up. */
    const char *p = buf;
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (nl) {
            char line[128];
            size_t len = (size_t)(nl - p);
            if (len >= sizeof(line)) len = sizeof(line) - 1u;
            memcpy(line, p, len);
            line[len] = '\0';
            ESP_LOGI(TAG, "YAML| %s", line);
            p = nl + 1u;
        } else {
            if (*p) ESP_LOGI(TAG, "YAML| %s", p);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    /* HA state is limited to 255 chars — publish a compact group→query map
     * to the sensor; the full YAML is available in the log block above. */
    char compact[128] = {};
    size_t cp = 0u;
    bool cfirst = true;
    for (uint8_t g = 0u; g < 16u; g++) {
        uint8_t qa = 0xFFu;
        bool has_gear = false;
        for (uint8_t a = 0u; a < DALI_SHORT_ADDRESS_COUNT; a++) {
            const DaliDiscoveryDeviceInfo *d = dali_discovery_inventory_get(inv, a);
            if (!d || !d->present || !d->has_groups ||
                (d->has_input_device && !d->has_control_gear)) continue;
            if (!(d->groups & (1u << g))) continue;
            if (qa == 0xFFu) qa = a;
            has_gear = true;
        }
        if (!has_gear) continue;
        const char *c = ((coupler_mask >> g) & 1u) ? "*" : "";
        int n = snprintf(compact + cp, sizeof(compact) - cp,
                         cfirst ? "g%u:q%u%s" : " g%u:q%u%s",
                         (unsigned)g, (unsigned)(qa != 0xFFu ? qa : 0u), c);
        if (n > 0 && cp + (size_t)n < sizeof(compact)) cp += (size_t)n;
        cfirst = false;
    }
    component->set_scan_yaml_pending(compact);
}

// ---------------------------------------------------------------------------
// Scan task — spawned on demand, deletes itself when done.
// ---------------------------------------------------------------------------

struct ScanTaskArgs {
    DaliComponent *component;
};

static void scan_task(void *arg) {
    ScanTaskArgs *args = static_cast<ScanTaskArgs *>(arg);
    DaliComponent *component = args->component;
    delete args;

    static DaliDiscoveryInventory inventory;  // static: several KB, keep off task stack

    DaliDiscoveryTransport transport = {
        .transact          = scan_sync_transact,
        .transact_sequence = scan_sync_sequence_transact,
        .ctx               = nullptr,
    };

    ESP_LOGI(TAG, "Scanning DALI bus...");

    if (!scan_wait_for_quiescent_scheduler()) {
        ESP_LOGE(TAG, "Scan start timed out waiting for queued work to drain");
        component->on_scan_complete(0, false, false);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t found = 0;
    DaliError err = dali_discovery_scan(&inventory, &transport, nullptr, nullptr, &found);

    if (err != DALI_OK) {
        ESP_LOGE(TAG, "Scan failed: %d", (int)err);
        component->on_scan_complete(0, false, false);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "---[ DALI Scan Complete: %u device(s) found ]---", (unsigned)found);
    log_inventory_json(&inventory);
    component->set_scan_level_profile_snapshot(&inventory);
    bool group_data_complete = rebuild_group_membership(&inventory, component);
    if (!group_data_complete) {
        ESP_LOGW(TAG, "group discovery incomplete or missed known gear; "
                      "keeping previous membership snapshot and withholding YAML");
        component->set_scan_yaml_pending(
            "# DALI scan group data incomplete; previous membership map retained. "
            "Retry the scan before using generated YAML.");
    } else {
        build_and_publish_yaml(&inventory,
                               component->get_coupler_group_mask(),
                               component);
    }

    /* Build compact summary for the scan_result text sensor.
     * Format: "16 gear | groups: 0,2,3,4,5,6,7 | 0 input | YAML in logs" */
    {
        char     summary[128]   = {};
        char     grp_buf[48]    = {};
        uint8_t  gear_count     = 0u;
        uint8_t  input_count    = 0u;
        uint16_t groups_seen    = 0u;

        for (uint8_t a = 0u; a < DALI_SHORT_ADDRESS_COUNT; a++) {
            const DaliDiscoveryDeviceInfo *d = dali_discovery_inventory_get(&inventory, a);
            if (!d || !d->present) continue;
            if (d->has_input_device) input_count++;
            if (d->has_control_gear) {
                gear_count++;
                if (d->has_groups) groups_seen |= d->groups;
            }
        }

        /* Build groups string. */
        bool gfirst = true;
        for (uint8_t g = 0u; g < 16u; g++) {
            if (!(groups_seen & (1u << g))) continue;
            char tmp[6];
            snprintf(tmp, sizeof(tmp), gfirst ? "%u" : ",%u", (unsigned)g);
            strncat(grp_buf, tmp, sizeof(grp_buf) - strlen(grp_buf) - 1u);
            gfirst = false;
        }

        if (group_data_complete) {
            snprintf(summary, sizeof(summary),
                     "%u gear | grp: %s | %u input | YAML in sensor",
                     (unsigned)gear_count,
                     grp_buf[0] ? grp_buf : "-",
                     (unsigned)input_count);
        } else {
            snprintf(summary, sizeof(summary),
                     "%u gear | %u input | GROUP DATA INCOMPLETE; map retained",
                     (unsigned)gear_count,
                     (unsigned)input_count);
        }

        component->set_scan_result_pending(summary);
    }

    component->on_scan_complete(found, true, group_data_complete);
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public entry point — called from ESPHome loop task (Core 0).
// ---------------------------------------------------------------------------

bool dali_scan_start(DaliComponent *component) {
    ScanTaskArgs *args = new (std::nothrow) ScanTaskArgs{component};
    if (args == nullptr) {
        ESP_LOGE(TAG, "failed to allocate scan task arguments");
        return false;
    }
    // Runs alongside the DALI task, off the ESPHome main loop's core wherever
    // there is a second one, so the sync wait cannot starve it.
    BaseType_t created = xTaskCreatePinnedToCore(scan_task, "dali_scan", 8192, args, 9,
                                                 nullptr, dali_worker_core());
    if (created != pdPASS) {
        delete args;
        ESP_LOGE(TAG, "failed to create scan task");
        return false;
    }
    return true;
}

}  // namespace dali
}  // namespace esphome
