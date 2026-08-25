#include "dali_component.h"
#include "dali_core_affinity.h"
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
#include "../../../components/dali/dali_cli.h"
#include "../../../components/dali/dali_control.h"
#include "../../../components/dali/dali_event.h"
#include "../../../components/dali/dali_dispatch.h"
#include "../../../components/dali/dali_protocol.h"
#include "../../../components/dali/dali_input_device.h"
#include "../../../components/dali/dali_input_config.h"
#include "../../../components/dali/dali_input_poll.h"
#include "../../../components/dali/dali_memory.h"
#include "../../../components/dali/dali_group_map.h"
#include "../../../components/dali/dali_gear_dt6.h"
#include "../../../components/dali/dali_lunatone.h"
#include "../../../components/dali/dali_steinel.h"
}

namespace esphome {
namespace dali {

static const char *TAG = "dali";

/* Home Assistant rejects a state string longer than this, so any summary built
 * for a text sensor has to fit — and has to say so when it cannot. */
static constexpr size_t HA_STATE_MAX = 255u;

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
    uint32_t      profile_generation;
};

static LightEntry s_light_registry[MAX_LIGHT_ENTITIES];
static uint8_t    s_light_count = 0u;
static std::atomic<bool> s_refresh_query_in_flight_{false};

enum ProfileRefreshPhase {
    PROFILE_REFRESH_DEVICE_TYPE,
    PROFILE_REFRESH_DEVICE_TYPES,
    PROFILE_REFRESH_PROFILE,
};

struct LevelProfileCacheEntry {
    bool             valid;
    bool             curve_capability_known;
    bool             query_dt6_curve;
    DaliLevelProfile profile;
};

struct ScanLevelProfileRecord {
    bool             valid;
    bool             curve_capability_known;
    bool             query_dt6_curve;
    DaliLevelProfile profile;
};

struct ProfileRefreshQuery {
    LightEntry        *entry;
    uint8_t            query_address;
    uint32_t           generation;
    bool               query_dt6_curve;
    ProfileRefreshPhase phase;
    DaliSequenceResult result;
    std::atomic<bool>  result_ready;
    bool               active;
    bool               actual_level_pending;
};

static LevelProfileCacheEntry s_level_profile_cache[DALI_SHORT_ADDRESS_COUNT] = {};
static ScanLevelProfileRecord
    s_scan_level_profile_snapshot[DALI_SHORT_ADDRESS_COUNT] = {};
static std::atomic<bool> s_scan_level_profile_snapshot_pending_{false};
static ProfileRefreshQuery s_profile_refresh_query = {};

static DaliLevelProfile default_level_profile()
{
    return { DALI_DIM_CURVE_STANDARD, 1u, DALI_DAPC_MAX_LEVEL };
}

static DaliLevelProfile cached_level_profile(uint8_t address)
{
    if (address < DALI_SHORT_ADDRESS_COUNT &&
        s_level_profile_cache[address].valid) {
        return s_level_profile_cache[address].profile;
    }
    return default_level_profile();
}

static uint32_t begin_light_profile_update(LightEntry *entry)
{
    entry->profile_generation++;
    if (entry->profile_generation == 0u) entry->profile_generation = 1u;
    entry->light->begin_level_profile_update(entry->profile_generation);
    return entry->profile_generation;
}

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

/* Persisted-to-flash layout (see load/save_group_membership). DGP2 deliberately
 * invalidates DGP1 snapshots, which older firmware could persist from a
 * partially failed scan as if they were authoritative. */
static constexpr uint32_t GROUP_MEMBERSHIP_MAGIC = 0x44475032u;  /* "DGP2" */
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

static uint64_t group_member_mask(uint8_t group)
{
    if (group >= DALI_GROUP_COUNT) return 0u;
    portENTER_CRITICAL(&s_group_map_mux);
    uint64_t mask = s_group_map.members[group];
    portEXIT_CRITICAL(&s_group_map_mux);
    return mask;
}

/* Union of every cached member profile in `mask`; see dali_level_profile_widen
 * for why a multi-gear target takes the widest window rather than the common
 * one. Members whose profile has never been read (no scan yet) abstain rather
 * than being assumed: a guessed 1..254 would widen the result to the maximum
 * and make the union meaningless. */
static bool union_level_profile(uint64_t mask, DaliLevelProfile *out)
{
    bool any = false;
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        if (((mask >> addr) & 1ull) == 0ull) continue;
        if (!s_level_profile_cache[addr].valid) continue;
        const DaliLevelProfile &member = s_level_profile_cache[addr].profile;
        if (!any) {
            *out = member;
            any  = true;
        } else {
            (void)dali_level_profile_widen(out, &member);
        }
    }
    return any;
}

/* The window an entity's brightness is mapped through: the union across known
 * members for a multi-gear target, the queried gear's own profile otherwise. */
static DaliLevelProfile entity_level_profile(const LightEntry *entry,
                                             uint8_t query_address)
{
    DaliLevelProfile profile;
    if (entry->target_type == (uint8_t)DALI_ADDR_GROUP) {
        if (union_level_profile(group_member_mask(entry->target_address), &profile))
            return profile;
    } else if (entry->target_type == (uint8_t)DALI_ADDR_BROADCAST) {
        if (union_level_profile(~0ull, &profile)) return profile;
    }
    return cached_level_profile(query_address);
}

static void set_light_profile(LightEntry *entry, uint8_t address,
                              uint32_t generation)
{
    entry->light->set_level_profile(entity_level_profile(entry, address),
                                    generation);
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

static bool config_changes_level_profile(DaliCommandId id)
{
    /* RESET restores the gear's default MIN/MAX, so it moves the window just
     * as much as setting either limit does. */
    return id == DALI_CMD_SET_MIN_LEVEL_DTR0 ||
           id == DALI_CMD_SET_MAX_LEVEL_DTR0 ||
           id == DALI_CMD_RESET;
}

/*
 * The DT6 counterpart. SELECT DIMMING CURVE is the only command in the DT6
 * table that changes how a level maps to light output; the others set the fast
 * fade time or start a reference measurement, and leave both the curve and the
 * level window where they were.
 *
 * Matched on the builder rather than on the name so that renaming the table
 * entry cannot quietly detach the invalidation from the command.
 */
static bool dt6_changes_level_profile(const DaliCliDtCommand *spec)
{
    return spec != nullptr && spec->build == dali_dt6_select_dimming_curve;
}

/*
 * Drop cached profiles the command just invalidated.
 *
 * The refresh that follows only re-reads addresses some entity queries, so a
 * group member that is not its group's representative would otherwise keep a
 * stale window in the union until the next scan. Dropping the entry makes that
 * member abstain from the union instead of skewing it.
 */
static void forget_level_profile_cache(DaliTarget target)
{
    if (target.type == DALI_ADDR_SHORT) {
        if (target.address < DALI_SHORT_ADDRESS_COUNT)
            s_level_profile_cache[target.address].valid = false;
        return;
    }

    uint64_t mask = target.type == DALI_ADDR_GROUP
                        ? group_member_mask(target.address)
                        : ~0ull;
    for (uint8_t addr = 0u; addr < DALI_SHORT_ADDRESS_COUNT; addr++) {
        if (((mask >> addr) & 1ull) != 0ull)
            s_level_profile_cache[addr].valid = false;
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

/* ── Registry accessors for the configuration exporter ───────────────────────
 *
 * dali_config_export.cpp reconstructs the YAML from what these hold. Read-only
 * and const, so the exporter can walk them from the shell task without being
 * able to disturb registration order or the fields the DALI task touches; see
 * dali_config_export.h for why the registries themselves stay file-static.
 * ---------------------------------------------------------------------------*/

uint8_t dali_registry_light_count() { return s_light_count; }

const DaliBusLight *dali_registry_light_at(uint8_t index)
{
    return index < s_light_count ? s_light_registry[index].light : nullptr;
}

uint8_t dali_registry_sensor_count() { return s_sensor_count; }

const DaliBusSensor *dali_registry_sensor_at(uint8_t index)
{
    return index < s_sensor_count ? s_sensor_registry[index].sensor : nullptr;
}

uint8_t dali_registry_dispatch_count() { return s_dispatch_count; }

const DaliDispatchEntry *dali_registry_dispatch_at(uint8_t index)
{
    return index < s_dispatch_count ? &s_dispatch_table[index] : nullptr;
}

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

/*
 * Ask for a level re-read after a command that moved the gear without saying
 * where it landed.
 *
 * Shared by the dispatch path below and by the console's level-changing verbs.
 * Both are in the same position: the frame carries no reply, and for the fade
 * and step commands not even the sender knows the resulting level, so the only
 * way the entity learns it is to query. The arming is deliberately deferred
 * rather than immediate — see the 600 ms window in loop(), which lets a fade
 * finish before it is read and collapses a burst of step commands into one
 * refresh instead of one query per keystroke.
 *
 * A fade longer than that window is read mid-fade and corrected by the next
 * periodic poll; `cont-up`/`cont-down` are the ones that can do this.
 */
static void arm_deferred_level_query()
{
    s_deferred_query_pending_.store(true, std::memory_order_release);
}

static void notify_lights(const DaliDispatchResult *res)
{
    if (!res->has_state) {
        if (res->matched) arm_deferred_level_query();
        return;
    }
    /*
     * A broadcast result applies to every control gear on the bus, so it must
     * reach every light entity regardless of how that entity is addressed. The
     * previous form required the entity's own target type to equal the result's,
     * which meant a broadcast only ever matched broadcast entities and ordinary
     * short-address lights never saw it.
     */
    const bool broadcast = (res->target.type == DALI_ADDR_BROADCAST);
    for (uint8_t i = 0u; i < s_light_count; i++) {
        LightEntry &e = s_light_registry[i];
        bool direct = broadcast ||
                      ((e.target_type == (uint8_t)res->target.type) &&
                       (e.target_address == res->target.address));
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
    if (level == DALI_DAPC_MASK_LEVEL) {
        ESP_LOGD(TAG, "query actual level unavailable: light target type=%u addr=%u",
                 (unsigned)e->target_type, (unsigned)e->target_address);
        s_refresh_query_in_flight_.store(false, std::memory_order_release);
        return;
    }
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

static void publish_level_profile_query_result(const DaliSequenceResult *result)
{
    if (result != nullptr) {
        s_profile_refresh_query.result = *result;
    } else {
        s_profile_refresh_query.result = {};
        s_profile_refresh_query.result.result = DALI_ERR_INVALID;
        s_profile_refresh_query.result.failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
    }
    s_profile_refresh_query.result_ready.store(true, std::memory_order_release);
    s_refresh_query_in_flight_.store(false, std::memory_order_release);
}

static void on_level_profile_query_done(const DaliSequenceResult *result,
                                        void * /*ctx*/)
{
    publish_level_profile_query_result(result);
}

static void on_level_profile_device_type_done(DaliError result,
                                              const DaliFrame *reply,
                                              void * /*ctx*/)
{
    DaliSequenceResult sequence_result = {};
    sequence_result.result = result;
    sequence_result.failed_step = result == DALI_OK
                                      ? DALI_SEQUENCE_NO_FAILED_STEP
                                      : DALI_DISCOVERY_DEVICE_TYPES_STEP_FIRST;
    sequence_result.steps_run = 1u;
    if (reply != nullptr) {
        sequence_result.replies[DALI_DISCOVERY_DEVICE_TYPES_STEP_FIRST] = *reply;
        sequence_result.reply_mask = 1u << DALI_DISCOVERY_DEVICE_TYPES_STEP_FIRST;
    }
    publish_level_profile_query_result(&sequence_result);
}

static DaliError enqueue_level_profile_sequence(DaliSequence *sequence)
{
    if (sequence == nullptr) return DALI_ERR_INVALID;
    sequence->on_complete = on_level_profile_query_done;
    sequence->cb_ctx = nullptr;
    s_profile_refresh_query.result_ready.store(false, std::memory_order_relaxed);
    s_refresh_query_in_flight_.store(true, std::memory_order_release);
    DaliError err = dali_sched_enqueue_sequence(sequence);
    if (err != DALI_OK) {
        s_refresh_query_in_flight_.store(false, std::memory_order_release);
    }
    return err;
}

static DaliError enqueue_level_profile_device_type_query(uint8_t address)
{
    DaliTarget target = { DALI_ADDR_SHORT, address };
    s_profile_refresh_query.result_ready.store(false, std::memory_order_relaxed);
    s_refresh_query_in_flight_.store(true, std::memory_order_release);
    DaliError err = dali_control_query(target, DALI_CMD_QUERY_DEVICE_TYPE, 0u,
                                       on_level_profile_device_type_done, nullptr);
    if (err != DALI_OK) {
        s_refresh_query_in_flight_.store(false, std::memory_order_release);
    }
    return err;
}

static DaliError enqueue_level_profile_device_types_query(uint8_t address)
{
    DaliSequence sequence;
    DaliError err = dali_discovery_build_device_types_sequence(address, &sequence);
    if (err != DALI_OK) return err;
    return enqueue_level_profile_sequence(&sequence);
}

static DaliError enqueue_level_profile_query(uint8_t address,
                                             bool query_dt6_curve)
{
    DaliSequence sequence;
    DaliError err = dali_discovery_build_profile_sequence(address,
                                                           query_dt6_curve,
                                                           &sequence);
    if (err != DALI_OK) return err;
    return enqueue_level_profile_sequence(&sequence);
}

static DaliError enqueue_actual_level_query(LightEntry *entry, uint8_t query_address)
{
    DaliTarget query_target;
    query_target.type = DALI_ADDR_SHORT;
    query_target.address = query_address;
    s_refresh_query_in_flight_.store(true, std::memory_order_release);
    DaliError err = dali_control_query(query_target, DALI_CMD_QUERY_ACTUAL_LEVEL,
                                       0u, on_level_query_reply, entry);
    if (err != DALI_OK) {
        s_refresh_query_in_flight_.store(false, std::memory_order_release);
    }
    return err;
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

/* Sized for the longest line dali_cli_format_response() produces — a status
 * byte with all eight flags named — so a decoded reply is never clipped. Still
 * well inside the Home Assistant state limit. */
static char              s_cmd_result_str[DALI_CLI_RESPONSE_LINE_MAX] = {};
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

static void set_cmd_enqueue_result(DaliError result)
{
    if (result == DALI_OK) {
        set_cmd_result("OK");
        return;
    }
    char buf[DALI_ERROR_TEXT_MAX];
    set_cmd_result(dali_error_text(result, buf, sizeof(buf)));
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

/*
 * What the in-flight query's reply means.
 *
 * A scheduler completion carries the frame and the caller's one context word,
 * which is already spent on the command generation. A reply byte is only
 * decodable with the name and response kind of the command that asked for it,
 * so a handler records those here before it enqueues. One slot is enough: every
 * handler publishes a new generation before it writes this, and a completion
 * whose generation no longer matches is discarded without publishing anything.
 *
 * `step` is where the reply sits in a sequence — past the DTR loads and, for
 * DT6, past its ENABLE DEVICE TYPE frame. It is ignored by single-transaction
 * callers.
 */
struct PendingReply {
    char             name[24];
    DaliResponseKind kind;
    uint8_t          step;
};

static PendingReply s_pending_reply = { "reply", DALI_RESP_UINT8, 0u };

static void set_pending_reply(const char *name, DaliResponseKind kind, uint8_t step)
{
    portENTER_CRITICAL(&s_string_mux);
    strncpy(s_pending_reply.name, name, sizeof(s_pending_reply.name) - 1u);
    s_pending_reply.name[sizeof(s_pending_reply.name) - 1u] = '\0';
    s_pending_reply.kind = kind;
    s_pending_reply.step = step;
    portEXIT_CRITICAL(&s_string_mux);
}

static PendingReply pending_reply_snapshot()
{
    portENTER_CRITICAL(&s_string_mux);
    PendingReply pending = s_pending_reply;
    portEXIT_CRITICAL(&s_string_mux);
    return pending;
}

/*
 * Publish a reply decoded the way the native CLI decodes it, so a yes/no answer
 * reads as "yes" on both surfaces rather than as 255 on this one.
 */
static void set_cmd_reply_for_generation(const DaliFrame *reply, void *ctx)
{
    PendingReply pending = pending_reply_snapshot();
    char line[DALI_CLI_RESPONSE_LINE_MAX];
    dali_cli_format_response(line, sizeof(line), pending.name, pending.kind, reply);
    set_cmd_result_for_generation(line, ctx);
}

/*
 * Publish what went wrong by name. "no reply" stays the wording for a timeout —
 * it is what the documentation and existing automations expect — but every
 * other code now says what it was instead of collapsing to "err", which is the
 * difference between an operator seeing "rx activity" and seeing nothing.
 */
static void set_cmd_error_for_generation(DaliError err, void *ctx)
{
    if (err == DALI_ERR_TIMEOUT) {
        set_cmd_result_for_generation("no reply", ctx);
        return;
    }
    char buf[DALI_ERROR_TEXT_MAX];
    set_cmd_result_for_generation(dali_error_text(err, buf, sizeof(buf)), ctx);
}

static void on_cmd_query_reply(DaliError result, const DaliFrame *reply, void *ctx)
{
    if (result != DALI_OK || reply == nullptr) {
        set_cmd_error_for_generation(result, ctx);
        return;
    }
    set_cmd_reply_for_generation(reply, ctx);
}

/* Sequence form of the above: the reply is at the step the handler recorded. */
static void on_cmd_sequence_reply(const DaliSequenceResult *result, void *ctx)
{
    if (result == nullptr || result->result != DALI_OK) {
        DaliError err = result != nullptr ? result->result : DALI_ERR_INVALID;
        set_cmd_error_for_generation(err, ctx);
        return;
    }

    DaliFrame reply;
    if (!dali_sequence_result_reply(result, pending_reply_snapshot().step, &reply)) {
        set_cmd_result_for_generation("no reply", ctx);
        return;
    }
    set_cmd_reply_for_generation(&reply, ctx);
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

    char ebuf[DALI_ERROR_TEXT_MAX];
    char buf[40];
    snprintf(buf, sizeof(buf), sent_twice ? "TX2 ERR %s" : "TX ERR %s",
             dali_error_text(result, ebuf, sizeof(ebuf)));
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
     * carry, so do not guess a target for them.
     *
     * Whether an event is worth a poll is per-instance, because "event" does
     * not imply "change": some instances report on a timer and repeat the same
     * value indefinitely. For those, following every event replaces the
     * configured interval with the device's report rate and puts traffic on the
     * bus that delays other instances' events. poll_on_event: false keeps such
     * an instance on its interval. */
    if (event.frame_kind == DALI_EVENT_FRAME_INPUT_24BIT &&
        event.source.scheme == DALI_EVENT_SOURCE_DEVICE_INSTANCE &&
        event.source.has_device_address && event.source.has_instance) {
        for (uint8_t i = 0u; i < s_sensor_count; i++) {
            SensorEntry &e = s_sensor_registry[i];
            if (e.sensor->get_address()  != event.source.device_address ||
                e.sensor->get_instance() != event.source.instance) {
                continue;
            }
            /* Gated at the source rather than at admission: a request stored
             * for a sensor that never consumes it would stay set and then fire
             * on whatever admitted the next poll. */
            if (!e.sensor->get_poll_on_event()) continue;

            e.poll_requested.store(true, std::memory_order_release);
            ESP_LOGD(TAG, "event poll requested: addr=%u inst=%u info=%u raw=%06X",
                     (unsigned)event.source.device_address,
                     (unsigned)event.source.instance,
                     (unsigned)event.event_information,
                     (unsigned)event.raw.data);
        }
    }
}

/* ── DALI task (worker core; see dali_core_affinity.h) ───────────────────── */

static void dali_task(void *raw_component)
{
    DaliComponent *component = static_cast<DaliComponent *>(raw_component);
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
            /* Keep draining/observing events during a scan so the fixed queue
             * cannot overflow, but do not let headless actions enqueue local
             * traffic into the scan. Such actions are intentionally suppressed,
             * not replayed later when their physical context may be stale. */
            bool scan_active = component != nullptr && component->is_scan_running();
            if (s_dispatch_count > 0u && !scan_active) {
                char event_str[48];
                format_event(event_str, sizeof(event_str), &rec.event);
                DaliDispatchResult result = {};
                DaliError err = dali_dispatch(s_dispatch_table, s_dispatch_count,
                                              &rec.event, &s_toggle_state, &result);
                if (err == DALI_ERR_QUEUE_FULL || err == DALI_ERR_BUSY) {
                    /* A matched entry was refused by the scheduler. The action
                     * is dropped, not deferred: replaying it later would act on
                     * a stale physical context. Report it rather than hide it. */
                    ESP_LOGW(TAG, "dispatch action dropped: %s (%s)",
                             err == DALI_ERR_QUEUE_FULL ? "queue full"
                                                        : "scheduler busy",
                             event_str);
                } else if (err != DALI_OK) {
                    /* No usable mapping for this frame — expected traffic on a
                     * mixed bus, so it stays at debug level. */
                    ESP_LOGD(TAG, "dispatch: no mapping, err %d (%s)",
                             (int)err, event_str);
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

    /* Nothing below this point works without the task: it is what drives RX
     * decoding and every scheduler transaction. A failed creation must fail the
     * component rather than leave a silent, permanently idle bus. */
    if (xTaskCreatePinnedToCore(dali_task, "dali", 4096, this, 10, nullptr,
                                dali_worker_core()) != pdPASS) {
        ESP_LOGE(TAG, "failed to create DALI task");
        mark_failed();
        return;
    }

    if (scan_status_) scan_status_->publish_state("Idle");
    if (bus_fault_)   bus_fault_->publish_state("OK");

    ESP_LOGI(TAG, "DALI initialized (TX GPIO%d, RX GPIO%d)", tx_pin_, rx_pin_);
}

void DaliComponent::loop()
{
    /* ── Bus state from snooping / query replies ── */
    /* flush_pending_write() also collects the scheduler completion for a
     * light's in-flight command, so it runs during a scan too; the entity
     * applies the scan gate itself before admitting any new traffic. */
    for (uint8_t i = 0u; i < s_light_count; i++) {
        s_light_registry[i].light->apply_bus_state();
        s_light_registry[i].light->flush_pending_write();
    }

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
    /* Keep recurring and event-requested sensor polls pending until the scan
     * releases admission. Due timestamps are not advanced while paused. */
    if (!scan_running_.load(std::memory_order_acquire)) {
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

    /* ── A shell workflow put the bus down; re-read what it may have moved ── */
    if (external_refresh_request_.exchange(false, std::memory_order_acq_rel))
        dali_refresh_cursor_request(&refresh_cursor_);

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
    update_bus_fault_();

    /* ── Scheduler queue drops ── */
    report_queue_drops_();

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
        if (scan_running_.load(std::memory_order_acquire)) {
            if (!identify_scan_paused_) {
                identify_scan_paused_   = true;
                identify_scan_pause_ms_ = now;
            }
        } else {
            if (identify_scan_paused_) {
                uint32_t paused_ms = now - identify_scan_pause_ms_;
                identify_start_ms_ += paused_ms;
                identify_last_ms_  += paused_ms;
                identify_scan_paused_ = false;
            }
            if ((uint32_t)(now - identify_last_ms_) >= 500u) {
                DaliTarget t;
                t.type    = DALI_ADDR_SHORT;
                t.address = diag_address_;
                bool next_phase = !identify_phase_;
                DaliError err = next_phase ? dali_control_recall_max(t)
                                           : dali_control_recall_min(t);
                if (err == DALI_OK) {
                    identify_last_ms_ = now;
                    identify_phase_   = next_phase;
                } else {
                    /* Retain the phase and the deadline so the next loop retries
                     * this half-blink; advancing on a rejected enqueue would
                     * silently drop it and stall the blink at one level. */
                    ESP_LOGW(TAG, "identify enqueue failed: %d; retrying", (int)err);
                }
            }
            if ((uint32_t)(now - identify_start_ms_) >= 10000u) {
                identify_active_ = false;
                identify_scan_paused_ = false;
            }
        }
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
                /* Home Assistant caps a state string at 255 characters, and the
                 * old 128-byte buffer silently dropped whatever did not fit —
                 * so a busy bus produced a summary that looked complete and was
                 * not. Fit as many entries as the budget allows and say plainly
                 * how many were left out; every captured frame is logged below
                 * regardless, which is the complete record. */
                char buf[HA_STATE_MAX] = {};
                size_t pos = 0u;
                uint8_t shown = 0u;

                for (uint8_t i = 0u; i < cnt; i++) {
                    const DaliInputEvent &event = s_coupler_frames[i];
                    char entry[48];
                    format_event(entry, sizeof(entry), &event);
                    ESP_LOGI(TAG, "coupler[%u] %s raw=%0*X", (unsigned)i,
                             entry, event.raw.bit_length == 24u ? 6 : 4,
                             (unsigned)event.raw.data);

                    /* Reserve room for the worst-case "; +NN more" suffix so a
                     * truncation can always be reported. */
                    const size_t reserve = 12u;
                    size_t need = strlen(entry) + (shown == 0u ? 0u : 2u);
                    if (pos + need + reserve >= sizeof(buf)) continue;

                    if (shown > 0u) {
                        buf[pos++] = ';';
                        buf[pos++] = ' ';
                    }
                    memcpy(buf + pos, entry, strlen(entry));
                    pos += strlen(entry);
                    buf[pos] = '\0';
                    shown++;
                }

                if (shown < cnt) {
                    snprintf(buf + pos, sizeof(buf) - pos, "%s+%u more",
                             shown > 0u ? "; " : "", (unsigned)(cnt - shown));
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
    bool scan_data_complete = scan_data_complete_.load();
    if (scan_status_) {
        if (scan_success) {
            std::string state =
                "Found " + std::to_string(scan_count_.load()) + " devices";
            if (!scan_data_complete) state += "; group data incomplete";
            scan_status_->publish_state(state);
        } else {
            scan_status_->publish_state("Scan error");
        }
    }
    if (scan_success) {
        apply_scan_level_profile_snapshot_();
        start_refresh();
    }
}

/* ── Public methods ──────────────────────────────────────────────────────── */

/* ── execute_command ─────────────────────────────────────────────────────── */

/* ── Console command table ───────────────────────────────────────────────── */

/*
 * The console and the native CLI reach the same bus through the same protocol
 * stack, so they share one tokenizer, one set of argument parsers, and one set
 * of named command tables (dali_cli.h). What they do not share is the verb
 * list: there is no terminal here to print a scan, a capture, or an inventory
 * into, and the result of a command is a single Home Assistant text state.
 *
 * This table is therefore the subset that is meaningful through that surface,
 * spelled exactly as the native CLI spells it. Sharing the spec struct is what
 * gives the console its argument-count checking: dali_cli_resolve_in() enforces
 * both bounds before a handler runs, so a trailing token is now an error rather
 * than something quietly ignored.
 *
 * What is deliberately absent, and why:
 *
 *   help, list, stats, bus, capture, trace, read, rxdebug, reset
 *       Their answer is a block of lines. There is one text state here.
 *   scan, discover, inventory, export, identify
 *       Already exposed as buttons and text sensors, which is the shape Home
 *       Assistant can actually use.
 *   commission
 *       No guarded workflow exists here; `special` refuses its primitives for
 *       the same reason (see console_special_).
 *   meminfo, instances, sensor poll
 *       Each needs a blocking transport to walk a device before it knows what
 *       to ask next. Everything on this surface is one enqueue and one
 *       completion, and Core 0's loop may not block. The scan covers the first
 *       two; the sensor platform covers the third.
 *   dt8
 *       Implemented in the shared stack and reachable from the native CLI, held
 *       back here until it has been run against real DT8 gear.
 *
 * `group` carries DALI_CLI_CMD_COUNT because it has no shared id: it edits the
 * group-membership cache, which exists only in this integration.
 */
static const DaliCliCommandSpec s_console_commands[] = {
    { DALI_CLI_CMD_QUEUE, "queue", "[reset]", "scheduler queue admission diagnostics", 0u, 1u, "reset" },

    { DALI_CLI_CMD_RAW,  "raw",  "<hex> len=<bits> [wait]", "send one arbitrary frame", 2u, 3u, NULL },
    { DALI_CLI_CMD_RAW2, "raw2", "<hex> len=<bits>", "send one frame twice within the 100 ms window", 2u, 2u, NULL },
    { DALI_CLI_CMD_DTR,  "dtr",  "<0|1|2> <0-255>", "load a control-gear DTR register", 2u, 2u, NULL },

    { DALI_CLI_CMD_LEVEL,      "level",      DALI_CLI_TARGET_ARG " <0-254|mask>", "direct arc power control", 2u, 2u, NULL },
    { DALI_CLI_CMD_MASK,       "mask",       DALI_CLI_TARGET_ARG, "arc power MASK: leave the level unchanged", 1u, 1u, NULL },
    { DALI_CLI_CMD_OFF,        "off",        DALI_CLI_TARGET_ARG, "OFF", 1u, 1u, NULL },
    { DALI_CLI_CMD_UP,         "up",         DALI_CLI_TARGET_ARG, "UP: one fade step up", 1u, 1u, NULL },
    { DALI_CLI_CMD_DOWN,       "down",       DALI_CLI_TARGET_ARG, "DOWN: one fade step down", 1u, 1u, NULL },
    { DALI_CLI_CMD_STEP_UP,    "step-up",    DALI_CLI_TARGET_ARG, "STEP UP", 1u, 1u, NULL },
    { DALI_CLI_CMD_STEP_DOWN,  "step-down",  DALI_CLI_TARGET_ARG, "STEP DOWN", 1u, 1u, NULL },
    { DALI_CLI_CMD_STEP_OFF,   "step-off",   DALI_CLI_TARGET_ARG, "STEP DOWN AND OFF", 1u, 1u, NULL },
    { DALI_CLI_CMD_ON_STEP,    "on-step",    DALI_CLI_TARGET_ARG, "ON AND STEP UP", 1u, 1u, NULL },
    { DALI_CLI_CMD_CONT_UP,    "cont-up",    DALI_CLI_TARGET_ARG, "CONTINUOUS UP: fade toward max", 1u, 1u, NULL },
    { DALI_CLI_CMD_CONT_DOWN,  "cont-down",  DALI_CLI_TARGET_ARG, "CONTINUOUS DOWN: fade toward min", 1u, 1u, NULL },
    { DALI_CLI_CMD_DAPC_SEQ,   "dapc-seq",   DALI_CLI_TARGET_ARG, "ENABLE DAPC SEQUENCE", 1u, 1u, NULL },
    { DALI_CLI_CMD_LAST,       "last",       DALI_CLI_TARGET_ARG, "GO TO LAST ACTIVE LEVEL", 1u, 1u, NULL },
    { DALI_CLI_CMD_SCENE,      "scene",      DALI_CLI_TARGET_ARG " <0-15>", "GO TO SCENE", 2u, 2u, NULL },
    { DALI_CLI_CMD_MAX,        "max",        DALI_CLI_TARGET_ARG, "RECALL MAX LEVEL", 1u, 1u, NULL },
    { DALI_CLI_CMD_MIN,        "min",        DALI_CLI_TARGET_ARG, "RECALL MIN LEVEL", 1u, 1u, NULL },
    { DALI_CLI_CMD_STATUS,     "status",     DALI_CLI_TARGET_ARG, "QUERY STATUS with decoded flags", 1u, 1u, NULL },

    { DALI_CLI_CMD_QUERY,       "query",       DALI_CLI_TARGET_ARG " <query-name> [param]", "addressed control-gear query", 2u, 3u, NULL },
    { DALI_CLI_CMD_SPECIAL,     "special",     "<name> [param]", "special/broadcast command", 1u, 2u, NULL },
    { DALI_CLI_CMD_CONFIG,      "config",      DALI_CLI_TARGET_ARG " <config-name> [param]", "addressed configuration command", 2u, 3u, NULL },
    { DALI_CLI_CMD_CONFIG_DTR0, "config-dtr0", DALI_CLI_TARGET_ARG " <config-name> <dtr0> [param]", "load DTR0 and configure atomically", 3u, 4u, NULL },

    { DALI_CLI_CMD_DT6, "dt6", "<addr> <name> [dtr0]", "device type 6 (LED) command", 2u, 3u, NULL },

    { DALI_CLI_CMD_IQUERY,  "iquery",  "<addr> <instance> <name> [dtr0]", "Part 103 instance query", 3u, 4u, NULL },
    { DALI_CLI_CMD_ICONFIG, "iconfig", "<addr> <instance> <name> [v0] [v1] [v2]", "Part 103 instance configuration", 3u, 6u, NULL },
    { DALI_CLI_CMD_VENDOR,  "vendor",  "lunatone <addr> <instance> <name> | steinel <instance> <raw>", "vendor helpers", 3u, 4u, "lunatone steinel" },

    { DALI_CLI_CMD_MEMREAD,  "memread",  "<addr> <bank> <offset> [count]", "control-gear memory read (Part 102)", 3u, 4u, NULL },
    { DALI_CLI_CMD_DEVMEM,   "devmem",   "read|write <addr> <bank> <offset> [count|value]", "control-device memory (Part 103)", 4u, 5u, "read write" },
    { DALI_CLI_CMD_DTRCHECK, "dtrcheck", "<addr> <0|1|2> <0-255>", "load a control-device DTR and read it back", 3u, 3u, NULL },

    { DALI_CLI_CMD_QUIESCENT, "quiescent", "on|off <addr|all>", "Part 103 quiescent mode: silence control-device events", 2u, 2u, "on off" },

    { DALI_CLI_CMD_COUNT, "group", "forget <addr> [group]", "retire a departed member from the group cache", 2u, 3u, "forget" },
};

static constexpr uint8_t CONSOLE_COMMAND_COUNT =
    (uint8_t)(sizeof(s_console_commands) / sizeof(s_console_commands[0]));

/* Longest console line accepted. dali_cli_tokenize() rejects anything longer
 * rather than truncating it, so a clipped line can never become a different,
 * valid command. */
static constexpr size_t CONSOLE_LINE_MAX = 96u;

static void set_cmd_usage(const DaliCliCommandSpec *spec)
{
    if (spec == nullptr) {
        set_cmd_result("bad args");
        return;
    }
    char buf[sizeof(s_cmd_result_str)];
    snprintf(buf, sizeof(buf), "usage: %s %s", spec->name, spec->args);
    set_cmd_result(buf);
}

/* ── Multi-byte memory read result (Core 1 -> Core 0) ────────────────────── */

/*
 * Shared by the control-gear (`memread`) and control-device (`devmem read`)
 * forms. Both builders lay a read sequence out the same way — DTR setup steps
 * that answer nothing, then one reply per byte — so the reporting is identical
 * even though the frames on the bus are not.
 */
static void on_memory_read_done(const DaliSequenceResult *result, void *ctx)
{
    if (result == nullptr || result->result != DALI_OK) {
        DaliError err = result != nullptr ? result->result : DALI_ERR_INVALID;
        set_cmd_error_for_generation(err, ctx);
        return;
    }

    /* One reply per read step; report every byte so a multi-byte read is not
     * silently reduced to its last value. */
    char buf[sizeof(s_cmd_result_str)];
    size_t pos = 0u;
    uint8_t reported = 0u;
    for (uint8_t step = 0u; step < DALI_SEQUENCE_MAX_STEPS; step++) {
        DaliFrame reply;
        if (!dali_sequence_result_reply(result, step, &reply)) continue;
        int n = snprintf(buf + pos, sizeof(buf) - pos, reported == 0u ? "%02X" : " %02X",
                         (unsigned)(reply.data & 0xFFu));
        if (n <= 0 || pos + (size_t)n >= sizeof(buf)) break;
        pos += (size_t)n;
        reported++;
    }
    if (reported == 0u) {
        set_cmd_result_for_generation("no reply", ctx);
        return;
    }
    set_cmd_result_for_generation(buf, ctx);
}

static void on_dtrcheck_done(const DaliSequenceResult *result, void *ctx)
{
    DaliFrame reply;
    if (result == nullptr || result->result != DALI_OK ||
        !dali_sequence_result_last_reply(result, &reply)) {
        DaliError err = result != nullptr ? result->result : DALI_ERR_INVALID;
        set_cmd_error_for_generation(err, ctx);
        return;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "read %u (0x%02X)", (unsigned)(reply.data & 0xFFu),
             (unsigned)(reply.data & 0xFFu));
    set_cmd_result_for_generation(buf, ctx);
}

/* ── Console handlers ────────────────────────────────────────────────────── */

void DaliComponent::console_queue_(const DaliCliTokens &t)
{
    if (t.count == 2u) {
        dali_sched_reset_queue_stats();
        last_queue_rejected_full_ = 0u;
        last_queue_rejected_busy_ = 0u;
    }
    DaliSchedQueueStats stats;
    if (dali_sched_queue_stats(&stats) != DALI_OK) {
        set_cmd_result("err");
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "d=%u/%u hw=%u ok=%u full=%u busy=%u",
             (unsigned)stats.depth, (unsigned)stats.capacity,
             (unsigned)stats.high_water, (unsigned)stats.admitted,
             (unsigned)stats.rejected_full, (unsigned)stats.rejected_busy);
    set_cmd_result(buf);
}

/*
 * Retire a departed group member from the runtime cache.
 *
 * This is the operator's counterpart to the scan's deliberate conservatism: a
 * scan that misses an address keeps its memberships, which is right for gear
 * that is merely offline but leaves physically removed gear in the cache
 * forever, still eligible as a group's query target. Unlike `config
 * remove-group` this sends nothing — the gear is gone and cannot answer — so it
 * only edits local bookkeeping, which is then persisted by the main loop.
 */
void DaliComponent::console_group_(const DaliCliTokens &t)
{
    uint8_t addr;
    if (!dali_cli_parse_short_addr(t.tok[2], &addr)) {
        set_cmd_result("group: addr 0-63");
        return;
    }

    uint8_t group = DALI_GROUP_MAP_ALL_GROUPS;
    if (t.count == 4u && (!dali_cli_parse_u8(t.tok[3], 15u, &group))) {
        set_cmd_result("group: 0-15");
        return;
    }

    portENTER_CRITICAL(&s_group_map_mux);
    bool changed = dali_group_map_forget(&s_group_map, addr, group);
    portEXIT_CRITICAL(&s_group_map_mux);

    if (!changed) {
        set_cmd_result("not a member");
        return;
    }
    s_group_members_dirty_.store(true, std::memory_order_release);

    char buf[48];
    if (group == DALI_GROUP_MAP_ALL_GROUPS) {
        snprintf(buf, sizeof(buf), "forgot a%u (all groups)", (unsigned)addr);
    } else {
        snprintf(buf, sizeof(buf), "forgot a%u from g%u", (unsigned)addr, (unsigned)group);
    }
    ESP_LOGI(TAG, "%s", buf);
    set_cmd_result(buf);
}

void DaliComponent::console_raw_(const DaliCliTokens &t, bool send_twice, void *ctx)
{
    DaliFrame frame;
    if (!dali_cli_parse_raw_frame(t.tok[1], t.tok[2], &frame)) {
        set_cmd_result("bad raw syntax");
        return;
    }

    bool wait_reply = false;
    if (t.count == 4u) {
        if (strcmp(t.tok[3], "wait") != 0) {
            set_cmd_result("bad raw syntax");
            return;
        }
        wait_reply = true;
    }

    DaliTransaction txn = {};
    txn.frame       = frame;
    txn.needs_reply = wait_reply;
    txn.send_twice  = send_twice;
    txn.on_complete = send_twice ? on_raw2_done : on_raw_done;
    txn.cb_ctx      = ctx;
    set_cmd_result("pending");
    set_cmd_enqueue_error(dali_sched_enqueue(&txn));
}

void DaliComponent::console_devmem_(const DaliCliTokens &t, void *ctx)
{
    bool is_write = strcmp(t.tok[1], "write") == 0;
    uint8_t addr, bank, offset;
    if (!dali_cli_parse_short_addr(t.tok[2], &addr) ||
        !dali_cli_parse_u8(t.tok[3], 255u, &bank) ||
        !dali_cli_parse_u8(t.tok[4], 255u, &offset)) {
        set_cmd_result("devmem: bad args");
        return;
    }

    DaliSequence seq;
    DaliError err;

    if (is_write) {
        uint8_t value;
        if (t.count != 6u || !dali_cli_parse_u8(t.tok[5], 255u, &value)) {
            set_cmd_result("usage: devmem write <addr> <bank> <off> <val>");
            return;
        }
        if (bank == 0u) {
            set_cmd_result("devmem: bank 0 is read-only");
            return;
        }
        err = dali_memory_build_control_device_write_sequence(addr, bank, offset,
                                                              value, &seq);
        if (err == DALI_OK) err = dali_sched_enqueue_sequence(&seq);
        /* Transmitted, not applied: nothing reads the value back. */
        set_cmd_enqueue_result(err);
        return;
    }

    uint8_t count = 1u;
    if (t.count == 6u &&
        (!dali_cli_parse_u8(t.tok[5], DALI_MEMORY_MAX_SEQUENCE_READ_BYTES, &count) ||
         count == 0u)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "devmem read count 1-%u",
                 (unsigned)DALI_MEMORY_MAX_SEQUENCE_READ_BYTES);
        set_cmd_result(buf);
        return;
    }
    if ((unsigned)offset + (unsigned)count > 256u) {
        set_cmd_result("devmem: past end of bank");
        return;
    }

    err = dali_memory_build_control_device_read_sequence(addr, bank, offset, count, &seq);
    if (err == DALI_OK) {
        seq.on_complete = on_memory_read_done;
        seq.cb_ctx      = ctx;
        set_cmd_result("pending");
        err = dali_sched_enqueue_sequence(&seq);
    }
    set_cmd_enqueue_error(err);
}

/*
 * Special (broadcast) commands.
 *
 * The commissioning primitives are refused on this Home Assistant text-command
 * surface. They can destroy the addressing of every device on the bus in one
 * line — RANDOMISE irreversibly. The shared native/TCP diagnostic shell runs
 * them inside the guarded `commission` workflow, which sequences and checks
 * them; use that surface instead.
 *
 * TERMINATE stays available on purpose: it is what closes an initialise window
 * another tool left open, and refusing it would leave an operator holding the
 * problem without the remedy.
 */
void DaliComponent::console_special_(const DaliCliTokens &t, void *ctx)
{
    const DaliCliGearCommand *spec = dali_cli_special_find(t.tok[1]);
    if (spec == nullptr) {
        set_cmd_result("unknown special");
        return;
    }
    if (dali_cli_special_is_commissioning(spec->id)) {
        set_cmd_result("commissioning special; use the native CLI");
        return;
    }

    uint8_t param = 0u;
    if (spec->needs_param) {
        if (t.count != 3u || !dali_cli_parse_u8(t.tok[2], spec->max_param, &param)) {
            char buf[sizeof(s_cmd_result_str)];
            snprintf(buf, sizeof(buf), "usage: special %s <0-%u>", spec->name,
                     (unsigned)spec->max_param);
            set_cmd_result(buf);
            return;
        }
    } else if (t.count != 2u) {
        char buf[sizeof(s_cmd_result_str)];
        snprintf(buf, sizeof(buf), "usage: special %s", spec->name);
        set_cmd_result(buf);
        return;
    }

    const DaliCommandInfo *info = dali_command_lookup(spec->id);
    if (info == nullptr || info->frame_kind != DALI_CMD_FRAME_SPECIAL) {
        set_cmd_result("err");
        return;
    }

    DaliFrame frame;
    DaliError err = dali_build_special(spec->id, param, &frame);
    if (err != DALI_OK) {
        set_cmd_enqueue_result(err);
        return;
    }

    const bool needs_reply = info->response_kind != DALI_RESP_NONE;

    DaliTransaction txn = {};
    txn.frame       = frame;
    txn.needs_reply = needs_reply;
    txn.send_twice  = info->send_twice;
    if (!needs_reply) {
        set_cmd_enqueue_result(dali_sched_enqueue(&txn));
        return;
    }

    set_pending_reply(spec->name, info->response_kind, 0u);
    txn.on_complete = on_cmd_query_reply;
    txn.cb_ctx      = ctx;
    set_cmd_result("pending");
    set_cmd_enqueue_error(dali_sched_enqueue(&txn));
}

/*
 * Control-gear (Part 102) block read.
 *
 * Named apart from `devmem` for the reason the native CLI names them apart: the
 * two use different DTR and memory opcodes, and picking the wrong one addresses
 * a different class of device entirely.
 */
void DaliComponent::console_memread_(const DaliCliTokens &t, void *ctx)
{
    uint8_t addr, bank, offset;
    if (!dali_cli_parse_short_addr(t.tok[1], &addr) ||
        !dali_cli_parse_u8(t.tok[2], 255u, &bank) ||
        !dali_cli_parse_u8(t.tok[3], 255u, &offset)) {
        set_cmd_result("memread: bad args");
        return;
    }

    uint8_t count = 1u;
    if (t.count == 5u &&
        (!dali_cli_parse_u8(t.tok[4], DALI_MEMORY_MAX_SEQUENCE_READ_BYTES, &count) ||
         count == 0u)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "memread count 1-%u",
                 (unsigned)DALI_MEMORY_MAX_SEQUENCE_READ_BYTES);
        set_cmd_result(buf);
        return;
    }
    if ((unsigned)offset + (unsigned)count > 256u) {
        set_cmd_result("memread: past end of bank");
        return;
    }

    DaliSequence seq;
    DaliError err = dali_memory_build_read_sequence(addr, bank, offset, count, &seq);
    if (err == DALI_OK) {
        seq.on_complete = on_memory_read_done;
        seq.cb_ctx      = ctx;
        set_cmd_result("pending");
        err = dali_sched_enqueue_sequence(&seq);
    }
    set_cmd_enqueue_error(err);
}

/*
 * Part 103 instance query.
 *
 * A name whose value is selected by DTR0 takes the selector as a fourth
 * argument and goes out as one sequence, because a DTR0 written by a separate
 * transaction can be replaced before the query that reads it arrives.
 */
void DaliComponent::console_iquery_(const DaliCliTokens &t, void *ctx)
{
    uint8_t addr, instance;
    if (!dali_cli_parse_short_addr(t.tok[1], &addr) ||
        !dali_cli_parse_instance(t.tok[2], &instance)) {
        set_cmd_result("iquery: addr 0-63, inst 0-31");
        return;
    }

    const DaliCliInstanceQuery *spec = dali_cli_iquery_find(t.tok[3]);
    if (spec == nullptr) {
        set_cmd_result("unknown iquery");
        return;
    }

    uint8_t dtr0 = 0u;
    if (spec->needs_dtr0) {
        if (t.count != 5u || !dali_cli_parse_u8(t.tok[4], 255u, &dtr0)) {
            char buf[sizeof(s_cmd_result_str)];
            snprintf(buf, sizeof(buf), "%s needs dtr0: %s", spec->name,
                     spec->dtr0_help != nullptr ? spec->dtr0_help : "0-255");
            set_cmd_result(buf);
            return;
        }
    } else if (t.count != 4u) {
        char buf[sizeof(s_cmd_result_str)];
        snprintf(buf, sizeof(buf), "%s takes no dtr0", spec->name);
        set_cmd_result(buf);
        return;
    }

    DaliFrame frame = spec->build(addr, instance);
    if (frame.bit_length == 0u) {
        set_cmd_result("iquery: bad args");
        return;
    }

    if (spec->needs_dtr0) {
        DaliSequence seq;
        DaliError err = dali_input_build_config_sequence(frame, false, true, &dtr0, 1u,
                                                         &seq);
        if (err == DALI_OK) {
            set_pending_reply(spec->name, spec->response_kind,
                              DALI_INPUT_CONFIG_COMMAND_STEP(1u));
            seq.on_complete = on_cmd_sequence_reply;
            seq.cb_ctx      = ctx;
            set_cmd_result("pending");
            err = dali_sched_enqueue_sequence(&seq);
        }
        set_cmd_enqueue_error(err);
        return;
    }

    set_pending_reply(spec->name, spec->response_kind, 0u);
    DaliTransaction txn = {};
    txn.frame       = frame;
    txn.needs_reply = true;
    txn.on_complete = on_cmd_query_reply;
    txn.cb_ctx      = ctx;
    set_cmd_result("pending");
    set_cmd_enqueue_error(dali_sched_enqueue(&txn));
}

/* Steinel readings are decoded, not transmitted: the conversion is the whole
 * point of the verb, and it answers without touching the bus. */
static void console_steinel_decode(uint8_t instance, uint16_t raw)
{
    const DaliSteinelInstanceInfo *info = dali_steinel_hf360_instance_lookup(instance);
    if (info == nullptr) {
        set_cmd_result("not an HF 360 II instance");
        return;
    }

    char buf[sizeof(s_cmd_result_str)];
    switch (instance) {
        case DALI_STEINEL_HF360_INSTANCE_TEMPERATURE: {
            int32_t deci = dali_steinel_temperature_deci_c(raw);
            snprintf(buf, sizeof(buf), "%s: %ld.%ld C", info->name, (long)(deci / 10),
                     (long)(deci < 0 ? -(deci % 10) : deci % 10));
            break;
        }
        case DALI_STEINEL_HF360_INSTANCE_HUMIDITY: {
            uint32_t deci = dali_steinel_humidity_deci_percent(raw);
            snprintf(buf, sizeof(buf), "%s: %lu.%lu %%", info->name,
                     (unsigned long)(deci / 10u), (unsigned long)(deci % 10u));
            break;
        }
        case DALI_STEINEL_HF360_INSTANCE_BRIGHTNESS:
            snprintf(buf, sizeof(buf), "%s: %lu.%02lu lx", info->name,
                     (unsigned long)(raw / 100u), (unsigned long)(raw % 100u));
            break;
        default:
            snprintf(buf, sizeof(buf), "%s: raw=%u", info->name, (unsigned)raw);
            break;
    }
    set_cmd_result(buf);
}

void DaliComponent::console_vendor_(const DaliCliTokens &t, void *ctx)
{
    if (strcmp(t.tok[1], "steinel") == 0) {
        uint8_t  instance;
        uint32_t raw;
        if (t.count != 4u || !dali_cli_parse_instance(t.tok[2], &instance) ||
            !dali_cli_parse_u32(t.tok[3], 0xFFFFu, &raw)) {
            set_cmd_result("usage: vendor steinel <instance> <raw>");
            return;
        }
        console_steinel_decode(instance, (uint16_t)raw);
        return;
    }

    uint8_t addr, instance;
    if (t.count != 5u || !dali_cli_parse_short_addr(t.tok[2], &addr) ||
        !dali_cli_parse_instance(t.tok[3], &instance)) {
        set_cmd_result("usage: vendor lunatone <addr> <inst> <name>");
        return;
    }

    const DaliCliLunatoneCommand *spec = dali_cli_lunatone_find(t.tok[4]);
    if (spec == nullptr) {
        set_cmd_result("unknown lunatone query");
        return;
    }

    DaliFrame frame;
    DaliError err = dali_lunatone_build_instance_command(addr, instance, spec->id,
                                                          &frame);
    if (err != DALI_OK) {
        set_cmd_enqueue_result(err);
        return;
    }

    const DaliLunatoneCommandInfo *info = dali_lunatone_command_lookup(spec->id);
    set_pending_reply(spec->name,
                      info != nullptr ? info->response_kind : DALI_RESP_UINT8, 0u);

    DaliTransaction txn = {};
    txn.frame       = frame;
    txn.needs_reply = true;
    txn.on_complete = on_cmd_query_reply;
    txn.cb_ctx      = ctx;
    set_cmd_result("pending");
    set_cmd_enqueue_error(dali_sched_enqueue(&txn));
}

/*
 * Device type 6 (IEC 62386-207).
 *
 * The command table, its names, and the DTR0 byte each one consumes are the
 * native CLI's (dali_cli_dt6_*), and the frames go out through
 * dali_dt6_build_command_sequence() so that ENABLE DEVICE TYPE 6, any DTR load,
 * and the command itself cannot be separated by other locally scheduled
 * traffic. A DT6 command that ran without its enable would be interpreted by
 * the gear as whatever its default device type says.
 */
void DaliComponent::console_dt6_(const DaliCliTokens &t, void *ctx)
{
    uint8_t addr;
    if (!dali_cli_parse_short_addr(t.tok[1], &addr)) {
        set_cmd_result("dt6: addr 0-63");
        return;
    }

    const DaliCliDtCommand *spec = dali_cli_dt6_find(t.tok[2]);
    if (spec == nullptr) {
        set_cmd_result("unknown dt6 cmd");
        return;
    }

    uint8_t dtr[DALI_DT6_MAX_DTR_BYTES] = {};
    if (t.count != (uint8_t)(3u + spec->dtr_count)) {
        char buf[sizeof(s_cmd_result_str)];
        snprintf(buf, sizeof(buf), "%s needs %u value(s)%s%s", spec->name,
                 (unsigned)spec->dtr_count,
                 spec->dtr_help != nullptr ? ": " : "",
                 spec->dtr_help != nullptr ? spec->dtr_help : "");
        set_cmd_result(buf);
        return;
    }
    for (uint8_t i = 0u; i < spec->dtr_count; i++) {
        if (!dali_cli_parse_u8(t.tok[3u + i], 255u, &dtr[i])) {
            set_cmd_result("dt6: value 0-255");
            return;
        }
    }

    DaliFrame command = spec->build(addr);
    if (command.bit_length == 0u) {
        set_cmd_result("dt6: bad args");
        return;
    }

    const bool send_twice    = spec->kind == DALI_CLI_DT_CONFIG;
    const bool expects_reply = spec->kind == DALI_CLI_DT_QUERY;

    DaliSequence seq;
    DaliError err = dali_dt6_build_command_sequence(command, send_twice, expects_reply,
                                                    dtr, spec->dtr_count, &seq);
    if (err != DALI_OK) {
        set_cmd_enqueue_result(err);
        return;
    }

    if (expects_reply) {
        /* Past the DTR loads and past ENABLE DEVICE TYPE 6. */
        set_pending_reply(spec->name, spec->response_kind,
                          (uint8_t)(spec->dtr_count + 1u));
        seq.on_complete = on_cmd_sequence_reply;
        seq.cb_ctx      = ctx;
        set_cmd_result("pending");
        set_cmd_enqueue_error(dali_sched_enqueue_sequence(&seq));
        return;
    }

    err = dali_sched_enqueue_sequence(&seq);
    if (err == DALI_OK && dt6_changes_level_profile(spec)) {
        /*
         * The dimming curve and the level window are what every brightness this
         * component sends is computed from, so a command that moves either one
         * makes the cached profile wrong in both directions. Drop it and
         * re-read, exactly as the equivalent Part 102 configuration commands do
         * — the write is fire-and-forget, so the re-read is also what tells us
         * whether the gear took it.
         */
        DaliTarget tgt{};
        tgt.type    = DALI_ADDR_SHORT;
        tgt.address = addr;
        forget_level_profile_cache(tgt);
        start_refresh();
    }
    set_cmd_enqueue_result(err);
}

void DaliComponent::console_iconfig_(const DaliCliTokens &t)
{
    uint8_t addr, instance;
    if (!dali_cli_parse_short_addr(t.tok[1], &addr) ||
        !dali_cli_parse_instance(t.tok[2], &instance)) {
        set_cmd_result("iconfig: addr 0-63, inst 0-31");
        return;
    }

    const DaliCliInstanceConfig *spec = dali_cli_iconfig_find(t.tok[3]);
    if (spec == nullptr) {
        set_cmd_result("unknown iconfig cmd");
        return;
    }

    if (t.count != (uint8_t)(4u + spec->dtr_count)) {
        char buf[sizeof(s_cmd_result_str)];
        snprintf(buf, sizeof(buf), "%s needs %u value(s)", spec->name,
                 (unsigned)spec->dtr_count);
        set_cmd_result(buf);
        return;
    }

    uint8_t dtr[DALI_INPUT_CONFIG_MAX_DTR_BYTES] = {};
    for (uint8_t i = 0u; i < spec->dtr_count; i++) {
        if (!dali_cli_parse_u8(t.tok[4u + i], 255u, &dtr[i]) ||
            !dali_cli_dtr_value_valid(&spec->dtr_range[i], dtr[i])) {
            char buf[sizeof(s_cmd_result_str)];
            snprintf(buf, sizeof(buf), "DTR%u out of range: %s", (unsigned)i,
                     spec->dtr_help != nullptr ? spec->dtr_help : "");
            set_cmd_result(buf);
            return;
        }
    }

    DaliFrame command = spec->build(addr, instance);
    if (command.bit_length == 0u) {
        set_cmd_result("iconfig: bad args");
        return;
    }

    /* The DTR loads and the command they configure go out as one sequence, so
     * no other locally scheduled frame can replace a DTR in between. */
    DaliSequence seq;
    DaliError err = dali_input_build_config_sequence(command, spec->send_twice, false,
                                                     dtr, spec->dtr_count, &seq);
    if (err == DALI_OK) err = dali_sched_enqueue_sequence(&seq);
    set_cmd_enqueue_result(err);
}

void DaliComponent::execute_command(const std::string &cmd_str)
{
    if (cmd_str.size() >= CONSOLE_LINE_MAX) {
        begin_cmd_generation();
        set_cmd_result("command too long");
        return;
    }

    DaliCliTokens tokens;
    const DaliCliCommandSpec *spec = nullptr;
    DaliCliResolveResult resolved =
        dali_cli_resolve_in(s_console_commands, CONSOLE_COMMAND_COUNT,
                            cmd_str.c_str(), &tokens, &spec);

    if (resolved == DALI_CLI_RESOLVE_EMPTY) return;

    uint32_t cmd_generation = begin_cmd_generation();
    void *cmd_ctx = cmd_generation_ctx(cmd_generation);

    switch (resolved) {
        case DALI_CLI_RESOLVE_OK:
            break;
        case DALI_CLI_RESOLVE_UNKNOWN:
            set_cmd_result("unknown verb");
            ESP_LOGD(TAG, "execute_command: %s -> unknown verb", cmd_str.c_str());
            return;
        case DALI_CLI_RESOLVE_ARITY:
            /* Both bounds are checked, so this is also what rejects a trailing
             * token instead of ignoring it. */
            set_cmd_usage(spec);
            return;
        case DALI_CLI_RESOLVE_MALFORMED:
        default:
            set_cmd_result("command too long");
            return;
    }

    /* A verb whose first argument is a fixed keyword is checked against the one
     * list in the table, so the usage line and what is accepted cannot drift. */
    if (spec->subcommands != nullptr && tokens.count >= 2u &&
        !dali_cli_has_subcommand(spec, tokens.tok[1])) {
        set_cmd_usage(spec);
        return;
    }

    /* Answered locally with no bus traffic, so unlike every other verb these
     * stay available during a scan — which is when queue pressure and a stale
     * group cache are most worth inspecting. */
    if (spec->id == DALI_CLI_CMD_QUEUE) {
        console_queue_(tokens);
        return;
    }
    if (spec->id == DALI_CLI_CMD_COUNT) {  /* group */
        console_group_(tokens);
        return;
    }
    /* `vendor steinel` is a unit conversion applied to a value the operator
     * already has, not a transmission, so it belongs with these rather than
     * behind the scan gate. `vendor lunatone` does reach the bus. */
    if (spec->id == DALI_CLI_CMD_VENDOR && strcmp(tokens.tok[1], "steinel") == 0) {
        console_vendor_(tokens, cmd_ctx);
        return;
    }

    /* Every verb below reaches the bus, so none may be admitted into a scan. */
    if (scan_running_.load(std::memory_order_acquire)) {
        set_cmd_result("scan active");
        return;
    }

    DaliTarget tgt{};
    switch (spec->id) {
        case DALI_CLI_CMD_RAW:
            console_raw_(tokens, false, cmd_ctx);
            return;
        case DALI_CLI_CMD_RAW2:
            console_raw_(tokens, true, cmd_ctx);
            return;

        case DALI_CLI_CMD_DTR: {
            uint8_t reg, value;
            if (!dali_cli_parse_u8(tokens.tok[1], 2u, &reg) ||
                !dali_cli_parse_u8(tokens.tok[2], 255u, &value)) {
                set_cmd_usage(spec);
                return;
            }
            /* Broadcast, and consumed by whichever command reads it next: a
             * DTR loaded from here is only reliable if nothing else transmits
             * in between. The verbs that need one atomically — config-dtr0,
             * iquery, iconfig, dt6 — carry their own load. */
            set_cmd_enqueue_result(dali_control_set_dtr((DaliDtrRegister)reg, value));
            return;
        }

        /*
         * One frame and no reply: the gear's level moves and a deferred refresh
         * reads it back, exactly as it does after a dim or scene dispatched from
         * a Part 103 event. ENABLE DAPC SEQUENCE is the one member of this group
         * that does not move the level — it opens a window for the DAPC commands
         * that follow — so it arms nothing.
         */
        case DALI_CLI_CMD_OFF:
        case DALI_CLI_CMD_MAX:
        case DALI_CLI_CMD_MIN:
        case DALI_CLI_CMD_UP:
        case DALI_CLI_CMD_DOWN:
        case DALI_CLI_CMD_STEP_UP:
        case DALI_CLI_CMD_STEP_DOWN:
        case DALI_CLI_CMD_STEP_OFF:
        case DALI_CLI_CMD_ON_STEP:
        case DALI_CLI_CMD_CONT_UP:
        case DALI_CLI_CMD_CONT_DOWN:
        case DALI_CLI_CMD_DAPC_SEQ:
        case DALI_CLI_CMD_LAST: {
            if (!dali_cli_parse_target(tokens.tok[1], &tgt)) {
                set_cmd_result("bad target");
                return;
            }
            DaliError err;
            switch (spec->id) {
                case DALI_CLI_CMD_OFF:       err = dali_control_off(tgt); break;
                case DALI_CLI_CMD_MAX:       err = dali_control_recall_max(tgt); break;
                case DALI_CLI_CMD_MIN:       err = dali_control_recall_min(tgt); break;
                case DALI_CLI_CMD_UP:        err = dali_control_up(tgt); break;
                case DALI_CLI_CMD_DOWN:      err = dali_control_down(tgt); break;
                case DALI_CLI_CMD_STEP_UP:   err = dali_control_step_up(tgt); break;
                case DALI_CLI_CMD_STEP_DOWN: err = dali_control_step_down(tgt); break;
                case DALI_CLI_CMD_STEP_OFF:  err = dali_control_step_down_and_off(tgt); break;
                case DALI_CLI_CMD_ON_STEP:   err = dali_control_on_and_step_up(tgt); break;
                case DALI_CLI_CMD_CONT_UP:   err = dali_control_continuous_up(tgt); break;
                case DALI_CLI_CMD_CONT_DOWN: err = dali_control_continuous_down(tgt); break;
                case DALI_CLI_CMD_DAPC_SEQ:  err = dali_control_enable_dapc_sequence(tgt); break;
                default:                     err = dali_control_go_to_last_active_level(tgt); break;
            }
            if (err == DALI_OK && spec->id != DALI_CLI_CMD_DAPC_SEQ)
                arm_deferred_level_query();
            set_cmd_enqueue_result(err);
            return;
        }

        case DALI_CLI_CMD_SCENE: {
            uint8_t scene;
            if (!dali_cli_parse_target(tokens.tok[1], &tgt)) {
                set_cmd_result("bad target");
                return;
            }
            if (!dali_cli_parse_u8(tokens.tok[2], 15u, &scene)) {
                set_cmd_result("scene 0-15");
                return;
            }
            DaliError err = dali_control_go_to_scene(tgt, scene);
            if (err == DALI_OK) arm_deferred_level_query();
            set_cmd_enqueue_result(err);
            return;
        }

        case DALI_CLI_CMD_MASK: {
            /* MASK leaves the level unchanged; it is not a DAPC value, so it
             * cannot go through dali_control_set_level(). */
            if (!dali_cli_parse_target(tokens.tok[1], &tgt)) {
                set_cmd_result("bad target");
                return;
            }
            DaliFrame frame;
            DaliError err = dali_control_build_dapc_mask(tgt, &frame);
            if (err == DALI_OK) {
                DaliTransaction txn = {};
                txn.frame = frame;
                err = dali_sched_enqueue(&txn);
            }
            set_cmd_enqueue_result(err);
            return;
        }

        case DALI_CLI_CMD_STATUS: {
            if (!dali_cli_parse_target(tokens.tok[1], &tgt)) {
                set_cmd_result("bad target");
                return;
            }
            set_pending_reply("status", DALI_RESP_STATUS, 0u);
            set_cmd_result("pending");
            set_cmd_enqueue_error(
                dali_control_query_status(tgt, on_cmd_query_reply, cmd_ctx));
            return;
        }

        case DALI_CLI_CMD_LEVEL: {
            if (!dali_cli_parse_target(tokens.tok[1], &tgt)) {
                set_cmd_result("bad target");
                return;
            }
            DaliCliLevel level;
            if (!dali_cli_parse_level(tokens.tok[2], &level)) {
                set_cmd_result("level 0-254 or mask");
                return;
            }
            if (level.is_mask) {
                /* MASK leaves the level unchanged; it is not a DAPC value, so
                 * it cannot go through dali_control_set_level(). */
                DaliFrame frame;
                DaliError err = dali_control_build_dapc_mask(tgt, &frame);
                if (err == DALI_OK) {
                    DaliTransaction txn = {};
                    txn.frame = frame;
                    err = dali_sched_enqueue(&txn);
                }
                set_cmd_enqueue_result(err);
                return;
            }
            DaliError err = dali_control_set_level(tgt, level.level);
            if (err == DALI_OK) arm_deferred_level_query();
            set_cmd_enqueue_result(err);
            return;
        }

        case DALI_CLI_CMD_QUERY: {
            if (!dali_cli_parse_target(tokens.tok[1], &tgt)) {
                set_cmd_result("bad target");
                return;
            }
            const DaliCliGearCommand *q = dali_cli_query_find(tokens.tok[2]);
            if (q == nullptr) {
                set_cmd_result("unknown query");
                return;
            }
            uint8_t param = 0u;
            if (q->needs_param) {
                if (tokens.count != 4u ||
                    !dali_cli_parse_u8(tokens.tok[3], q->max_param, &param)) {
                    set_cmd_usage(spec);
                    return;
                }
            } else if (tokens.count != 3u) {
                set_cmd_usage(spec);
                return;
            }
            /* Command metadata says how the reply decodes, so a yes/no query
             * answers "yes" here and not 255. */
            const DaliCommandInfo *info = dali_command_lookup(q->id);
            set_pending_reply(q->name,
                              info != nullptr ? info->response_kind : DALI_RESP_UINT8,
                              0u);
            set_cmd_result("pending");
            set_cmd_enqueue_error(
                dali_control_query(tgt, q->id, param, on_cmd_query_reply, cmd_ctx));
            return;
        }

        case DALI_CLI_CMD_CONFIG:
        case DALI_CLI_CMD_CONFIG_DTR0: {
            bool with_dtr0 = (spec->id == DALI_CLI_CMD_CONFIG_DTR0);
            if (!dali_cli_parse_target(tokens.tok[1], &tgt)) {
                set_cmd_result("bad target");
                return;
            }
            const DaliCliGearCommand *c = dali_cli_config_find(tokens.tok[2]);
            if (c == nullptr) {
                set_cmd_result("unknown config");
                return;
            }
            if (c->uses_dtr0 != with_dtr0) {
                set_cmd_result(with_dtr0 ? "use config for this command"
                                         : "use config-dtr0 for this command");
                return;
            }

            uint8_t first_param_tok = with_dtr0 ? 4u : 3u;
            uint8_t dtr0 = 0u;
            if (with_dtr0 && !dali_cli_parse_u8(tokens.tok[3], 255u, &dtr0)) {
                set_cmd_result("bad dtr0 value");
                return;
            }

            uint8_t param = 0u;
            if (c->needs_param) {
                if (tokens.count != (uint8_t)(first_param_tok + 1u) ||
                    !dali_cli_parse_u8(tokens.tok[first_param_tok], c->max_param, &param)) {
                    set_cmd_usage(spec);
                    return;
                }
            } else if (tokens.count != first_param_tok) {
                set_cmd_usage(spec);
                return;
            }

            bool group_edit = (c->id == DALI_CMD_ADD_TO_GROUP ||
                               c->id == DALI_CMD_REMOVE_FROM_GROUP);
            if (group_edit && tgt.type == DALI_ADDR_BROADCAST) {
                set_cmd_result("no broadcast group config");
                return;
            }

            DaliError err = with_dtr0
                                ? dali_control_config_with_dtr0(tgt, c->id, dtr0, param)
                                : dali_control_config(tgt, c->id, param);
            if (err == DALI_OK) {
                if (group_edit) update_group_members_from_config(tgt, c->id, param);
                if (config_changes_level_profile(c->id)) forget_level_profile_cache(tgt);
                if (group_edit || config_changes_level_profile(c->id)) start_refresh();
            }
            set_cmd_enqueue_result(err);
            return;
        }

        case DALI_CLI_CMD_SPECIAL:
            console_special_(tokens, cmd_ctx);
            return;

        case DALI_CLI_CMD_DT6:
            console_dt6_(tokens, cmd_ctx);
            return;

        case DALI_CLI_CMD_IQUERY:
            console_iquery_(tokens, cmd_ctx);
            return;

        case DALI_CLI_CMD_ICONFIG:
            console_iconfig_(tokens);
            return;

        case DALI_CLI_CMD_VENDOR:
            console_vendor_(tokens, cmd_ctx);
            return;

        case DALI_CLI_CMD_MEMREAD:
            console_memread_(tokens, cmd_ctx);
            return;

        case DALI_CLI_CMD_DEVMEM:
            console_devmem_(tokens, cmd_ctx);
            return;

        /*
         * START/STOP QUIESCENT MODE, send-twice, no reply. `all` is address
         * byte 0xFF — every control device, control gear untouched.
         *
         * A quiesced device reports no events, so anything an automation drives
         * from one stops updating until it is released. The console can only
         * say so in the result string; nothing here tracks the state.
         */
        case DALI_CLI_CMD_QUIESCENT: {
            bool enable;
            if (strcmp(tokens.tok[1], "on") == 0) {
                enable = true;
            } else if (strcmp(tokens.tok[1], "off") == 0) {
                enable = false;
            } else {
                set_cmd_usage(spec);
                return;
            }

            DaliFrame frame;
            DaliError err;
            uint8_t addr = 0u;
            bool all = strcmp(tokens.tok[2], "all") == 0;
            if (all) {
                err = dali_input_build_quiescent_mode_broadcast(enable, &frame);
            } else if (dali_cli_parse_short_addr(tokens.tok[2], &addr)) {
                err = dali_input_build_quiescent_mode(addr, enable, &frame);
            } else {
                set_cmd_usage(spec);
                return;
            }
            if (err != DALI_OK) {
                set_cmd_enqueue_result(err);
                return;
            }

            /* Send-twice expansion is the scheduler's job: one transaction,
             * not two enqueues, so nothing can land between the pair. */
            DaliTransaction txn = {};
            txn.frame      = frame;
            txn.send_twice = true;
            err = dali_sched_enqueue(&txn);
            if (err != DALI_OK) {
                set_cmd_enqueue_result(err);
                return;
            }
            set_cmd_result(enable ? (all ? "quiescent on: all, no events"
                                         : "quiescent on: no events")
                                  : "OK");
            return;
        }

        case DALI_CLI_CMD_DTRCHECK: {
            uint8_t addr, reg, value;
            if (!dali_cli_parse_short_addr(tokens.tok[1], &addr) ||
                !dali_cli_parse_u8(tokens.tok[2], 2u, &reg) ||
                !dali_cli_parse_u8(tokens.tok[3], 255u, &value)) {
                set_cmd_usage(spec);
                return;
            }
            DaliSequence seq;
            DaliError err = dali_input_build_dtr_check_sequence(
                addr, (DaliDtrRegister)reg, value, &seq);
            if (err == DALI_OK) {
                seq.on_complete = on_dtrcheck_done;
                seq.cb_ctx      = cmd_ctx;
                set_cmd_result("pending");
                err = dali_sched_enqueue_sequence(&seq);
            }
            set_cmd_enqueue_error(err);
            return;
        }

        default:
            set_cmd_result("unsupported verb");
            return;
    }
}


void DaliComponent::register_light(uint8_t type, uint8_t address,
                                   uint16_t member_groups, DaliBusLight *light)
{
    if (s_light_count < MAX_LIGHT_ENTITIES) {
        LightEntry &entry = s_light_registry[s_light_count++];
        entry.target_type = type;
        entry.target_address = address;
        entry.member_groups = member_groups;
        entry.light = light;
        entry.profile_generation = 0u;
        begin_light_profile_update(&entry);
    } else {
        ESP_LOGW(TAG, "light registry full — increase MAX_LIGHT_ENTITIES");
    }
}

void DaliComponent::set_scan_level_profile_snapshot(
    const DaliDiscoveryInventory *inventory)
{
    if (inventory == nullptr) return;

    s_scan_level_profile_snapshot_pending_.store(false, std::memory_order_relaxed);
    for (uint8_t address = 0u; address < DALI_SHORT_ADDRESS_COUNT; address++) {
        ScanLevelProfileRecord &record = s_scan_level_profile_snapshot[address];
        record.valid = false;

        const DaliDiscoveryDeviceInfo *device =
            dali_discovery_inventory_get(inventory, address);
        if (device == nullptr || !device->present || !device->has_control_gear ||
            !device->has_level_limits) {
            continue;
        }

        DaliLevelProfile profile = {
            device->has_dimming_curve ? device->dimming_curve
                                       : DALI_DIM_CURVE_STANDARD,
            device->min_level,
            device->max_level,
        };
        if (dali_level_profile_validate(&profile) != DALI_OK) continue;
        record.valid = true;
        record.curve_capability_known =
            dali_discovery_has_device_type(device, 6u);
        record.query_dt6_curve = record.curve_capability_known;
        record.profile = profile;
    }
    s_scan_level_profile_snapshot_pending_.store(true, std::memory_order_release);
}

void DaliComponent::apply_scan_level_profile_snapshot_()
{
    if (!s_scan_level_profile_snapshot_pending_.exchange(false,
                                                          std::memory_order_acq_rel)) {
        return;
    }

    s_profile_refresh_query.active = false;
    s_profile_refresh_query.actual_level_pending = false;
    s_profile_refresh_query.result_ready.store(false, std::memory_order_relaxed);

    for (uint8_t address = 0u; address < DALI_SHORT_ADDRESS_COUNT; address++) {
        const ScanLevelProfileRecord &record = s_scan_level_profile_snapshot[address];
        if (!record.valid) continue;
        s_level_profile_cache[address].valid = true;
        s_level_profile_cache[address].curve_capability_known =
            record.curve_capability_known;
        s_level_profile_cache[address].query_dt6_curve = record.query_dt6_curve;
        s_level_profile_cache[address].profile = record.profile;
    }

    for (uint8_t index = 0u; index < s_light_count; index++) {
        LightEntry *entry = &s_light_registry[index];
        uint8_t query_address = entry->target_type == (uint8_t)DALI_ADDR_GROUP
                                    ? pick_group_member(entry->target_address)
                                    : entry->light->get_query_address();
        /* 0xFF (no representative) still resolves: a group falls back to the
         * union across its known members, and anything else to the default
         * window. An entity without a profile cannot transmit at all. */
        const uint32_t generation = begin_light_profile_update(entry);
        set_light_profile(entry, query_address, generation);
    }
}

bool DaliComponent::set_group_membership_snapshot(const uint64_t masks[16],
                                                  uint16_t verified,
                                                  uint64_t observed_gear)
{
    if (masks == nullptr || verified != 0xFFFFu) return false;
    /* Runs on the scan task (Core 1); flash write is deferred to loop() (Core 0). */
    portENTER_CRITICAL(&s_group_map_mux);
    if (!dali_group_map_scan_covers_known_members(&s_group_map,
                                                   observed_gear)) {
        portEXIT_CRITICAL(&s_group_map_mux);
        return false;
    }
    for (uint8_t g = 0u; g < 16u; g++)
        s_group_map.members[g] = masks[g];
    s_group_map.verified = 0xFFFFu;
    portEXIT_CRITICAL(&s_group_map_mux);
    s_group_members_dirty_.store(true, std::memory_order_release);
    return true;
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
    if (scan_running_.load(std::memory_order_acquire)) return;

    auto reject_profile_stage = [&](LightEntry *entry,
                                    uint8_t query_address,
                                    uint32_t generation,
                                    DaliError err) {
        s_profile_refresh_query.active = false;
        /* Whatever went wrong, the entity must not be left waiting on a profile
         * that is no longer coming: without one it cannot transmit at all. */
        if (entry != nullptr && generation == entry->profile_generation) {
            set_light_profile(entry, query_address, generation);
        }
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
        if (entry != nullptr) {
            ESP_LOGW(TAG, "level profile refresh rejected: target type=%u addr=%u result=%d",
                     (unsigned)entry->target_type, (unsigned)entry->target_address,
                     (int)err);
        }
    };

    if (s_profile_refresh_query.active) {
        if (!s_profile_refresh_query.actual_level_pending) {
            if (s_refresh_query_in_flight_.load(std::memory_order_acquire) ||
                !s_profile_refresh_query.result_ready.exchange(
                    false, std::memory_order_acq_rel)) {
                return;
            }

            LightEntry *entry = s_profile_refresh_query.entry;
            const uint8_t query_address = s_profile_refresh_query.query_address;
            const uint32_t generation = s_profile_refresh_query.generation;
            if (entry == nullptr || generation != entry->profile_generation) {
                s_profile_refresh_query.active = false;
                dali_refresh_cursor_complete(&refresh_cursor_, s_light_count,
                                             DALI_REFRESH_RETRY);
                return;
            }

            DaliError err = DALI_OK;
            switch (s_profile_refresh_query.phase) {
            case PROFILE_REFRESH_DEVICE_TYPE: {
                uint8_t device_type = 0u;
                err = dali_discovery_u8_from_sequence(
                    &s_profile_refresh_query.result,
                    DALI_DISCOVERY_DEVICE_TYPES_STEP_FIRST, &device_type);
                if (err != DALI_OK) {
                    /* Not knowing the device type only rules out the curve, not
                     * the limits — read MIN/MAX anyway. curve_capability_known
                     * stays false so a gear that answers later still gets its
                     * curve, at the cost of one unanswered query per refresh. */
                    ESP_LOGD(TAG, "device-type query failed for short %u: %d; reading limits only",
                             (unsigned)query_address, (int)err);
                    s_profile_refresh_query.query_dt6_curve = false;
                    s_profile_refresh_query.phase = PROFILE_REFRESH_PROFILE;
                    err = enqueue_level_profile_query(query_address, false);
                    if (err == DALI_OK) {
                        refresh_queue_blocked_ = false;
                        return;
                    }
                    reject_profile_stage(entry, query_address, generation, err);
                    return;
                }

                if (device_type == DALI_DISCOVERY_DEVICE_TYPE_MULTIPLE) {
                    s_profile_refresh_query.phase = PROFILE_REFRESH_DEVICE_TYPES;
                    err = enqueue_level_profile_device_types_query(query_address);
                    if (err == DALI_OK) {
                        refresh_queue_blocked_ = false;
                        return;
                    }
                    reject_profile_stage(entry, query_address, generation, err);
                    return;
                }

                s_level_profile_cache[query_address].curve_capability_known = true;
                s_level_profile_cache[query_address].query_dt6_curve =
                    device_type == 6u;
                s_profile_refresh_query.query_dt6_curve =
                    s_level_profile_cache[query_address].query_dt6_curve;
                s_profile_refresh_query.phase = PROFILE_REFRESH_PROFILE;
                err = enqueue_level_profile_query(
                    query_address, s_profile_refresh_query.query_dt6_curve);
                if (err == DALI_OK) {
                    refresh_queue_blocked_ = false;
                    return;
                }
                reject_profile_stage(entry, query_address, generation, err);
                return;
            }

            case PROFILE_REFRESH_DEVICE_TYPES: {
                DaliDiscoveryDeviceInfo device = {};
                err = dali_discovery_device_types_from_sequence(
                    &s_profile_refresh_query.result, &device);
                const bool supports_dt6 = dali_discovery_has_device_type(&device, 6u);
                const bool complete = err == DALI_OK &&
                                      s_profile_refresh_query.result.result == DALI_OK &&
                                      !device.device_types_truncated;
                if (!supports_dt6 && !complete) {
                    /* DT6 may be in the part of the list that never arrived, so
                     * the capability stays unknown — but the limits do not
                     * depend on it, so read them with the two-step sequence. */
                    ESP_LOGD(TAG, "device-type enumeration incomplete for short %u; reading limits only",
                             (unsigned)query_address);
                    s_profile_refresh_query.query_dt6_curve = false;
                    s_profile_refresh_query.phase = PROFILE_REFRESH_PROFILE;
                    err = enqueue_level_profile_query(query_address, false);
                    if (err == DALI_OK) {
                        refresh_queue_blocked_ = false;
                        return;
                    }
                    reject_profile_stage(entry, query_address, generation, err);
                    return;
                }

                s_level_profile_cache[query_address].curve_capability_known = true;
                s_level_profile_cache[query_address].query_dt6_curve = supports_dt6;
                s_profile_refresh_query.query_dt6_curve = supports_dt6;
                s_profile_refresh_query.phase = PROFILE_REFRESH_PROFILE;
                err = enqueue_level_profile_query(query_address, supports_dt6);
                if (err == DALI_OK) {
                    refresh_queue_blocked_ = false;
                    return;
                }
                reject_profile_stage(entry, query_address, generation, err);
                return;
            }

            case PROFILE_REFRESH_PROFILE: {
                DaliDiscoveryGearProfile discovered = {};
                if (s_level_profile_cache[query_address].valid) {
                    const DaliLevelProfile &cached =
                        s_level_profile_cache[query_address].profile;
                    discovered.has_level_limits = true;
                    discovered.min_level = cached.min_level;
                    discovered.max_level = cached.max_level;
                    discovered.has_dimming_curve = true;
                    discovered.dimming_curve = cached.curve;
                }

                DaliError profile_err = dali_discovery_profile_from_sequence(
                    &s_profile_refresh_query.result,
                    s_profile_refresh_query.query_dt6_curve, &discovered);
                if (profile_err == DALI_OK && discovered.has_level_limits) {
                    DaliLevelProfile profile = {
                        discovered.has_dimming_curve ? discovered.dimming_curve
                                                     : DALI_DIM_CURVE_STANDARD,
                        discovered.min_level,
                        discovered.max_level,
                    };
                    if (dali_level_profile_validate(&profile) == DALI_OK) {
                        s_level_profile_cache[query_address].valid = true;
                        s_level_profile_cache[query_address].profile = profile;
                    }
                } else {
                    ESP_LOGD(TAG, "level profile refresh failed for short %u: %d; retaining prior profile",
                             (unsigned)query_address, (int)profile_err);
                }
                set_light_profile(entry, query_address, generation);
                s_profile_refresh_query.actual_level_pending = true;
                break;
            }

            default:
                set_light_profile(entry, query_address, generation);
                s_profile_refresh_query.actual_level_pending = true;
                break;
            }
        }

        if (s_refresh_query_in_flight_.load(std::memory_order_acquire)) return;

        LightEntry *entry = s_profile_refresh_query.entry;
        const uint8_t query_address = s_profile_refresh_query.query_address;
        DaliError err = enqueue_actual_level_query(entry, query_address);
        if (err == DALI_OK) {
            s_profile_refresh_query.active = false;
            refresh_queue_blocked_ = false;
            dali_refresh_cursor_complete(&refresh_cursor_, s_light_count,
                                         DALI_REFRESH_ADVANCE);
            ESP_LOGD(TAG, "query actual level: light target type=%u addr=%u via short %u",
                     (unsigned)entry->target_type, (unsigned)entry->target_address,
                     (unsigned)query_address);
            return;
        }
        if (err == DALI_ERR_QUEUE_FULL) {
            if (!refresh_queue_blocked_) {
                ESP_LOGD(TAG, "light refresh paused: scheduler queue full");
                refresh_queue_blocked_ = true;
            }
            return;
        }

        s_profile_refresh_query.active = false;
        refresh_queue_blocked_ = false;
        dali_refresh_cursor_complete(&refresh_cursor_, s_light_count,
                                     DALI_REFRESH_ADVANCE);
        ESP_LOGW(TAG, "actual-level refresh rejected: target type=%u addr=%u result=%d",
                 (unsigned)entry->target_type, (unsigned)entry->target_address,
                 (int)err);
        return;
    }

    if (s_refresh_query_in_flight_.load(std::memory_order_acquire)) return;

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
            /* No gear to query — a broadcast entity with no query_address, or a
             * group whose membership is not known yet. It still needs a profile
             * before it will transmit anything, including OFF, so give it one
             * from whatever is known and move on. Passing the current
             * generation keeps this idempotent across passes: an entity already
             * holding this profile keeps its deduplication cache. */
            set_light_profile(e, qa, e->profile_generation);
            dali_refresh_cursor_complete(&refresh_cursor_, s_light_count,
                                         DALI_REFRESH_ADVANCE);
            continue;
        }

        const uint32_t generation = begin_light_profile_update(e);
        s_profile_refresh_query.entry = e;
        s_profile_refresh_query.query_address = qa;
        s_profile_refresh_query.generation = generation;
        s_profile_refresh_query.actual_level_pending = false;
        s_profile_refresh_query.active = true;
        DaliError err;
        if (s_level_profile_cache[qa].curve_capability_known) {
            s_profile_refresh_query.query_dt6_curve =
                s_level_profile_cache[qa].query_dt6_curve;
            s_profile_refresh_query.phase = PROFILE_REFRESH_PROFILE;
            err = enqueue_level_profile_query(
                qa, s_profile_refresh_query.query_dt6_curve);
        } else {
            s_profile_refresh_query.query_dt6_curve = false;
            s_profile_refresh_query.phase = PROFILE_REFRESH_DEVICE_TYPE;
            err = enqueue_level_profile_device_type_query(qa);
        }
        if (err == DALI_OK) {
            refresh_queue_blocked_ = false;
            ESP_LOGD(TAG, "query level profile: light target type=%u addr=%u via short %u",
                     (unsigned)e->target_type, (unsigned)e->target_address,
                     (unsigned)qa);
            return;
        }

        reject_profile_stage(e, qa, generation, err);
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

bool DaliComponent::try_claim_bus(const char *what)
{
    if (scan_running_.exchange(true)) {
        ESP_LOGW(TAG, "%s: bus already in use", what != nullptr ? what : "claim");
        return false;
    }
    ESP_LOGI(TAG, "Bus claimed by %s", what != nullptr ? what : "shell");
    return true;
}

void DaliComponent::release_bus()
{
    /*
     * A claimed workflow may have moved levels or changed which addresses
     * exist, so ask for a refresh pass. The request is handed over as an atomic
     * rather than by touching refresh_cursor_ directly: this runs on the shell
     * task, and the cursor belongs to Core 0's loop.
     */
    external_refresh_request_.store(true, std::memory_order_release);
    scan_running_.store(false, std::memory_order_release);
}

void DaliComponent::start_identify()
{
    if (identify_active_) return;
    ESP_LOGI(TAG, "Identify: short address %u (10 s)", (unsigned)diag_address_);
    identify_active_   = true;
    identify_phase_    = true;
    identify_start_ms_ = millis();
    identify_last_ms_  = millis() - 500u;  // fire immediately on first loop tick
    identify_scan_paused_ = false;
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

/*
 * Bus availability now, plus the fault history behind it.
 *
 * bus_idle_failures only ever grows, so on its own it cannot say whether the
 * bus is stuck right now or recovered an hour ago — the previous version of
 * this latched "Bus stuck: N" and never returned. Recovery is therefore judged
 * against tx_frames_ok: a frame clocked out in full after the last fault is
 * direct evidence the PHY can drive the bus again.
 *
 * Both facts are published, because they answer different questions. The state
 * string leads with current availability ("OK" / "Bus stuck") for automations,
 * and carries the cumulative count so a bus that recovers between glances still
 * shows that it has been failing.
 */
void DaliComponent::update_bus_fault_()
{
    if (bus_fault_ == nullptr) return;

    uint32_t faults = g_dali_stats.bus_idle_failures;
    uint32_t tx_ok  = g_dali_stats.tx_frames_ok;

    if (faults != last_bus_fault_count_) {
        /* New fault(s) since the last look: the bus is stuck as far as we know,
         * and stays that way until a frame gets out. */
        last_bus_fault_count_ = faults;
        bus_fault_recovered_  = false;
        tx_ok_at_bus_fault_   = tx_ok;
        char buf[48];
        snprintf(buf, sizeof(buf), "Bus stuck (%u total)", (unsigned)faults);
        ESP_LOGE(TAG, "DALI bus stuck — %u total occurrence(s)", (unsigned)faults);
        bus_fault_->publish_state(buf);
        return;
    }

    if (faults == 0u || bus_fault_recovered_) return;

    /* Sitting in a fault state: a completed transmission clears it. */
    if (tx_ok != tx_ok_at_bus_fault_) {
        bus_fault_recovered_ = true;
        char buf[48];
        snprintf(buf, sizeof(buf), "OK (%u past fault%s)", (unsigned)faults,
                 faults == 1u ? "" : "s");
        ESP_LOGI(TAG, "DALI bus recovered after %u fault(s)", (unsigned)faults);
        bus_fault_->publish_state(buf);
    }
}

/*
 * A rejected submission is dropped work: no scheduler client retries on the
 * caller's behalf. Report only the increments, so a bus under sustained
 * pressure produces one line per new loss rather than one per loop.
 */
void DaliComponent::report_queue_drops_()
{
    DaliSchedQueueStats stats;
    if (dali_sched_queue_stats(&stats) != DALI_OK) return;

    if (stats.rejected_full != last_queue_rejected_full_) {
        ESP_LOGW(TAG, "scheduler queue full: %u command(s) dropped "
                      "(total %u, depth %u/%u, high-water %u)",
                 (unsigned)(stats.rejected_full - last_queue_rejected_full_),
                 (unsigned)stats.rejected_full,
                 (unsigned)stats.depth, (unsigned)stats.capacity,
                 (unsigned)stats.high_water);
        last_queue_rejected_full_ = stats.rejected_full;
    }
    if (stats.rejected_busy != last_queue_rejected_busy_) {
        ESP_LOGW(TAG, "scheduler reset barrier: %u command(s) dropped (total %u)",
                 (unsigned)(stats.rejected_busy - last_queue_rejected_busy_),
                 (unsigned)stats.rejected_busy);
        last_queue_rejected_busy_ = stats.rejected_busy;
    }
}

/*
 * Diagnostic buttons are fire-and-forget, so a rejected enqueue would otherwise
 * be invisible: the operator sees a button press and no bus activity. Report it
 * on the diagnostic text sensor and in the log. `OK` is not published on
 * success — as elsewhere, admission is not device acknowledgement, and the
 * refresh button is the way to read back an actual level.
 */
void DaliComponent::report_diag_enqueue_(const char *what, DaliError err)
{
    if (err == DALI_OK) return;
    ESP_LOGW(TAG, "diag %s enqueue failed: %d", what, (int)err);
    if (scan_status_ == nullptr) return;
    char buf[40];
    snprintf(buf, sizeof(buf), "%s: %s", what,
             err == DALI_ERR_QUEUE_FULL ? "queue full"
                                        : (err == DALI_ERR_BUSY ? "busy" : "err"));
    scan_status_->publish_state(buf);
}

void DaliComponent::send_diag_on()
{
    if (scan_running_.load(std::memory_order_acquire)) return;
    DaliTarget t{ DALI_ADDR_SHORT, diag_address_ };
    report_diag_enqueue_("on", dali_control_go_to_last_active_level(t));
}

void DaliComponent::send_diag_off()
{
    if (scan_running_.load(std::memory_order_acquire)) return;
    DaliTarget t{ DALI_ADDR_SHORT, diag_address_ };
    report_diag_enqueue_("off", dali_control_off(t));
}

void DaliComponent::send_diag_max()
{
    if (scan_running_.load(std::memory_order_acquire)) return;
    DaliTarget t{ DALI_ADDR_SHORT, diag_address_ };
    report_diag_enqueue_("max", dali_control_recall_max(t));
}

void DaliComponent::send_diag_min()
{
    if (scan_running_.load(std::memory_order_acquire)) return;
    DaliTarget t{ DALI_ADDR_SHORT, diag_address_ };
    report_diag_enqueue_("min", dali_control_recall_min(t));
}

void DaliComponent::send_diag_refresh()
{
    if (scan_running_.load(std::memory_order_acquire)) return;
    DaliTarget qt{ DALI_ADDR_SHORT, diag_address_ };
    report_diag_enqueue_("refresh",
                         dali_control_query(qt, DALI_CMD_QUERY_ACTUAL_LEVEL, 0u,
                                            on_diag_refresh_reply, nullptr));
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

void DaliComponent::on_scan_complete(uint8_t count,
                                     bool success,
                                     bool data_complete)
{
    scan_count_         = count;
    scan_success_       = success;
    scan_data_complete_ = data_complete;
    scan_done_.store(true, std::memory_order_release);
}

}  // namespace dali
}  // namespace esphome
