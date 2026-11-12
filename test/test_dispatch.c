/*
 * test_dispatch.c — unit tests for dali_dispatch.c
 */

#include "unity.h"
#include "dali_dispatch.h"
#include "dali_scheduler.h"
#include <string.h>

/* ── Mock scheduler ──────────────────────────────────────────────────────── */

static DaliFrame         s_last_tx;
static uint8_t           s_tx_count;
static DaliError         s_tx_result;
static DaliPhyRxCallback s_rx_cb;
static void             *s_rx_ctx;
static uint32_t          s_tick_ms;

static DaliError mock_tx(const DaliFrame *frame)
{
    s_last_tx = *frame;
    s_tx_count++;
    return s_tx_result;
}

static void mock_set_rx_callback(DaliPhyRxCallback cb, void *ctx)
{
    s_rx_cb  = cb;
    s_rx_ctx = ctx;
}

static uint32_t mock_get_tick_ms(void) { return s_tick_ms; }

static void advance_to_next_forward(void)
{
    s_tick_ms += (DALI_FORWARD_INTERFRAME_US + 999u) / 1000u;
}

/* The frame just sent completes once the settle period has passed. */
static void finish_tx(void)
{
    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
}

/* Completion capture for dali_dispatch_cb(). */
static int                s_done_count;
static DaliError          s_done_result;
static DaliDispatchResult s_done_state;
static void              *s_done_ctx;
static uint8_t            s_done_marker;

static void on_done(DaliError result, const DaliDispatchResult *state, void *ctx)
{
    s_done_count++;
    s_done_result = result;
    s_done_state  = *state;
    s_done_ctx    = ctx;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static DaliInputEvent make_legacy(DaliEventAddressKind kind,
                                  uint8_t              address,
                                  bool                 selector,
                                  uint8_t              opcode)
{
    uint8_t addr_byte;
    if (kind == DALI_EVENT_ADDRESS_GROUP) {
        addr_byte = (uint8_t)(0x80u | ((address & 0x0Fu) << 1u) | (selector ? 1u : 0u));
    } else {
        addr_byte = (uint8_t)(((address & 0x3Fu) << 1u) | (selector ? 1u : 0u));
    }
    DaliFrame frame = {
        .data = ((uint32_t)addr_byte << 8u) | opcode,
        .bit_length = DALI_FORWARD_FRAME_BITS,
    };
    DaliInputEvent ev;
    TEST_ASSERT_EQUAL(DALI_OK, dali_event_parse_frame(&frame, &ev));
    return ev;
}

static DaliInputEvent make_dali2(DaliEventAddressKind kind,
                                 uint8_t              address,
                                 uint8_t              instance,
                                 uint16_t             event_information)
{
    TEST_ASSERT_EQUAL(DALI_EVENT_ADDRESS_SHORT, kind);
    DaliFrame frame = {
        .data = ((uint32_t)(address & 0x3Fu) << 17u) |
                0x008000u |
                ((uint32_t)(instance & 0x1Fu) << 10u) |
                (event_information & 0x03FFu),
        .bit_length = DALI_EXTENDED_FRAME_BITS,
    };
    DaliInputEvent ev;
    TEST_ASSERT_EQUAL(DALI_OK, dali_event_parse_frame(&frame, &ev));
    return ev;
}

/* ── setUp / tearDown ────────────────────────────────────────────────────── */

void setUp(void)
{
    memset(&s_last_tx, 0, sizeof(s_last_tx));
    s_tx_count  = 0u;
    s_tx_result = DALI_OK;
    s_rx_cb     = NULL;
    s_rx_ctx    = NULL;
    s_tick_ms   = 0u;
    memset(&g_dali_stats, 0, sizeof(g_dali_stats));
    s_done_count  = 0;
    s_done_result = DALI_ERR_INVALID;
    memset(&s_done_state, 0, sizeof(s_done_state));
    s_done_ctx    = NULL;

    DaliSchedOps ops = {
        .tx              = mock_tx,
        .set_rx_callback = mock_set_rx_callback,
        .get_tick_ms     = mock_get_tick_ms,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_init(&ops));
    dali_dispatch_init();
}

void tearDown(void) {}

/* ── Tests ───────────────────────────────────────────────────────────────── */

void test_null_args_return_invalid(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_dispatch(NULL,  1u, &ev,   NULL, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_dispatch(table, 0u, &ev,   NULL, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_dispatch(table, 1u, NULL,  NULL, NULL));
}

void test_no_matching_entry_returns_ok_silently(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 3, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count); /* nothing sent */
}

void test_observe_recall_max_infers_on_without_tx(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_OBSERVE, 0 },
    };
    DaliDispatchToggleState state = {};
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
    /* Known on, for toggle bookkeeping... */
    TEST_ASSERT_TRUE((state.group_on >> 6u) & 1u);
    /* ...but the resulting level is the gear's MAX LEVEL, which is not
     * knowable here and is only 254 when MAX LEVEL was never reduced. */
    TEST_ASSERT_TRUE(result.matched);
    TEST_ASSERT_FALSE(result.has_state);
    TEST_ASSERT_EQUAL_INT(DALI_ADDR_GROUP, result.target.type);
    TEST_ASSERT_EQUAL_UINT8(6u, result.target.address);
}

void test_observe_off_infers_off_without_tx(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x00u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_OBSERVE, 0 },
    };
    DaliDispatchToggleState state = { .group_on = (uint16_t)(1u << 6u) };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
    TEST_ASSERT_FALSE((state.group_on >> 6u) & 1u);
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_FALSE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(0u, result.level);
}

void test_observe_dim_has_unknown_state_without_tx(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x01u); /* UP */
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_OBSERVE, 0 },
    };
    DaliDispatchResult result = { .has_state = true };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, &result));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
    TEST_ASSERT_FALSE(result.has_state);
    TEST_ASSERT_TRUE(result.matched);
    TEST_ASSERT_EQUAL_INT(DALI_ADDR_GROUP, result.target.type);
    TEST_ASSERT_EQUAL_UINT8(0u, result.target.address);
}

void test_observe_dapc_level_infers_level_without_tx(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 2, false, 123u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 2,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 2 }, DALI_DISPATCH_ACTION_OBSERVE, 0 },
    };
    DaliDispatchToggleState state = {};
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
    TEST_ASSERT_TRUE((state.group_on >> 2u) & 1u);
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_TRUE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(123u, result.level);
    TEST_ASSERT_EQUAL_INT(DALI_ADDR_GROUP, result.target.type);
    TEST_ASSERT_EQUAL_UINT8(2u, result.target.address);
}

void test_observe_dapc_zero_infers_off_without_tx(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 2, false, 0u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 2,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 2 }, DALI_DISPATCH_ACTION_OBSERVE, 0 },
    };
    DaliDispatchToggleState state = { .group_on = (uint16_t)(1u << 2u) };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
    TEST_ASSERT_FALSE((state.group_on >> 2u) & 1u);
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_FALSE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(0u, result.level);
}

void test_mirror_dapc_level_translates_to_output_target(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_SHORT, 16, false, 123u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_SHORT, 16,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, &result));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8C7Bu, s_last_tx.data); /* group 6 DAPC 123 */
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_TRUE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(123u, result.level);
}

void test_mirror_recall_max_issues_correct_frame(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, NULL));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    /* group 6 RECALL MAX: addr_byte = 0x80|(6<<1)|1 = 0x8D; opcode = 0x05 → 0x8D05 */
    TEST_ASSERT_EQUAL_HEX32(0x8D05u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
}

void test_mirror_off_issues_correct_frame(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x00u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, NULL));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8D00u, s_last_tx.data);
}

void test_mirror_dapc_frame_issues_dapc(void)
{
    /* address_selector = false → DAPC, not a command */
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, false, 0x80u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };

    DaliError err = dali_dispatch(table, 1u, &ev, NULL, NULL);
    TEST_ASSERT_EQUAL(DALI_OK, err);
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8C80u, s_last_tx.data);
}

void test_mirror_phantom_remap_to_different_group(void)
{
    /* Phantom address 16 → real group 6 (Approach B) */
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_SHORT, 16, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_SHORT, 16,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, NULL));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    /* output is group 6 RECALL MAX, not short 16 */
    TEST_ASSERT_EQUAL_HEX32(0x8D05u, s_last_tx.data);
}

void test_event_information_specific_only_fires_on_match(void)
{
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            0x05u, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_RECALL_MAX, 0 },
    };

    /* wrong opcode — no match */
    DaliInputEvent ev_off = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x00u);
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev_off, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);

    /* correct opcode — fires */
    DaliInputEvent ev_on = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x05u);
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev_on, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count); /* not yet run */
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
}

void test_action_off_sends_off_frame(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 2 }, DALI_DISPATCH_ACTION_OFF, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, NULL));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    /* group 2 OFF: addr_byte = 0x80|(2<<1)|1 = 0x85; opcode = 0x00 → 0x8500 */
    TEST_ASSERT_EQUAL_HEX32(0x8500u, s_last_tx.data);
}

void test_action_scene_sends_correct_frame(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_SCENE, 5u },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, NULL));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    /* group 0 GO TO SCENE 5: addr_byte = 0x81; opcode = 0x10+5 = 0x15 → 0x8115 */
    TEST_ASSERT_EQUAL_HEX32(0x8115u, s_last_tx.data);
}

void test_toggle_starts_off_and_flips(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 3, 0, 0x02u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 3, 0x02u, 0u,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_TOGGLE, 0 },
    };
    DaliDispatchToggleState state = {};

    /* First press: off → on → RECALL MAX */
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, NULL));
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8105u, s_last_tx.data); /* group 0 RECALL MAX */

    /* Second press: on → off → OFF */
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, NULL));
    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(2u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8100u, s_last_tx.data); /* group 0 OFF */

    /* Third press: back to on */
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, NULL));
    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(3u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8105u, s_last_tx.data); /* group 0 RECALL MAX */
}

void test_broadcast_toggle_flips_between_recall_max_and_off(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 3, 0, 0x02u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 3, 0x02u, 0u,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_BROADCAST, 0 }, DALI_DISPATCH_ACTION_TOGGLE, 0 },
    };
    DaliDispatchToggleState state = {};
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0xFF05u, s_last_tx.data);
    finish_tx();
    /* The toggle knows it turned things on; the published state does not claim
     * a level, because the on branch is RECALL MAX. */
    TEST_ASSERT_TRUE(state.broadcast_on);
    TEST_ASSERT_TRUE(result.matched);
    TEST_ASSERT_FALSE(result.has_state);

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));
    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(2u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0xFF00u, s_last_tx.data);
    finish_tx();
    TEST_ASSERT_FALSE(state.broadcast_on);
    /* OFF is exactly level 0, so this half still publishes a state. */
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_FALSE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(0u, result.level);
}

void test_dali2_device_instance_matches_canonical_address_and_instance(void)
{
    /* Device/Instance: short address 5, instance 3, short-press information. */
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 5u, 3u,
                                   DALI_DT301_EVENT_SHORT_PRESS);
    DaliDispatchEntry wrong_address[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 4u,
            DALI_DT301_EVENT_SHORT_PRESS, 3u,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0u }, DALI_DISPATCH_ACTION_RECALL_MAX, 0u },
    };
    DaliDispatchEntry wrong_instance[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 5u,
            DALI_DT301_EVENT_SHORT_PRESS, 2u,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0u }, DALI_DISPATCH_ACTION_RECALL_MAX, 0u },
    };
    DaliDispatchEntry matching[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 5u,
            DALI_DT301_EVENT_SHORT_PRESS, 3u,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0u }, DALI_DISPATCH_ACTION_RECALL_MAX, 0u },
    };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_DEVICE_INSTANCE, ev.source.scheme);
    TEST_ASSERT_TRUE(ev.source.has_device_address);
    TEST_ASSERT_EQUAL_UINT8(5u, ev.source.device_address);
    TEST_ASSERT_TRUE(ev.source.has_instance);
    TEST_ASSERT_EQUAL_UINT8(3u, ev.source.instance);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dispatch(wrong_address, 1u, &ev, NULL, &result));
    TEST_ASSERT_FALSE(result.matched);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dispatch(wrong_instance, 1u, &ev, NULL, &result));
    TEST_ASSERT_FALSE(result.matched);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dispatch(matching, 1u, &ev, NULL, &result));
    TEST_ASSERT_TRUE(result.matched);

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8105u, s_last_tx.data);
}

void test_dali2_dispatch_matches_all_ten_event_information_bits(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 5u, 3u, 0x2ABu);
    DaliDispatchEntry low_eight_bits_only[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 5u,
            0x00ABu, 3u,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0u }, DALI_DISPATCH_ACTION_RECALL_MAX, 0u },
    };
    DaliDispatchEntry full_information[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 5u,
            0x02ABu, 3u,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0u }, DALI_DISPATCH_ACTION_RECALL_MAX, 0u },
    };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL_HEX16(0x02ABu, ev.event_information);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dispatch(low_eight_bits_only, 1u, &ev, NULL, &result));
    TEST_ASSERT_FALSE(result.matched);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dispatch(full_information, 1u, &ev, NULL, &result));
    TEST_ASSERT_TRUE(result.matched);
}

static DaliInputEvent parse_event_24(uint32_t raw)
{
    DaliFrame frame = { .data = raw, .bit_length = DALI_EXTENDED_FRAME_BITS };
    DaliInputEvent ev;
    TEST_ASSERT_EQUAL(DALI_OK, dali_event_parse_frame(&frame, &ev));
    return ev;
}

static bool dispatch_matches(const DaliDispatchKey *key, const DaliInputEvent *ev)
{
    DaliDispatchEntry entry = {
        .key    = *key,
        .output = { DALI_ADDR_GROUP, 0u },
        .action = DALI_DISPATCH_ACTION_RECALL_MAX,
    };
    DaliDispatchResult result = {};
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(&entry, 1u, ev, NULL, &result));
    return result.matched;
}

/*
 * Instance group 3 and device group 3 are unrelated sources that the original
 * five-field key could not tell apart. group_kind can; EITHER keeps the old
 * behaviour for keys that never set it.
 */
void test_dali2_group_kind_separates_device_and_instance_groups(void)
{
    DaliInputEvent instance_group = parse_event_24(0xC60C0Bu); /* ig3, type 3 */
    DaliInputEvent device_group   = parse_event_24(0x860C0Bu); /* dg3, type 3 */
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_INSTANCE_GROUP, instance_group.source.scheme);
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_DEVICE_GROUP, device_group.source.scheme);

    DaliDispatchKey key = {
        .frame_kind        = DALI_EVENT_FRAME_INPUT_24BIT,
        .address_kind      = DALI_EVENT_ADDRESS_GROUP,
        .address           = 3u,
        .event_information = DALI_DISPATCH_EVENT_ANY,
        .instance          = DALI_DISPATCH_INSTANCE_ANY,
    };
    TEST_ASSERT_TRUE(dispatch_matches(&key, &instance_group));
    TEST_ASSERT_TRUE(dispatch_matches(&key, &device_group));

    key.group_kind = DALI_DISPATCH_GROUP_INSTANCE;
    TEST_ASSERT_TRUE(dispatch_matches(&key, &instance_group));
    TEST_ASSERT_FALSE(dispatch_matches(&key, &device_group));

    key.group_kind = DALI_DISPATCH_GROUP_DEVICE;
    TEST_ASSERT_FALSE(dispatch_matches(&key, &instance_group));
    TEST_ASSERT_TRUE(dispatch_matches(&key, &device_group));

    key.address = 4u;
    TEST_ASSERT_FALSE(dispatch_matches(&key, &device_group));
}

void test_dali2_instance_type_key_needs_a_typed_event(void)
{
    DaliInputEvent device_scheme = parse_event_24(0x0A0C0Bu); /* d5, type 3 */
    DaliInputEvent device_instance = make_dali2(DALI_EVENT_ADDRESS_SHORT, 5u, 1u,
                                                0x00Bu);
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_DEVICE, device_scheme.source.scheme);

    DaliDispatchKey key = {
        .frame_kind          = DALI_EVENT_FRAME_INPUT_24BIT,
        .address_kind        = DALI_EVENT_ADDRESS_SHORT,
        .address             = 5u,
        .event_information   = DALI_DISPATCH_EVENT_ANY,
        .instance            = DALI_DISPATCH_INSTANCE_ANY,
        .match_instance_type = true,
        .instance_type       = 3u,
    };
    TEST_ASSERT_TRUE(dispatch_matches(&key, &device_scheme));
    /* Device/Instance frames carry no type, so a typed key cannot match. */
    TEST_ASSERT_FALSE(dispatch_matches(&key, &device_instance));

    key.instance_type = 4u;
    TEST_ASSERT_FALSE(dispatch_matches(&key, &device_scheme));

    key.match_instance_type = false;
    TEST_ASSERT_TRUE(dispatch_matches(&key, &device_scheme));
    TEST_ASSERT_TRUE(dispatch_matches(&key, &device_instance));
}

void test_dali2_exact_00ff_does_not_collide_with_event_any(void)
{
    DaliInputEvent different = make_dali2(DALI_EVENT_ADDRESS_SHORT, 5u, 3u,
                                          0x0100u);
    DaliInputEvent exact = make_dali2(DALI_EVENT_ADDRESS_SHORT, 5u, 3u,
                                      0x00FFu);
    DaliDispatchEntry exact_ff[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 5u,
            0x00FFu, 3u,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0u }, DALI_DISPATCH_ACTION_RECALL_MAX, 0u },
    };
    DaliDispatchEntry any_information[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 5u,
            DALI_DISPATCH_EVENT_ANY, 3u,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0u }, DALI_DISPATCH_ACTION_RECALL_MAX, 0u },
    };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL_HEX16(0xFFFFu, DALI_DISPATCH_EVENT_ANY);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dispatch(exact_ff, 1u, &different, NULL, &result));
    TEST_ASSERT_FALSE(result.matched);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dispatch(exact_ff, 1u, &exact, NULL, &result));
    TEST_ASSERT_TRUE(result.matched);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_dispatch(any_information, 1u, &different, NULL, &result));
    TEST_ASSERT_TRUE(result.matched);
}

void test_legacy_only_actions_reject_part103_events(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 1, 0, 0x02u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 1,
            DALI_DISPATCH_EVENT_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_OBSERVE, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dispatch(table, 1u, &ev, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);

    table[0].action = DALI_DISPATCH_ACTION_MIRROR;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_dispatch(table, 1u, &ev, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
}

void test_mirror_updates_toggle_state(void)
{
    DaliInputEvent ev_on  = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x05u);
    DaliInputEvent ev_off = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x00u);
    DaliDispatchEntry mirror_table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };
    DaliDispatchToggleState state = {};

    /* A mirrored frame moves the toggle once it has gone out, not before. */
    dali_dispatch(mirror_table, 1u, &ev_on,  &state, NULL);
    TEST_ASSERT_FALSE((state.group_on >> 6u) & 1u);
    dali_sched_run();
    finish_tx();
    TEST_ASSERT_TRUE((state.group_on >> 6u) & 1u);

    dali_dispatch(mirror_table, 1u, &ev_off, &state, NULL);
    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_TRUE((state.group_on >> 6u) & 1u);
    finish_tx();
    TEST_ASSERT_FALSE((state.group_on >> 6u) & 1u);
}

void test_first_matching_entry_wins(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_RECALL_MAX, 0 },
        /* Second matching entry — must be skipped */
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 7 }, DALI_DISPATCH_ACTION_OFF, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 2u, &ev, NULL, NULL));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8105u, s_last_tx.data); /* group 0 RECALL MAX, not group 7 OFF */
}

/* ── Result inference tests ──────────────────────────────────────────────── */

void test_result_mirror_recall_max_does_not_claim_a_level(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };
    DaliDispatchResult result = { .has_state = false };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, &result));
    TEST_ASSERT_TRUE(result.matched);
    TEST_ASSERT_FALSE(result.has_state);
    TEST_ASSERT_EQUAL_INT(DALI_ADDR_GROUP, result.target.type);
    TEST_ASSERT_EQUAL_UINT8(6u, result.target.address);
}

/*
 * STEP DOWN AND OFF switches off only if the gear was already at its minimum;
 * otherwise it steps down one point and stays on. Claiming "off, level 0" was
 * wrong for every gear above minimum.
 */
void test_result_step_down_and_off_claims_neither_state_nor_level(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x07u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_OBSERVE, 0 },
    };
    DaliDispatchToggleState state = {};
    DaliDispatchResult result = {};

    /* Seed the toggle on, as a prior RECALL MAX would have. */
    dali_dispatch_seed_toggle(&state, (DaliTarget){ DALI_ADDR_GROUP, 6 }, true);

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));

    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
    TEST_ASSERT_TRUE(result.matched);
    TEST_ASSERT_FALSE(result.has_state);
    /* The toggle is left as it was rather than being flipped to off on a guess;
     * the deferred actual-level query is what settles it. */
    TEST_ASSERT_TRUE((state.group_on >> 6u) & 1u);
}

void test_result_mirror_off_infers_off(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x00u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };
    DaliDispatchResult result = { .has_state = false };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, &result));
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_FALSE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(0u, result.level);
}

void test_result_mirror_dim_has_no_state(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x01u); /* UP */
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };
    DaliDispatchResult result = { .has_state = true }; /* pre-set true to check it's cleared */

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, &result));
    TEST_ASSERT_FALSE(result.has_state);
}

void test_result_action_off_infers_off(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 3 }, DALI_DISPATCH_ACTION_OFF, 0 },
    };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, &result));
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_FALSE(result.is_on);
    TEST_ASSERT_EQUAL_INT(DALI_ADDR_GROUP, result.target.type);
    TEST_ASSERT_EQUAL_UINT8(3u, result.target.address);
}

void test_result_action_recall_max_does_not_claim_a_level(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x00u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 5 }, DALI_DISPATCH_ACTION_RECALL_MAX, 0 },
    };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, &result));
    TEST_ASSERT_TRUE(result.matched);
    TEST_ASSERT_FALSE(result.has_state);
    TEST_ASSERT_EQUAL_UINT8(5u, result.target.address);
}

void test_result_toggle_infers_state(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 1, 0, 0x02u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 1, 0x02u, 0u,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_TOGGLE, 0 },
    };
    DaliDispatchToggleState state = {};
    DaliDispatchResult result = {};

    /* First press: off → on. The on branch is RECALL MAX, so the level is the
     * gear's own MAX LEVEL and is left for the deferred query to report. */
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));
    TEST_ASSERT_TRUE(result.matched);
    TEST_ASSERT_FALSE(result.has_state);

    /* Second press: on → off. OFF is exactly level 0 on any gear. */
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_FALSE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(0u, result.level);
}

void test_result_null_result_out_does_not_crash(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_RECALL_MAX, 0 },
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, NULL));
}

/* ── Acting on transmission, not admission ───────────────────────────────── */

/*
 * A transmitting action used to flip the toggle and hand its inferred state to
 * Home Assistant as soon as its frame was queued. These pin the replacement:
 * nothing moves until the scheduler reports the frame sent.
 */

static const DaliDispatchEntry s_off_group3[] = {
    { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 4, 0x02u, 0u,
        DALI_DISPATCH_GROUP_EITHER, false, 0u },
      { DALI_ADDR_GROUP, 3 }, DALI_DISPATCH_ACTION_OFF, 0 },
};

static const DaliDispatchEntry s_toggle_group0[] = {
    { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 4, 0x02u, 0u,
        DALI_DISPATCH_GROUP_EITHER, false, 0u },
      { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_TOGGLE, 0 },
};

void test_transmitted_action_reports_through_done_once_sent(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 4, 0, 0x02u);
    DaliDispatchToggleState state = { .group_on = (uint16_t)(1u << 3u) };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch_cb(s_off_group3, 1u, &ev, &state,
                                                &result, on_done,
                                                &s_done_marker));
    /* Queued: the inference is there and marked pending; nothing moved. */
    TEST_ASSERT_TRUE(result.matched);
    TEST_ASSERT_TRUE(result.pending);
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_FALSE(result.is_on);
    TEST_ASSERT_TRUE((state.group_on >> 3u) & 1u);
    TEST_ASSERT_EQUAL(0, s_done_count);

    dali_sched_run();
    TEST_ASSERT_EQUAL_HEX32(0x8700u, s_last_tx.data);   /* group 3 OFF */
    TEST_ASSERT_EQUAL(0, s_done_count);

    finish_tx();
    TEST_ASSERT_EQUAL(1, s_done_count);
    TEST_ASSERT_EQUAL(DALI_OK, s_done_result);
    TEST_ASSERT_EQUAL_PTR(&s_done_marker, s_done_ctx);
    TEST_ASSERT_FALSE(s_done_state.pending);
    TEST_ASSERT_TRUE(s_done_state.matched);
    TEST_ASSERT_TRUE(s_done_state.has_state);
    TEST_ASSERT_FALSE(s_done_state.is_on);
    TEST_ASSERT_EQUAL_UINT8(0u, s_done_state.level);
    TEST_ASSERT_EQUAL_INT(DALI_ADDR_GROUP, s_done_state.target.type);
    TEST_ASSERT_EQUAL_UINT8(3u, s_done_state.target.address);
    TEST_ASSERT_FALSE((state.group_on >> 3u) & 1u);
}

void test_failed_transmission_changes_nothing_and_says_so(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 4, 0, 0x02u);
    DaliDispatchToggleState state = { .group_on = (uint16_t)(1u << 3u) };
    s_tx_result = DALI_ERR_BUS_STUCK;

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch_cb(s_off_group3, 1u, &ev, &state,
                                                NULL, on_done, NULL));
    dali_sched_run();

    TEST_ASSERT_EQUAL(1, s_done_count);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, s_done_result);
    /* The OFF never went out, so the group is still on as far as anyone knows. */
    TEST_ASSERT_TRUE((state.group_on >> 3u) & 1u);
}

void test_cancelled_frame_changes_nothing_and_says_so(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 4, 0, 0x02u);
    DaliDispatchToggleState state = { .group_on = (uint16_t)(1u << 3u) };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch_cb(s_off_group3, 1u, &ev, &state,
                                                NULL, on_done, NULL));
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_reset());
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
    TEST_ASSERT_EQUAL(1, s_done_count);
    TEST_ASSERT_EQUAL(DALI_SCHED_RESET_ERROR, s_done_result);
    TEST_ASSERT_TRUE((state.group_on >> 3u) & 1u);
}

void test_refused_frame_queues_nothing_and_never_calls_done(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 4, 0, 0x02u);
    DaliDispatchToggleState state = {};
    DaliDispatchResult result = {};
    DaliTransaction filler = {
        .frame = { .data = 0x0100u, .bit_length = 16u },
    };
    for (uint8_t i = 0u; i < DALI_CMD_QUEUE_SIZE; i++) {
        TEST_ASSERT_EQUAL(DALI_OK, dali_sched_enqueue(&filler));
    }

    TEST_ASSERT_EQUAL(DALI_ERR_QUEUE_FULL,
                      dali_dispatch_cb(s_toggle_group0, 1u, &ev, &state,
                                       &result, on_done, NULL));
    TEST_ASSERT_FALSE(result.pending);

    /* Refused, so not pending either: the next press decides from "off". */
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_reset());
    dali_sched_run();
    TEST_ASSERT_EQUAL(0, s_done_count);
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch_cb(s_toggle_group0, 1u, &ev,
                                                &state, NULL, on_done, NULL));
    dali_sched_run();
    TEST_ASSERT_EQUAL_HEX32(0x8105u, s_last_tx.data);   /* RECALL MAX */
}

/* Every completion returns its slot: far more actions than the pool holds. */
void test_pending_slots_are_returned(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 4, 0, 0x02u);
    DaliDispatchToggleState state = {};

    for (int i = 0; i < 3 * (int)(DALI_CMD_QUEUE_SIZE + 1u); i++) {
        TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch_cb(s_toggle_group0, 1u, &ev,
                                                    &state, NULL, on_done,
                                                    NULL));
        advance_to_next_forward();
        dali_sched_run();
        finish_tx();
        TEST_ASSERT_EQUAL(i + 1, s_done_count);
        TEST_ASSERT_EQUAL(DALI_OK, s_done_result);
    }
}

/*
 * The toggle decides from the bus state with queued actions on top, so a
 * second press that lands before the first press's frame goes out still
 * toggles back.
 */
void test_toggle_second_press_before_the_first_is_sent_toggles_back(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 4, 0, 0x02u);
    DaliDispatchToggleState state = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(s_toggle_group0, 1u, &ev, &state, NULL));
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(s_toggle_group0, 1u, &ev, &state, NULL));
    TEST_ASSERT_FALSE(state.group_on & 1u);

    dali_sched_run();
    TEST_ASSERT_EQUAL_HEX32(0x8105u, s_last_tx.data);   /* RECALL MAX */
    finish_tx();
    TEST_ASSERT_TRUE(state.group_on & 1u);

    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL_HEX32(0x8100u, s_last_tx.data);   /* OFF */
    finish_tx();
    TEST_ASSERT_FALSE(state.group_on & 1u);
}

/* A press whose frame failed leaves nothing behind for the next to read. */
void test_toggle_falls_back_when_the_queued_frame_fails(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 4, 0, 0x02u);
    DaliDispatchToggleState state = {};

    s_tx_result = DALI_ERR_BUS_STUCK;
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(s_toggle_group0, 1u, &ev, &state, NULL));
    dali_sched_run();
    TEST_ASSERT_EQUAL_HEX32(0x8105u, s_last_tx.data);
    TEST_ASSERT_FALSE(state.group_on & 1u);

    s_tx_result = DALI_OK;
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(s_toggle_group0, 1u, &ev, &state, NULL));
    advance_to_next_forward();
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(2u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8105u, s_last_tx.data);   /* RECALL MAX again */
}

void test_observe_is_final_not_pending(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x00u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY, DALI_DISPATCH_INSTANCE_ANY,
            DALI_DISPATCH_GROUP_EITHER, false, 0u },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_OBSERVE, 0 },
    };
    DaliDispatchToggleState state = { .group_on = (uint16_t)(1u << 6u) };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch_cb(table, 1u, &ev, &state, &result,
                                                on_done, NULL));
    TEST_ASSERT_TRUE(result.matched);
    TEST_ASSERT_FALSE(result.pending);
    TEST_ASSERT_FALSE((state.group_on >> 6u) & 1u);
    /* Nothing was sent, so there is nothing to call back about. */
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
    TEST_ASSERT_EQUAL(0, s_done_count);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_null_args_return_invalid);
    RUN_TEST(test_no_matching_entry_returns_ok_silently);
    RUN_TEST(test_observe_recall_max_infers_on_without_tx);
    RUN_TEST(test_observe_off_infers_off_without_tx);
    RUN_TEST(test_observe_dim_has_unknown_state_without_tx);
    RUN_TEST(test_observe_dapc_level_infers_level_without_tx);
    RUN_TEST(test_observe_dapc_zero_infers_off_without_tx);
    RUN_TEST(test_mirror_dapc_level_translates_to_output_target);
    RUN_TEST(test_mirror_recall_max_issues_correct_frame);
    RUN_TEST(test_mirror_off_issues_correct_frame);
    RUN_TEST(test_mirror_dapc_frame_issues_dapc);
    RUN_TEST(test_mirror_phantom_remap_to_different_group);
    RUN_TEST(test_event_information_specific_only_fires_on_match);
    RUN_TEST(test_action_off_sends_off_frame);
    RUN_TEST(test_action_scene_sends_correct_frame);
    RUN_TEST(test_toggle_starts_off_and_flips);
    RUN_TEST(test_broadcast_toggle_flips_between_recall_max_and_off);
    RUN_TEST(test_dali2_device_instance_matches_canonical_address_and_instance);
    RUN_TEST(test_dali2_dispatch_matches_all_ten_event_information_bits);
    RUN_TEST(test_dali2_group_kind_separates_device_and_instance_groups);
    RUN_TEST(test_dali2_instance_type_key_needs_a_typed_event);
    RUN_TEST(test_dali2_exact_00ff_does_not_collide_with_event_any);
    RUN_TEST(test_legacy_only_actions_reject_part103_events);
    RUN_TEST(test_mirror_updates_toggle_state);
    RUN_TEST(test_first_matching_entry_wins);
    /* DaliDispatchResult inference */
    RUN_TEST(test_result_mirror_recall_max_does_not_claim_a_level);
    RUN_TEST(test_result_step_down_and_off_claims_neither_state_nor_level);
    RUN_TEST(test_result_mirror_off_infers_off);
    RUN_TEST(test_result_mirror_dim_has_no_state);
    RUN_TEST(test_result_action_off_infers_off);
    RUN_TEST(test_result_action_recall_max_does_not_claim_a_level);
    RUN_TEST(test_result_toggle_infers_state);
    RUN_TEST(test_result_null_result_out_does_not_crash);
    /* Acting on transmission */
    RUN_TEST(test_transmitted_action_reports_through_done_once_sent);
    RUN_TEST(test_failed_transmission_changes_nothing_and_says_so);
    RUN_TEST(test_cancelled_frame_changes_nothing_and_says_so);
    RUN_TEST(test_refused_frame_queues_nothing_and_never_calls_done);
    RUN_TEST(test_pending_slots_are_returned);
    RUN_TEST(test_toggle_second_press_before_the_first_is_sent_toggles_back);
    RUN_TEST(test_toggle_falls_back_when_the_queued_frame_fails);
    RUN_TEST(test_observe_is_final_not_pending);
    return UNITY_END();
}
