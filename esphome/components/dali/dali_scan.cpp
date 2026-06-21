#include "dali_scan.h"
#include "dali_component.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" {
#include "dali_discovery.h"
#include "dali_scheduler.h"
}

namespace esphome {
namespace dali {

static const char *TAG = "dali.scan";

// ---------------------------------------------------------------------------
// Sync transport — mirrors diag_sched_sync() in dali_diag.c.
// Enqueues a frame and blocks the calling task until the scheduler completes it.
// Safe to call from any task; one call at a time per task (local stack ctx).
// ---------------------------------------------------------------------------

struct ScanSyncCtx {
    TaskHandle_t waiting_task;
    DaliError    result;
    DaliFrame    reply;
    bool         has_reply;
};

static void scan_sync_cb(DaliError result, const DaliFrame *reply, void *raw_ctx) {
    ScanSyncCtx *ctx = static_cast<ScanSyncCtx *>(raw_ctx);
    ctx->result    = result;
    ctx->has_reply = (reply != nullptr);
    if (reply) ctx->reply = *reply;
    // Notify AFTER writing result so the unblocked task sees valid data.
    xTaskNotifyGive(ctx->waiting_task);
}

static DaliError scan_sync_transact(const DaliFrame *frame,
                                    bool             needs_reply,
                                    uint8_t          retries_left,
                                    bool             send_twice,
                                    DaliFrame       *reply_out,
                                    void *           /*transport_ctx*/) {
    // Flush any stale notification that arrived before this call.
    ulTaskNotifyTake(pdTRUE, 0);

    ScanSyncCtx sync = {
        .waiting_task = xTaskGetCurrentTaskHandle(),
        .result       = DALI_ERR_TIMEOUT,
        .has_reply    = false,
    };

    DaliTransaction txn = {
        .frame        = *frame,
        .needs_reply  = needs_reply,
        .send_twice   = send_twice,
        .retries_left = retries_left,
        .on_complete  = scan_sync_cb,
        .cb_ctx       = &sync,
    };

    DaliError err = dali_sched_enqueue(&txn);
    if (err != DALI_OK) return err;

    // Block until the scheduler's completion callback wakes us.
    // 500 ms is generous — longest DALI query round-trip is ~50 ms.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) == 0) {
        return DALI_ERR_TIMEOUT;
    }

    if (reply_out && sync.has_reply) *reply_out = sync.reply;
    return sync.result;
}

// ---------------------------------------------------------------------------
// JSON formatter — one device per log line to stay under the ~256-char limit.
// ---------------------------------------------------------------------------

static void log_inventory_json(const DaliDiscoveryInventory *inv) {
    ESP_LOGI(TAG, "---[ Inventory JSON ]---");
    ESP_LOGI(TAG, "{ \"schema_version\": 1, \"devices\": [");

    bool first = true;
    for (uint8_t addr = 0; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *d = dali_discovery_inventory_get(inv, addr);
        if (!d || !d->present) continue;

        // Build groups array string inline
        char groups_str[64] = "[";
        bool gfirst = true;
        for (uint8_t g = 0; g < 16; g++) {
            if (d->has_groups && (d->groups & (1u << g))) {
                char buf[8];
                snprintf(buf, sizeof(buf), gfirst ? "%u" : ", %u", (unsigned)g);
                strncat(groups_str, buf, sizeof(groups_str) - strlen(groups_str) - 1);
                gfirst = false;
            }
        }
        strncat(groups_str, "]", sizeof(groups_str) - strlen(groups_str) - 1);

        const char *kind = d->has_input_device ? "input_device"
                         : d->has_device_type  ? "control_gear"
                                               : "unknown";

        ESP_LOGI(TAG, "  %s{ \"address\": %u, \"groups\": %s, \"kind\": \"%s\","
                      " \"device_type\": %u, \"actual_level\": %u }",
                 first ? "" : "",
                 (unsigned)addr, groups_str, kind,
                 (unsigned)(d->has_device_type ? d->device_type : 0xFFu),
                 (unsigned)(d->has_actual_level ? d->actual_level : 0u));
        if (!first) {
            // Trailing comma note: not strictly valid JSON but readable in logs
        }
        first = false;
    }
    ESP_LOGI(TAG, "] }");
}

// ---------------------------------------------------------------------------
// YAML snippet generator
// ---------------------------------------------------------------------------

static void log_yaml_snippet(const DaliDiscoveryInventory *inv) {
    ESP_LOGI(TAG, "---[ Draft ESPHome YAML — copy into your final config ]---");
    ESP_LOGI(TAG, "light:");

    // Collect which groups have control gear and which addresses are in each.
    for (uint8_t g = 0; g < 16; g++) {
        char addr_list[64] = "";
        bool has_gear = false;

        for (uint8_t addr = 0; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
            const DaliDiscoveryDeviceInfo *d = dali_discovery_inventory_get(inv, addr);
            if (!d || !d->present || !d->has_groups) continue;
            if (!(d->groups & (1u << g))) continue;
            if (d->has_input_device) continue;  // skip input devices for light entities
            char buf[8];
            snprintf(buf, sizeof(buf), has_gear ? ", %u" : "%u", (unsigned)addr);
            strncat(addr_list, buf, sizeof(addr_list) - strlen(addr_list) - 1);
            has_gear = true;
        }

        if (!has_gear) continue;

        ESP_LOGI(TAG, "  - platform: dali");
        ESP_LOGI(TAG, "    name: \"Group %u\"  # addresses: %s", (unsigned)g, addr_list);
        ESP_LOGI(TAG, "    target_type: group");
        ESP_LOGI(TAG, "    target_address: %u", (unsigned)g);
    }

    // Input devices get sensor entries (placeholder comment for now)
    bool has_input = false;
    for (uint8_t addr = 0; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        const DaliDiscoveryDeviceInfo *d = dali_discovery_inventory_get(inv, addr);
        if (!d || !d->present || !d->has_input_device) continue;
        if (!has_input) {
            ESP_LOGI(TAG, "# Input devices found — sensor entities not yet implemented:");
            has_input = true;
        }
        ESP_LOGI(TAG, "#   addr %u: %u instance(s)",
                 (unsigned)addr,
                 (unsigned)(d->has_instance_count ? d->instance_count : 0u));
    }

    ESP_LOGI(TAG, "---[ End of draft YAML ]---");
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
    log_yaml_snippet(&inventory);

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
