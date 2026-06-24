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
    DaliInputEvent ev = {
        .frame_kind       = DALI_EVENT_FRAME_LEGACY_16BIT,
        .raw              = { .data = ((uint32_t)addr_byte << 8u) | opcode, .bit_length = 16u },
        .address_byte     = addr_byte,
        .address_kind     = kind,
        .address          = address,
        .address_selector = selector,
        .has_instance     = false,
        .instance         = 0xFFu,
        .event_code       = opcode,
    };
    return ev;
}

static DaliInputEvent make_dali2(DaliEventAddressKind kind,
                                 uint8_t              address,
                                 uint8_t              instance,
                                 uint8_t              event_code)
{
    uint8_t addr_byte = (kind == DALI_EVENT_ADDRESS_GROUP)
                      ? (uint8_t)(0x80u | ((address & 0x0Fu) << 1u) | 1u)
                      : (uint8_t)(((address & 0x3Fu) << 1u) | 1u);
    DaliInputEvent ev = {
        .frame_kind       = DALI_EVENT_FRAME_INPUT_24BIT,
        .raw              = { .data = ((uint32_t)addr_byte << 16u) |
                                      ((uint32_t)instance << 8u) | event_code,
                              .bit_length = 24u },
        .address_byte     = addr_byte,
        .address_kind     = kind,
        .address          = address,
        .address_selector = true,
        .has_instance     = true,
        .instance         = instance,
        .event_code       = event_code,
    };
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

    DaliSchedOps ops = {
        .tx              = mock_tx,
        .set_rx_callback = mock_set_rx_callback,
        .get_tick_ms     = mock_get_tick_ms,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_init(&ops));
}

void tearDown(void) {}

/* ── Tests ───────────────────────────────────────────────────────────────── */

void test_null_args_return_invalid(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY },
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
            DALI_DISPATCH_OPCODE_ANY },
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
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_OBSERVE, 0 },
    };
    DaliDispatchToggleState state = {};
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
    TEST_ASSERT_TRUE((state.group_on >> 6u) & 1u);
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_TRUE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(254u, result.level);
    TEST_ASSERT_EQUAL_INT(DALI_ADDR_GROUP, result.target.type);
    TEST_ASSERT_EQUAL_UINT8(6u, result.target.address);
}

void test_observe_off_infers_off_without_tx(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x00u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY },
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
            DALI_DISPATCH_OPCODE_ANY },
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

void test_mirror_recall_max_issues_correct_frame(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY },
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
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, NULL));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8D00u, s_last_tx.data);
}

void test_mirror_skips_dapc_frame(void)
{
    /* address_selector = false → DAPC, not a command */
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, false, 0x80u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };

    DaliError err = dali_dispatch(table, 1u, &ev, NULL, NULL);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, err);
    TEST_ASSERT_EQUAL_UINT8(0u, s_tx_count);
}

void test_mirror_phantom_remap_to_different_group(void)
{
    /* Phantom address 16 → real group 6 (Approach B) */
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_SHORT, 16, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_SHORT, 16,
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, NULL));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    /* output is group 6 RECALL MAX, not short 16 */
    TEST_ASSERT_EQUAL_HEX32(0x8D05u, s_last_tx.data);
}

void test_event_code_specific_only_fires_on_match(void)
{
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0, 0x05u },
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
            DALI_DISPATCH_OPCODE_ANY },
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
            DALI_DISPATCH_OPCODE_ANY },
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
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 3, 0x02u },
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
    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(2u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8100u, s_last_tx.data); /* group 0 OFF */

    /* Third press: back to on */
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, NULL));
    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(3u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8105u, s_last_tx.data); /* group 0 RECALL MAX */
}

void test_mirror_updates_toggle_state(void)
{
    DaliInputEvent ev_on  = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x05u);
    DaliInputEvent ev_off = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x00u);
    DaliDispatchEntry mirror_table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };
    DaliDispatchToggleState state = {};

    dali_dispatch(mirror_table, 1u, &ev_on,  &state, NULL);
    TEST_ASSERT_TRUE((state.group_on >> 6u) & 1u);

    dali_dispatch(mirror_table, 1u, &ev_off, &state, NULL);
    TEST_ASSERT_FALSE((state.group_on >> 6u) & 1u);
}

void test_first_matching_entry_wins(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_RECALL_MAX, 0 },
        /* Second matching entry — must be skipped */
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 7 }, DALI_DISPATCH_ACTION_OFF, 0 },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 2u, &ev, NULL, NULL));
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8105u, s_last_tx.data); /* group 0 RECALL MAX, not group 7 OFF */
}

/* ── Result inference tests ──────────────────────────────────────────────── */

void test_result_mirror_recall_max_infers_on_level_254(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x05u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 6 }, DALI_DISPATCH_ACTION_MIRROR, 0 },
    };
    DaliDispatchResult result = { .has_state = false };

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, &result));
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_TRUE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(254u, result.level);
    TEST_ASSERT_EQUAL_INT(DALI_ADDR_GROUP, result.target.type);
    TEST_ASSERT_EQUAL_UINT8(6u, result.target.address);
}

void test_result_mirror_off_infers_off(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 6, true, 0x00u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 6,
            DALI_DISPATCH_OPCODE_ANY },
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
            DALI_DISPATCH_OPCODE_ANY },
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
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 3 }, DALI_DISPATCH_ACTION_OFF, 0 },
    };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, &result));
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_FALSE(result.is_on);
    TEST_ASSERT_EQUAL_INT(DALI_ADDR_GROUP, result.target.type);
    TEST_ASSERT_EQUAL_UINT8(3u, result.target.address);
}

void test_result_action_recall_max_infers_on(void)
{
    DaliInputEvent ev = make_legacy(DALI_EVENT_ADDRESS_GROUP, 0, true, 0x00u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_LEGACY_16BIT, DALI_EVENT_ADDRESS_GROUP, 0,
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 5 }, DALI_DISPATCH_ACTION_RECALL_MAX, 0 },
    };
    DaliDispatchResult result = {};

    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, &result));
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_TRUE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(254u, result.level);
    TEST_ASSERT_EQUAL_UINT8(5u, result.target.address);
}

void test_result_toggle_infers_state(void)
{
    DaliInputEvent ev = make_dali2(DALI_EVENT_ADDRESS_SHORT, 1, 0, 0x02u);
    DaliDispatchEntry table[] = {
        { { DALI_EVENT_FRAME_INPUT_24BIT, DALI_EVENT_ADDRESS_SHORT, 1, 0x02u },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_TOGGLE, 0 },
    };
    DaliDispatchToggleState state = {};
    DaliDispatchResult result = {};

    /* First press: off → on */
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, &state, &result));
    TEST_ASSERT_TRUE(result.has_state);
    TEST_ASSERT_TRUE(result.is_on);
    TEST_ASSERT_EQUAL_UINT8(254u, result.level);

    /* Second press: on → off */
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
            DALI_DISPATCH_OPCODE_ANY },
          { DALI_ADDR_GROUP, 0 }, DALI_DISPATCH_ACTION_RECALL_MAX, 0 },
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_dispatch(table, 1u, &ev, NULL, NULL));
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
    RUN_TEST(test_mirror_recall_max_issues_correct_frame);
    RUN_TEST(test_mirror_off_issues_correct_frame);
    RUN_TEST(test_mirror_skips_dapc_frame);
    RUN_TEST(test_mirror_phantom_remap_to_different_group);
    RUN_TEST(test_event_code_specific_only_fires_on_match);
    RUN_TEST(test_action_off_sends_off_frame);
    RUN_TEST(test_action_scene_sends_correct_frame);
    RUN_TEST(test_toggle_starts_off_and_flips);
    RUN_TEST(test_mirror_updates_toggle_state);
    RUN_TEST(test_first_matching_entry_wins);
    /* DaliDispatchResult inference */
    RUN_TEST(test_result_mirror_recall_max_infers_on_level_254);
    RUN_TEST(test_result_mirror_off_infers_off);
    RUN_TEST(test_result_mirror_dim_has_no_state);
    RUN_TEST(test_result_action_off_infers_off);
    RUN_TEST(test_result_action_recall_max_infers_on);
    RUN_TEST(test_result_toggle_infers_state);
    RUN_TEST(test_result_null_result_out_does_not_crash);
    return UNITY_END();
}
