#include "dali_scan.h"
#include "dali_component.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" {
#include "../../../components/dali/dali_discovery.h"
#include "../../../components/dali/dali_scheduler.h"
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
    bool         in_use;
    bool         complete;
};

static portMUX_TYPE  s_scan_mux  = portMUX_INITIALIZER_UNLOCKED;
static ScanSyncCtx   s_scan_slot = {};

static void scan_sync_cb(DaliError result, const DaliFrame *reply, void *raw_ctx) {
    ScanSyncCtx *ctx = static_cast<ScanSyncCtx *>(raw_ctx);
    TaskHandle_t notify_task = nullptr;

    taskENTER_CRITICAL(&s_scan_mux);
    ctx->result    = result;
    ctx->has_reply = (reply != nullptr);
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

static bool scan_sync_complete() {
    bool done;
    taskENTER_CRITICAL(&s_scan_mux);
    done = s_scan_slot.complete;
    taskEXIT_CRITICAL(&s_scan_mux);
    return done;
}

static DaliError scan_sync_transact(const DaliFrame *frame,
                                    bool             needs_reply,
                                    uint8_t          retries_left,
                                    bool             send_twice,
                                    DaliFrame       *reply_out,
                                    void *           /*transport_ctx*/) {
    // Guard against a previous transaction's callback not having fired yet.
    // Under normal conditions in_use clears within microseconds of a timeout.
    taskENTER_CRITICAL(&s_scan_mux);
    if (s_scan_slot.in_use) {
        taskEXIT_CRITICAL(&s_scan_mux);
        return DALI_ERR_BUSY;
    }
    s_scan_slot.waiting_task = xTaskGetCurrentTaskHandle();
    s_scan_slot.result       = DALI_ERR_TIMEOUT;
    s_scan_slot.reply        = {};
    s_scan_slot.has_reply    = false;
    s_scan_slot.in_use       = true;
    s_scan_slot.complete     = false;
    taskEXIT_CRITICAL(&s_scan_mux);

    // Flush any stale task notification before we start waiting.
    ulTaskNotifyTake(pdTRUE, 0);

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
        taskENTER_CRITICAL(&s_scan_mux);
        s_scan_slot.in_use = false;
        taskEXIT_CRITICAL(&s_scan_mux);
        return enq;
    }

    // Wait up to 500 ms. Re-check complete after each wakeup to handle
    // spurious notifications. 500 ms >> longest DALI round-trip (~50 ms).
    const TickType_t wait_ticks = pdMS_TO_TICKS(500);
    const TickType_t start      = xTaskGetTickCount();
    while (!scan_sync_complete()) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= wait_ticks) break;
        ulTaskNotifyTake(pdTRUE, wait_ticks - elapsed);
    }

    // Read result and perform two-phase release under the spinlock.
    DaliError result = DALI_ERR_TIMEOUT;
    DaliFrame reply  = {};
    bool      has_reply = false;
    bool      completed = false;

    taskENTER_CRITICAL(&s_scan_mux);
    completed = s_scan_slot.complete;
    if (completed) {
        result    = s_scan_slot.result;
        has_reply = s_scan_slot.has_reply;
        reply     = s_scan_slot.reply;
        s_scan_slot.waiting_task = nullptr;
        s_scan_slot.in_use       = false;
        s_scan_slot.complete     = false;
    } else {
        // Timed out. Clear waiting_task so the callback knows not to notify us
        // and will clear in_use itself when it eventually fires.
        s_scan_slot.waiting_task = nullptr;
    }
    taskEXIT_CRITICAL(&s_scan_mux);

    if (completed && reply_out && has_reply) *reply_out = reply;
    return result;
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

        const char *kind = d->has_input_device ? "input_device"
                         : d->has_device_type  ? "control_gear"
                                               : "unknown";

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
            if (d->has_input_device) continue;
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
            if (!d || !d->present || !d->has_groups || d->has_input_device) continue;
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

    static DaliDiscoveryInventory inventory;  // static: ~7 KB, keep off task stack

    DaliDiscoveryTransport transport = {
        .transact = scan_sync_transact,
        .ctx      = nullptr,
    };

    ESP_LOGI(TAG, "Scanning DALI bus...");

    uint8_t found = 0;
    DaliError err = dali_discovery_scan(&inventory, &transport, nullptr, nullptr, &found);

    if (err != DALI_OK) {
        ESP_LOGE(TAG, "Scan failed: %d", (int)err);
        component->on_scan_complete(0, false);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "---[ DALI Scan Complete: %u device(s) found ]---", (unsigned)found);
    log_inventory_json(&inventory);
    build_and_publish_yaml(&inventory, component->get_coupler_group_mask(), component);

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
            if (d->has_input_device) { input_count++; continue; }
            gear_count++;
            if (d->has_groups) groups_seen |= d->groups;
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

        snprintf(summary, sizeof(summary), "%u gear | grp: %s | %u input | YAML in sensor",
                 (unsigned)gear_count,
                 grp_buf[0] ? grp_buf : "-",
                 (unsigned)input_count);

        component->set_scan_result_pending(summary);
    }

    component->on_scan_complete(found, true);
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public entry point — called from ESPHome loop task (Core 0).
// ---------------------------------------------------------------------------

void dali_scan_start(DaliComponent *component) {
    ScanTaskArgs *args = new ScanTaskArgs{component};
    // Pin to Core 1 alongside the DALI task so the sync wait doesn't starve
    // the ESPHome main loop on Core 0.
    xTaskCreatePinnedToCore(scan_task, "dali_scan", 8192, args, 9, nullptr, 1);
}

}  // namespace dali
}  // namespace esphome
