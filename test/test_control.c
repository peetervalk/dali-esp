/*
 * test_control.c - unit tests for the DALI command translator.
 */

#include "unity.h"
#include "dali_control.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Mock scheduler ops
 * --------------------------------------------------------------------------*/
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

static uint32_t mock_get_tick_ms(void)
{
    return s_tick_ms;
}

/* ---------------------------------------------------------------------------
 * Completion capture
 * --------------------------------------------------------------------------*/
static DaliError s_cb_result;
static DaliFrame s_cb_reply;
static uint8_t   s_cb_count;

static void on_complete(DaliError result, const DaliFrame *reply, void *cb_ctx)
{
    (void)cb_ctx;
    s_cb_result = result;
    if (reply != NULL) {
        s_cb_reply = *reply;
    } else {
        memset(&s_cb_reply, 0, sizeof(s_cb_reply));
    }
    s_cb_count++;
}

static DaliTarget target(DaliAddressType type, uint8_t address)
{
    DaliTarget t = {
        .type    = type,
        .address = address,
    };
    return t;
}

/* ---------------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------------*/
void setUp(void)
{
    memset(&s_last_tx, 0, sizeof(s_last_tx));
    memset(&s_cb_reply, 0, sizeof(s_cb_reply));
    s_tx_count  = 0u;
    s_tx_result = DALI_OK;
    s_rx_cb     = NULL;
    s_rx_ctx    = NULL;
    s_tick_ms   = 0u;
    s_cb_result = DALI_OK;
    s_cb_count  = 0u;
    memset(&g_dali_stats, 0, sizeof(g_dali_stats));

    DaliSchedOps ops = {
        .tx              = mock_tx,
        .set_rx_callback = mock_set_rx_callback,
        .get_tick_ms     = mock_get_tick_ms,
    };
    TEST_ASSERT_EQUAL(DALI_OK, dali_sched_init(&ops));
}

void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------------*/
void test_brightness_conversions(void)
{
    TEST_ASSERT_EQUAL_UINT8(0u, dali_control_ha_brightness_to_dapc(0u));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_control_ha_brightness_to_dapc(1u));
    TEST_ASSERT_EQUAL_UINT8(128u, dali_control_ha_brightness_to_dapc(128u));
    TEST_ASSERT_EQUAL_UINT8(254u, dali_control_ha_brightness_to_dapc(255u));

    TEST_ASSERT_EQUAL_UINT8(0u, dali_control_percent_to_dapc(0u));
    TEST_ASSERT_EQUAL_UINT8(128u, dali_control_percent_to_dapc(50u));
    TEST_ASSERT_EQUAL_UINT8(254u, dali_control_percent_to_dapc(100u));
    TEST_ASSERT_EQUAL_UINT8(254u, dali_control_percent_to_dapc(150u));
}

void test_build_group_dapc_example(void)
{
    DaliFrame frame;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_dapc(target(DALI_ADDR_GROUP, 0u), 128u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8080u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_build_short_and_broadcast_dapc(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_dapc(target(DALI_ADDR_SHORT, 1u), 128u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0280u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_dapc(target(DALI_ADDR_BROADCAST, 0u), 128u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFE80u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_build_off_for_group_and_broadcast(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_off(target(DALI_ADDR_GROUP, 0u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8100u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_off(target(DALI_ADDR_BROADCAST, 0u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFF00u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_build_recall_min_for_short_group_and_broadcast(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_recall_min(target(DALI_ADDR_SHORT, 5u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B06u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_recall_min(target(DALI_ADDR_GROUP, 2u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8506u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_recall_min(target(DALI_ADDR_BROADCAST, 0u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFF06u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_build_output_level_commands(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK, dali_control_build_up(target(DALI_ADDR_SHORT, 5u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B01u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK, dali_control_build_down(target(DALI_ADDR_GROUP, 2u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8502u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_step_up(target(DALI_ADDR_BROADCAST, 0u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFF03u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK, dali_control_build_step_down(target(DALI_ADDR_SHORT, 0u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0104u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_step_down_and_off(target(DALI_ADDR_GROUP, 0u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8107u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_on_and_step_up(target(DALI_ADDR_BROADCAST, 0u),
                                                        &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFF08u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_enable_dapc_sequence(target(DALI_ADDR_SHORT, 0u),
                                                              &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0109u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_go_to_last_active_level(target(DALI_ADDR_SHORT, 0u),
                                                                 &frame));
    TEST_ASSERT_EQUAL_HEX32(0x010Au, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_build_go_to_scene_for_group(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_go_to_scene(target(DALI_ADDR_GROUP, 0u), 5u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8115u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_go_to_scene(target(DALI_ADDR_SHORT, 0u),
                                                     DALI_SCENE_COUNT,
                                                     &frame));
}

void test_invalid_targets_and_levels_are_rejected(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_dapc(target(DALI_ADDR_SHORT, 64u), 1u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_dapc(target(DALI_ADDR_GROUP, 16u), 1u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_dapc(target(DALI_ADDR_GROUP, 0u), 255u, &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_dapc(target(DALI_ADDR_GROUP, 0u), 1u, NULL));
}

void test_output_level_commands_enqueue(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_step_down_and_off(target(DALI_ADDR_SHORT, 0u)));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0107u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
}

void test_go_to_scene_enqueues(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_go_to_scene(target(DALI_ADDR_GROUP, 0u), 5u));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8115u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
}

void test_build_generic_query_commands(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_query(target(DALI_ADDR_SHORT, 5u),
                                               DALI_CMD_QUERY_CONTROL_GEAR_PRESENT,
                                               0u,
                                               &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B91u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_query(target(DALI_ADDR_GROUP, 2u),
                                               DALI_CMD_QUERY_GROUPS_8_15,
                                               0u,
                                               &frame));
    TEST_ASSERT_EQUAL_HEX32(0x85C1u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_query(target(DALI_ADDR_SHORT, 5u),
                                               DALI_CMD_QUERY_SCENE_LEVEL,
                                               5u,
                                               &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0BB5u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_query(target(DALI_ADDR_SHORT, 5u),
                                               DALI_CMD_READ_MEMORY_LOCATION,
                                               0u,
                                               &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0BC5u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_generic_query_rejects_invalid_command_classes(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_query(target(DALI_ADDR_SHORT, 0u),
                                               DALI_CMD_OFF,
                                               0u,
                                               &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_query(target(DALI_ADDR_SHORT, 0u),
                                               DALI_CMD_QUERY_INSTANCE_TYPE,
                                               0u,
                                               &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_query(target(DALI_ADDR_SHORT, 0u),
                                               DALI_CMD_COMPARE,
                                               0u,
                                               &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_query(target(DALI_ADDR_SHORT, 0u),
                                               DALI_CMD_QUERY_SCENE_LEVEL,
                                               DALI_SCENE_COUNT,
                                               &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_query(target(DALI_ADDR_SHORT, 0u),
                                               DALI_CMD_QUERY_STATUS,
                                               0u,
                                               NULL));
}

void test_generic_query_enqueues_and_completes_with_reply(void)
{
    DaliFrame reply = {
        .data       = DALI_YES_RESPONSE,
        .bit_length = DALI_BACKWARD_FRAME_BITS,
    };

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_query(target(DALI_ADDR_SHORT, 5u),
                                         DALI_CMD_QUERY_CONTROL_GEAR_PRESENT,
                                         0u,
                                         on_complete,
                                         NULL));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0B91u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);

    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    dali_sched_notify_rx(&reply);
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, s_cb_result);
    TEST_ASSERT_EQUAL_HEX32(DALI_YES_RESPONSE, s_cb_reply.data);
    TEST_ASSERT_EQUAL_UINT8(DALI_BACKWARD_FRAME_BITS, s_cb_reply.bit_length);

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_query(target(DALI_ADDR_SHORT, 0u),
                                         DALI_CMD_QUERY_STATUS,
                                         0u,
                                         NULL,
                                         NULL));
}

void test_build_config_commands(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_config(target(DALI_ADDR_SHORT, 5u),
                                                DALI_CMD_RESET,
                                                0u,
                                                &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0B20u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_config(target(DALI_ADDR_GROUP, 0u),
                                                DALI_CMD_SET_SCENE,
                                                5u,
                                                &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8145u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_config(target(DALI_ADDR_BROADCAST, 0u),
                                                DALI_CMD_ADD_TO_GROUP,
                                                2u,
                                                &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFF62u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_config(target(DALI_ADDR_SHORT, 0u),
                                                DALI_CMD_ENABLE_WRITE_MEMORY,
                                                0u,
                                                &frame));
    TEST_ASSERT_EQUAL_HEX32(0x0181u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);
}

void test_build_dtr_frames(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK, dali_control_build_dtr(DALI_DTR0, 0x44u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0xA344u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK, dali_control_build_dtr(DALI_DTR1, 0x55u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0xC355u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK, dali_control_build_dtr(DALI_DTR2, 0x66u, &frame));
    TEST_ASSERT_EQUAL_HEX32(0xC566u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_dtr((DaliDtrRegister)3, 0x44u, &frame));
}

void test_config_rejects_invalid_command_classes(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_config(target(DALI_ADDR_SHORT, 0u),
                                                DALI_CMD_OFF,
                                                0u,
                                                &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_config(target(DALI_ADDR_SHORT, 0u),
                                                DALI_CMD_QUERY_STATUS,
                                                0u,
                                                &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_config(target(DALI_ADDR_SHORT, 0u),
                                                DALI_CMD_DTR0_DATA,
                                                0u,
                                                &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_config(target(DALI_ADDR_SHORT, 0u),
                                                DALI_CMD_SET_SCENE,
                                                DALI_SCENE_COUNT,
                                                &frame));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_build_config(target(DALI_ADDR_SHORT, 0u),
                                                DALI_CMD_RESET,
                                                0u,
                                                NULL));
}

void test_config_uses_dtr0_predicate(void)
{
    TEST_ASSERT_TRUE(dali_control_config_uses_dtr0(DALI_CMD_SET_MAX_LEVEL_DTR0));
    TEST_ASSERT_TRUE(dali_control_config_uses_dtr0(DALI_CMD_SET_SCENE));
    TEST_ASSERT_TRUE(dali_control_config_uses_dtr0(DALI_CMD_SET_SHORT_ADDRESS_DTR0));

    TEST_ASSERT_FALSE(dali_control_config_uses_dtr0(DALI_CMD_RESET));
    TEST_ASSERT_FALSE(dali_control_config_uses_dtr0(DALI_CMD_STORE_ACTUAL_LEVEL_DTR0));
    TEST_ASSERT_FALSE(dali_control_config_uses_dtr0(DALI_CMD_ADD_TO_GROUP));
    TEST_ASSERT_FALSE(dali_control_config_uses_dtr0(DALI_CMD_QUERY_STATUS));
}

void test_config_enqueue_sends_twice(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_config(target(DALI_ADDR_SHORT, 5u),
                                          DALI_CMD_RESET,
                                          0u));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0B20u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());

    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(2u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0B20u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());

    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(2u, s_tx_count);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_set_dtr_enqueues_special_frame(void)
{
    TEST_ASSERT_EQUAL(DALI_OK, dali_control_set_dtr(DALI_DTR2, 0x7Fu));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0xC57Fu, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
    TEST_ASSERT_EQUAL(SCHED_WAIT_SETTLE, dali_sched_state());

    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_config_with_dtr0_sequences_load_before_send_twice_config(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_config_with_dtr0(target(DALI_ADDR_SHORT, 5u),
                                                    DALI_CMD_SET_MAX_LEVEL_DTR0,
                                                    200u,
                                                    0u));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0xA3C8u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);

    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(2u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0B2Au, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);

    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(3u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0B2Au, s_last_tx.data);

    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(3u, s_tx_count);
    TEST_ASSERT_EQUAL(SCHED_IDLE, dali_sched_state());
}

void test_config_with_dtr0_rejects_non_dtr0_configs(void)
{
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_config_with_dtr0(target(DALI_ADDR_SHORT, 5u),
                                                    DALI_CMD_RESET,
                                                    1u,
                                                    0u));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_config_with_dtr0(target(DALI_ADDR_SHORT, 5u),
                                                    DALI_CMD_SET_SCENE,
                                                    1u,
                                                    DALI_SCENE_COUNT));
}

void test_set_brightness_group_enqueues_dapc(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_set_brightness(target(DALI_ADDR_GROUP, 0u), 128u));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8080u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
}

void test_set_brightness_zero_enqueues_off(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_set_brightness(target(DALI_ADDR_GROUP, 0u), 0u));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8100u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
}

void test_set_percent_group_50_enqueues_dapc_128(void)
{
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_set_percent(target(DALI_ADDR_GROUP, 0u), 50u));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x8080u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);
}

void test_query_status_short_address_completes_with_reply(void)
{
    DaliFrame reply = {
        .data       = 0xAFu,
        .bit_length = 8u,
    };

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_query_status(target(DALI_ADDR_SHORT, 5u),
                                                on_complete,
                                                NULL));

    dali_sched_run();
    TEST_ASSERT_EQUAL_UINT8(1u, s_tx_count);
    TEST_ASSERT_EQUAL_HEX32(0x0B90u, s_last_tx.data);
    TEST_ASSERT_EQUAL_UINT8(16u, s_last_tx.bit_length);

    s_tick_ms += DALI_SETTLE_MS;
    dali_sched_run();
    TEST_ASSERT_EQUAL(SCHED_WAIT_REPLY, dali_sched_state());

    dali_sched_notify_rx(&reply);
    dali_sched_run();

    TEST_ASSERT_EQUAL_UINT8(1u, s_cb_count);
    TEST_ASSERT_EQUAL(DALI_OK, s_cb_result);
    TEST_ASSERT_EQUAL_HEX32(0xAFu, s_cb_reply.data);
    TEST_ASSERT_EQUAL_UINT8(8u, s_cb_reply.bit_length);
}

void test_query_status_allows_group_broadcast_and_rejects_null_callback(void)
{
    DaliFrame frame;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_query_status(target(DALI_ADDR_GROUP, 0u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0x8190u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_build_query_status(target(DALI_ADDR_BROADCAST, 0u), &frame));
    TEST_ASSERT_EQUAL_HEX32(0xFF90u, frame.data);
    TEST_ASSERT_EQUAL_UINT8(16u, frame.bit_length);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_control_query_status(target(DALI_ADDR_GROUP, 0u),
                                                on_complete,
                                                NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_control_query_status(target(DALI_ADDR_SHORT, 0u),
                                                NULL,
                                                NULL));
}

/* ---------------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------------*/
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_brightness_conversions);
    RUN_TEST(test_build_group_dapc_example);
    RUN_TEST(test_build_short_and_broadcast_dapc);
    RUN_TEST(test_build_off_for_group_and_broadcast);
    RUN_TEST(test_build_recall_min_for_short_group_and_broadcast);
    RUN_TEST(test_build_output_level_commands);
    RUN_TEST(test_build_go_to_scene_for_group);
    RUN_TEST(test_invalid_targets_and_levels_are_rejected);
    RUN_TEST(test_output_level_commands_enqueue);
    RUN_TEST(test_go_to_scene_enqueues);
    RUN_TEST(test_build_generic_query_commands);
    RUN_TEST(test_generic_query_rejects_invalid_command_classes);
    RUN_TEST(test_generic_query_enqueues_and_completes_with_reply);
    RUN_TEST(test_build_config_commands);
    RUN_TEST(test_build_dtr_frames);
    RUN_TEST(test_config_rejects_invalid_command_classes);
    RUN_TEST(test_config_uses_dtr0_predicate);
    RUN_TEST(test_config_enqueue_sends_twice);
    RUN_TEST(test_set_dtr_enqueues_special_frame);
    RUN_TEST(test_config_with_dtr0_sequences_load_before_send_twice_config);
    RUN_TEST(test_config_with_dtr0_rejects_non_dtr0_configs);
    RUN_TEST(test_set_brightness_group_enqueues_dapc);
    RUN_TEST(test_set_brightness_zero_enqueues_off);
    RUN_TEST(test_set_percent_group_50_enqueues_dapc_128);
    RUN_TEST(test_query_status_short_address_completes_with_reply);
    RUN_TEST(test_query_status_allows_group_broadcast_and_rejects_null_callback);
    return UNITY_END();
}
