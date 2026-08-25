#include "unity.h"
#include "dali_commissioning.h"

#include <string.h>

#define MOCK_DEVICE_COUNT 4u
#define MOCK_LOG_MAX 520u
#define MOCK_UNADDRESSED 0xFFu

typedef struct {
    bool     present;
    bool     active;
    uint32_t random_address;
    uint8_t  short_address;
} MockDevice;

typedef struct {
    uint32_t data;
    uint8_t  bit_length;
    bool     needs_reply;
    bool     send_twice;
    uint8_t  retries_left;
} MockTxLog;

typedef struct {
    MockDevice devices[MOCK_DEVICE_COUNT];
    MockTxLog  log[MOCK_LOG_MAX];
    uint16_t   log_count;
    uint32_t   search_address;
    uint8_t    initialise_count;
    uint8_t    randomize_count;
    uint8_t    withdraw_count;
    uint8_t    fail_opcode;   /* 0 = no injected fault */
    DaliError  fail_err;
    bool       cancelled;
    uint8_t    cleanup_count;
    DaliError  cleanup_error;
    bool       sequence_timeout_with_unknown_progress;
    uint8_t    delay_count;
    uint32_t   delay_total_ms;
    /* Frame most recently on the bus when a settle wait was requested. */
    uint32_t   delay_last_frame_data;
} MockCommissioningBus;

static MockCommissioningBus s_bus;

static void mock_add_device(uint8_t slot, uint32_t random_address)
{
    TEST_ASSERT_LESS_THAN_UINT8(MOCK_DEVICE_COUNT, slot);
    s_bus.devices[slot] = (MockDevice){
        .present = true,
        .active = false,
        .random_address = random_address,
        .short_address = MOCK_UNADDRESSED,
    };
}

static void mock_log_tx(const DaliFrame *frame,
                        bool needs_reply,
                        uint8_t retries_left,
                        bool send_twice)
{
    TEST_ASSERT_LESS_THAN_UINT16(MOCK_LOG_MAX, s_bus.log_count);
    s_bus.log[s_bus.log_count++] = (MockTxLog){
        .data = frame->data,
        .bit_length = frame->bit_length,
        .needs_reply = needs_reply,
        .send_twice = send_twice,
        .retries_left = retries_left,
    };
}

static bool mock_selected(const MockDevice *device)
{
    return device != NULL &&
           device->present &&
           device->active &&
           device->random_address <= s_bus.search_address;
}

static bool mock_any_selected(void)
{
    for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
        if (mock_selected(&s_bus.devices[i])) {
            return true;
        }
    }
    return false;
}

static MockDevice *mock_first_selected(void)
{
    for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
        if (mock_selected(&s_bus.devices[i])) {
            return &s_bus.devices[i];
        }
    }
    return NULL;
}

static DaliError mock_special_no_reply(uint8_t opcode, uint8_t param)
{
    if (s_bus.fail_opcode != 0u && opcode == s_bus.fail_opcode) {
        return s_bus.fail_err;
    }

    switch (opcode) {
        case 0xA1u:
            for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                s_bus.devices[i].active = false;
            }
            return DALI_OK;

        case 0xA5u:
            TEST_ASSERT_EQUAL_HEX8(DALI_INITIALISE_UNADDRESSED_PARAM, param);
            s_bus.initialise_count++;
            for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                s_bus.devices[i].active =
                    s_bus.devices[i].present &&
                    s_bus.devices[i].short_address == MOCK_UNADDRESSED;
            }
            return DALI_OK;

        case 0xA7u:
            s_bus.randomize_count++;
            return DALI_OK;

        case 0xABu:
            for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                if (mock_selected(&s_bus.devices[i])) {
                    s_bus.devices[i].active = false;
                    s_bus.withdraw_count++;
                }
            }
            return DALI_OK;

        case 0xB1u:
            s_bus.search_address =
                (s_bus.search_address & 0x00FFFFu) | ((uint32_t)param << 16u);
            return DALI_OK;

        case 0xB3u:
            s_bus.search_address =
                (s_bus.search_address & 0xFF00FFu) | ((uint32_t)param << 8u);
            return DALI_OK;

        case 0xB5u:
            s_bus.search_address = (s_bus.search_address & 0xFFFF00u) | param;
            return DALI_OK;

        case 0xB7u: {
            uint8_t short_address = 0u;
            DaliError err = dali_commissioning_decode_short_address(param,
                                                                    &short_address);
            if (err != DALI_OK) {
                return err;
            }
            for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                if (mock_selected(&s_bus.devices[i])) {
                    s_bus.devices[i].short_address = short_address;
                }
            }
            return DALI_OK;
        }

        default:
            return DALI_ERR_INVALID;
    }
}

static DaliError mock_special_reply(uint8_t opcode,
                                    uint8_t param,
                                    DaliFrame *reply_out)
{
    TEST_ASSERT_NOT_NULL(reply_out);

    switch (opcode) {
        case 0xA9u:
            if (!mock_any_selected()) {
                return DALI_ERR_TIMEOUT;
            }
            *reply_out = (DaliFrame){
                .data = DALI_YES_RESPONSE,
                .bit_length = DALI_BACKWARD_FRAME_BITS,
            };
            return DALI_OK;

        case 0xB9u: {
            uint8_t short_address = 0u;
            DaliError err = dali_commissioning_decode_short_address(param,
                                                                    &short_address);
            if (err != DALI_OK) {
                return err;
            }
            bool verified = false;
            for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                if (mock_selected(&s_bus.devices[i]) &&
                    s_bus.devices[i].short_address == short_address) {
                    verified = true;
                }
            }
            if (!verified) {
                return DALI_ERR_TIMEOUT;
            }
            *reply_out = (DaliFrame){
                .data = DALI_YES_RESPONSE,
                .bit_length = DALI_BACKWARD_FRAME_BITS,
            };
            return DALI_OK;
        }

        case 0xBBu: {
            MockDevice *device = mock_first_selected();
            if (device == NULL || device->short_address == MOCK_UNADDRESSED) {
                return DALI_ERR_TIMEOUT;
            }
            *reply_out = (DaliFrame){
                .data = dali_commissioning_encode_short_address(device->short_address),
                .bit_length = DALI_BACKWARD_FRAME_BITS,
            };
            return DALI_OK;
        }

        default:
            return DALI_ERR_INVALID;
    }
}

static DaliError mock_transact(const DaliFrame *frame,
                               bool needs_reply,
                               uint8_t retries_left,
                               bool send_twice,
                               DaliFrame *reply_out,
                               void *ctx)
{
    (void)ctx;
    if (s_bus.cancelled) {
        return DALI_ERR_CANCELLED;
    }
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_EQUAL_UINT8(DALI_FORWARD_FRAME_BITS, frame->bit_length);
    mock_log_tx(frame, needs_reply, retries_left, send_twice);

    uint8_t opcode = (uint8_t)((frame->data >> 8u) & 0xFFu);
    uint8_t param = (uint8_t)(frame->data & 0xFFu);
    if (needs_reply) {
        TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_QUERY_RETRIES_LEFT,
                                retries_left);
        TEST_ASSERT_FALSE(send_twice);
        return mock_special_reply(opcode, param, reply_out);
    }

    TEST_ASSERT_EQUAL_UINT8(0u, retries_left);
    return mock_special_no_reply(opcode, param);
}

static DaliError mock_cleanup_transact(const DaliFrame *frame,
                                       bool needs_reply,
                                       uint8_t retries_left,
                                       bool send_twice,
                                       DaliFrame *reply_out,
                                       void *ctx)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_FALSE(needs_reply);
    TEST_ASSERT_EQUAL_UINT8(0u, retries_left);
    TEST_ASSERT_FALSE(send_twice);
    TEST_ASSERT_NULL(reply_out);

    s_bus.cleanup_count++;
    mock_log_tx(frame, false, 0u, false);
    if (s_bus.cleanup_error != DALI_OK) {
        return s_bus.cleanup_error;
    }

    uint8_t opcode = (uint8_t)((frame->data >> 8u) & 0xFFu);
    uint8_t param = (uint8_t)(frame->data & 0xFFu);
    return mock_special_no_reply(opcode, param);
}

static DaliError mock_transact_sequence(const DaliSequence *seq,
                                         DaliSequenceResult *result_out,
                                         void *ctx)
{
    if (seq == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliSequenceResult result = {
        .result = DALI_OK,
        .failed_step = DALI_SEQUENCE_NO_FAILED_STEP,
    };
    if (s_bus.sequence_timeout_with_unknown_progress) {
        result.result = DALI_ERR_TIMEOUT;
        if (result_out != NULL) {
            *result_out = result;
        }
        return result.result;
    }
    for (uint8_t i = 0u; i < seq->step_count; i++) {
        const DaliSequenceStep *step = &seq->steps[i];
        DaliFrame reply = {0u, 0u};
        DaliError err = mock_transact(&step->frame,
                                      step->needs_reply,
                                      step->retries_left,
                                      step->send_twice,
                                      step->needs_reply ? &reply : NULL,
                                      ctx);
        result.steps_run = (uint8_t)(i + 1u);
        if (err != DALI_OK) {
            result.result = err;
            result.failed_step = i;
            if (result_out != NULL) {
                *result_out = result;
            }
            return err;
        }
        if (step->needs_reply) {
            result.replies[i] = reply;
            result.reply_mask |= (uint8_t)(1u << i);
        }
    }

    if (result_out != NULL) {
        *result_out = result;
    }
    return DALI_OK;
}

static void mock_delay_ms(uint32_t ms, void *ctx)
{
    (void)ctx;
    s_bus.delay_count++;
    s_bus.delay_total_ms += ms;
    s_bus.delay_last_frame_data =
        s_bus.log_count > 0u ? s_bus.log[s_bus.log_count - 1u].data : 0u;
}

static DaliDiscoveryTransport transport(void)
{
    return (DaliDiscoveryTransport){
        .transact = mock_transact,
        .transact_sequence = mock_transact_sequence,
        .ctx = NULL,
        .transact_cleanup = mock_cleanup_transact,
        .delay_ms = mock_delay_ms,
    };
}

static DaliError mock_rx_activity_transact(const DaliFrame *frame,
                                           bool needs_reply,
                                           uint8_t retries_left,
                                           bool send_twice,
                                           DaliFrame *reply_out,
                                           void *ctx)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_TRUE(needs_reply);
    TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_QUERY_RETRIES_LEFT,
                            retries_left);
    TEST_ASSERT_FALSE(send_twice);
    TEST_ASSERT_NOT_NULL(reply_out);
    return DALI_ERR_RX_ACTIVITY;
}

static DaliError mock_non_yes_transact(const DaliFrame *frame,
                                       bool needs_reply,
                                       uint8_t retries_left,
                                       bool send_twice,
                                       DaliFrame *reply_out,
                                       void *ctx)
{
    (void)frame;
    (void)retries_left;
    (void)send_twice;
    (void)ctx;
    TEST_ASSERT_TRUE(needs_reply);
    TEST_ASSERT_NOT_NULL(reply_out);
    *reply_out = (DaliFrame){
        .data = 0x00u,
        .bit_length = DALI_BACKWARD_FRAME_BITS,
    };
    return DALI_OK;
}

void setUp(void)
{
    memset(&s_bus, 0, sizeof(s_bus));
    for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
        s_bus.devices[i].short_address = MOCK_UNADDRESSED;
    }
}

void tearDown(void) {}

void test_short_address_encoding_round_trip(void)
{
    uint8_t decoded = 0u;

    TEST_ASSERT_EQUAL_HEX8(0x01u, dali_commissioning_encode_short_address(0u));
    TEST_ASSERT_EQUAL_HEX8(0x7Fu, dali_commissioning_encode_short_address(63u));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_decode_short_address(0x25u, &decoded));
    TEST_ASSERT_EQUAL_UINT8(18u, decoded);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_decode_short_address(0x24u, &decoded));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_decode_short_address(0xFFu, &decoded));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_decode_short_address(0x01u, NULL));
}

void test_set_search_address_sends_h_m_l_special_commands(void)
{
    DaliDiscoveryTransport t = transport();

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_set_search_address(&t, 0x123456u));

    TEST_ASSERT_EQUAL_UINT16(3u, s_bus.log_count);
    TEST_ASSERT_EQUAL_HEX32(0xB112u, s_bus.log[0].data);
    TEST_ASSERT_EQUAL_HEX32(0xB334u, s_bus.log[1].data);
    TEST_ASSERT_EQUAL_HEX32(0xB556u, s_bus.log[2].data);
    TEST_ASSERT_FALSE(s_bus.log[0].needs_reply);
    TEST_ASSERT_FALSE(s_bus.log[1].send_twice);
    TEST_ASSERT_EQUAL_HEX32(0x123456u, s_bus.search_address);
}

void test_compare_timeout_is_no(void)
{
    DaliDiscoveryTransport t = transport();
    bool yes = true;

    TEST_ASSERT_EQUAL(DALI_OK, dali_commissioning_compare(&t, &yes));

    TEST_ASSERT_FALSE(yes);
    TEST_ASSERT_EQUAL_UINT16(1u, s_bus.log_count);
    TEST_ASSERT_EQUAL_HEX32(0xA900u, s_bus.log[0].data);
    TEST_ASSERT_TRUE(s_bus.log[0].needs_reply);
}

void test_set_search_address_rejects_frame_only_transport_without_traffic(void)
{
    DaliDiscoveryTransport frame_only = {
        .transact = mock_transact,
        .ctx = NULL,
    };

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_set_search_address(&frame_only,
                                                            0x123456u));
    TEST_ASSERT_EQUAL_UINT16(0u, s_bus.log_count);
}

void test_find_next_random_address_binary_searches_lowest_active_device(void)
{
    DaliDiscoveryTransport t = transport();
    uint32_t random_address = 0u;
    bool found = false;

    mock_add_device(0u, 0x345678u);
    mock_add_device(1u, 0x123456u);
    s_bus.devices[0].active = true;
    s_bus.devices[1].active = true;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_find_next_random_address(&t,
                                                                  &random_address,
                                                                  &found));

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_HEX32(0x123456u, random_address);
    TEST_ASSERT_EQUAL_HEX32(0x123456u, s_bus.search_address);
}

void test_commission_unaddressed_assigns_free_short_addresses_in_order(void)
{
    DaliDiscoveryTransport t = transport();
    DaliCommissioningResult result;

    mock_add_device(0u, 0x010203u);
    mock_add_device(1u, 0xA0B0C0u);

    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 0u,
        .used_address_mask = ((uint64_t)1u << 0u) | ((uint64_t)1u << 1u),
        .query_short_address = true,
    };

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_commission_unaddressed(&t,
                                                               &options,
                                                               &result,
                                                               NULL,
                                                               NULL));

    TEST_ASSERT_EQUAL_UINT8(2u, result.assigned_count);
    TEST_ASSERT_TRUE(result.no_more_devices);
    TEST_ASSERT_FALSE(result.address_space_full);
    TEST_ASSERT_EQUAL(DALI_OK, result.last_error);
    TEST_ASSERT_TRUE(result.termination_required);
    TEST_ASSERT_TRUE(result.termination_attempted);
    TEST_ASSERT_TRUE(result.terminate_tx_succeeded);
    TEST_ASSERT_FALSE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL(DALI_OK, result.cleanup_error);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.cleanup_count);

    TEST_ASSERT_EQUAL_HEX32(0x010203u, result.assignments[0].random_address);
    TEST_ASSERT_EQUAL_UINT8(2u, result.assignments[0].short_address);
    TEST_ASSERT_TRUE(result.assignments[0].has_query_short);
    TEST_ASSERT_EQUAL_HEX8(dali_commissioning_encode_short_address(2u),
                           result.assignments[0].query_short_raw);
    TEST_ASSERT_EQUAL_UINT8(2u, result.assignments[0].query_short_address);

    TEST_ASSERT_EQUAL_HEX32(0xA0B0C0u, result.assignments[1].random_address);
    TEST_ASSERT_EQUAL_UINT8(3u, result.assignments[1].short_address);

    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.initialise_count);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.randomize_count);
    TEST_ASSERT_EQUAL_UINT8(2u, s_bus.withdraw_count);
    TEST_ASSERT_EQUAL_UINT8(2u, s_bus.devices[0].short_address);
    TEST_ASSERT_EQUAL_UINT8(3u, s_bus.devices[1].short_address);

    TEST_ASSERT_EQUAL_HEX32(0xA100u, s_bus.log[0].data);
    TEST_ASSERT_EQUAL_HEX32(0xA5FFu, s_bus.log[1].data);
    TEST_ASSERT_TRUE(s_bus.log[1].send_twice);
    TEST_ASSERT_EQUAL_HEX32(0xA700u, s_bus.log[2].data);
    TEST_ASSERT_TRUE(s_bus.log[2].send_twice);
    TEST_ASSERT_EQUAL_HEX32(0xA100u, s_bus.log[s_bus.log_count - 1u].data);
}

void test_commission_unaddressed_respects_max_devices(void)
{
    DaliDiscoveryTransport t = transport();
    DaliCommissioningResult result;

    mock_add_device(0u, 0x010203u);
    mock_add_device(1u, 0x020304u);

    DaliCommissioningOptions options = {
        .first_short_address = 10u,
        .max_devices = 1u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_commission_unaddressed(&t,
                                                               &options,
                                                               &result,
                                                               NULL,
                                                               NULL));

    TEST_ASSERT_EQUAL_UINT8(1u, result.assigned_count);
    TEST_ASSERT_FALSE(result.no_more_devices);
    TEST_ASSERT_EQUAL_UINT8(10u, result.assignments[0].short_address);
    TEST_ASSERT_EQUAL_UINT8(10u, s_bus.devices[0].short_address);
    TEST_ASSERT_EQUAL_UINT8(MOCK_UNADDRESSED, s_bus.devices[1].short_address);
}

void test_commission_unaddressed_returns_without_tx_when_address_space_full(void)
{
    DaliDiscoveryTransport t = transport();
    DaliCommissioningResult result;

    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 0u,
        .used_address_mask = UINT64_MAX,
        .query_short_address = false,
    };

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_commission_unaddressed(&t,
                                                               &options,
                                                               &result,
                                                               NULL,
                                                               NULL));

    TEST_ASSERT_EQUAL_UINT8(0u, result.assigned_count);
    TEST_ASSERT_TRUE(result.address_space_full);
    TEST_ASSERT_FALSE(result.termination_required);
    TEST_ASSERT_FALSE(result.termination_attempted);
    TEST_ASSERT_FALSE(result.terminate_tx_succeeded);
    TEST_ASSERT_FALSE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL(DALI_OK, result.cleanup_error);
    TEST_ASSERT_EQUAL_UINT8(0u, s_bus.cleanup_count);
    TEST_ASSERT_EQUAL_UINT16(0u, s_bus.log_count);
}

void test_commission_unaddressed_rejects_frame_only_transport_without_traffic(void)
{
    DaliDiscoveryTransport frame_only = {
        .transact = mock_transact,
        .transact_cleanup = mock_cleanup_transact,
    };
    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 1u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_commission_unaddressed(
                          &frame_only, &options, &result, NULL, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, result.last_error);
    TEST_ASSERT_FALSE(result.termination_required);
    TEST_ASSERT_FALSE(result.termination_attempted);
    TEST_ASSERT_EQUAL_UINT8(0u, s_bus.cleanup_count);
    TEST_ASSERT_EQUAL_UINT16(0u, s_bus.log_count);
}

void test_inventory_used_mask_marks_control_gear_only(void)
{
    DaliDiscoveryInventory inventory;
    DaliDiscoveryInputDevice input = {
        .device = {
            .address = 5u,
            .has_instance_count = true,
            .instance_count = 1u,
        },
    };

    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_inventory_reset(&inventory));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_inventory_store_status(&inventory,
                                                           4u,
                                                           0x00u));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_inventory_store_status(&inventory,
                                                           63u,
                                                           0x00u));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_inventory_update_input_device(&inventory,
                                                                   &input));

    uint64_t mask = dali_commissioning_used_mask_from_inventory(&inventory);
    TEST_ASSERT_NOT_EQUAL(0u, (mask & ((uint64_t)1u << 4u)));
    TEST_ASSERT_NOT_EQUAL(0u, (mask & ((uint64_t)1u << 63u)));
    TEST_ASSERT_EQUAL(0u, (mask & ((uint64_t)1u << 5u)));
}

void test_inventory_used_mask_reserves_undecodable_addresses(void)
{
    DaliDiscoveryInventory inventory;

    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_inventory_reset(&inventory));

    /*
     * An address that answered a gear QUERY STATUS with undecodable activity is
     * occupied even though nothing could be read from it. Handing it back as
     * free would let a run assign a further device onto a contested address.
     */
    inventory.devices[9u].has_undecodable_activity = true;
    inventory.undecodable_count = 1u;

    uint64_t mask = dali_commissioning_used_mask_from_inventory(&inventory);
    TEST_ASSERT_NOT_EQUAL(0u, (mask & ((uint64_t)1u << 9u)));
    TEST_ASSERT_EQUAL(0u, (mask & ((uint64_t)1u << 10u)));
}

void test_commission_skips_undecodable_addresses(void)
{
    DaliDiscoveryInventory inventory;
    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_inventory_reset(&inventory));
    inventory.devices[0u].has_undecodable_activity = true;

    DaliDiscoveryTransport t = transport();
    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 1u,
        .used_address_mask = dali_commissioning_used_mask_from_inventory(&inventory),
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    mock_add_device(0u, 0x000123u);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_commission_unaddressed(&t,
                                                                &options,
                                                                &result,
                                                                NULL,
                                                                NULL));

    /* Address 0 is contested, so the first assignment lands on 1. */
    TEST_ASSERT_EQUAL_UINT8(1u, result.assigned_count);
    TEST_ASSERT_EQUAL_UINT8(1u, result.assignments[0].short_address);
}

void test_opening_settles_after_randomize_before_first_compare(void)
{
    DaliDiscoveryTransport t = transport();
    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 1u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    mock_add_device(0u, 0x000123u);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_commission_unaddressed(&t,
                                                                &options,
                                                                &result,
                                                                NULL,
                                                                NULL));

    /*
     * Exactly one settle, of the documented duration, and taken while RANDOMIZE
     * was the last frame on the bus. Gear that has not finished generating its
     * random address answers no COMPARE, which reads as an empty bus rather than
     * as an error — so the wait existing is not enough, it has to land here.
     */
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.delay_count);
    TEST_ASSERT_EQUAL_UINT32(DALI_COMMISSIONING_RANDOMISE_SETTLE_MS,
                             s_bus.delay_total_ms);
    TEST_ASSERT_EQUAL_HEX32(0xA700u, s_bus.delay_last_frame_data);
}

void test_commission_rejects_transport_without_settle_wait_and_sends_nothing(void)
{
    /* Atomic sequences, cleanup, everything but the settle wait. */
    DaliDiscoveryTransport no_delay = {
        .transact = mock_transact,
        .transact_sequence = mock_transact_sequence,
        .ctx = NULL,
        .transact_cleanup = mock_cleanup_transact,
    };
    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 1u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    mock_add_device(0u, 0x000123u);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_commission_unaddressed(&no_delay,
                                                                &options,
                                                                &result,
                                                                NULL,
                                                                NULL));

    /* Refused before INITIALISE, so no gear is left in initialisation state. */
    TEST_ASSERT_EQUAL_UINT16(0u, s_bus.log_count);
    TEST_ASSERT_EQUAL_UINT8(0u, s_bus.initialise_count);
    TEST_ASSERT_EQUAL_UINT8(0u, s_bus.cleanup_count);
    TEST_ASSERT_FALSE(result.termination_required);
    TEST_ASSERT_FALSE(result.initialisation_state_unknown);
}

void test_invalid_arguments_are_rejected(void)
{
    DaliDiscoveryTransport t = transport();
    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 0u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;
    bool yes = false;
    uint32_t random_address = 0u;
    bool found = false;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_set_search_address(NULL, 0u));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_set_search_address(&t,
                                                           DALI_RANDOM_ADDRESS_MAX + 1u));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_compare(&t, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_find_next_random_address(&t,
                                                                  NULL,
                                                                  &found));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_find_next_random_address(&t,
                                                                  &random_address,
                                                                  NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_program_short_address(
                          &t,
                          DALI_SHORT_ADDRESS_COUNT));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_verify_short_address(&t, 0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_query_short_address(&t, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_commission_unaddressed(NULL,
                                                               &options,
                                                               &result,
                                                               NULL,
                                                               NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_commission_unaddressed(&t,
                                                               NULL,
                                                               &result,
                                                               NULL,
                                                               NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_commission_unaddressed(&t,
                                                               &options,
                                                               NULL,
                                                               NULL,
                                                               NULL));
    options.first_short_address = DALI_SHORT_ADDRESS_COUNT;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_commission_unaddressed(&t,
                                                               &options,
                                                               &result,
                                                               NULL,
                                                               NULL));

    TEST_ASSERT_EQUAL(DALI_OK, dali_commissioning_compare(&t, &yes));
    TEST_ASSERT_FALSE(yes);
}

/* ---------------------------------------------------------------------------
 * Sequence builders and result readers
 *
 * Frames are derived from the IEC 62386-102 special-command opcodes rather than
 * from the builders: SEARCH ADDRH/M/L are 0xB1/0xB3/0xB5, COMPARE is 0xA9,
 * PROGRAM SHORT ADDRESS is 0xB7 and VERIFY SHORT ADDRESS is 0xB9, each carrying
 * its parameter in the low byte.
 * --------------------------------------------------------------------------*/

void test_build_start_sequence_layout(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK, dali_commissioning_build_start_sequence(&seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_START_SEQUENCE_STEPS,
                            seq.step_count);

    /* TERMINATE 0xA1, INITIALISE 0xA5 with the unaddressed parameter,
     * RANDOMIZE 0xA7. */
    const DaliSequenceStep *terminate =
        &seq.steps[DALI_COMMISSIONING_START_STEP_TERMINATE];
    TEST_ASSERT_EQUAL_HEX32(0xA100u, terminate->frame.data);
    TEST_ASSERT_FALSE(terminate->send_twice);

    const DaliSequenceStep *initialise =
        &seq.steps[DALI_COMMISSIONING_START_STEP_INITIALISE];
    TEST_ASSERT_EQUAL_HEX32(
        (uint32_t)(0xA500u | DALI_INITIALISE_UNADDRESSED_PARAM),
        initialise->frame.data);
    TEST_ASSERT_TRUE(initialise->send_twice);

    const DaliSequenceStep *randomize =
        &seq.steps[DALI_COMMISSIONING_START_STEP_RANDOMIZE];
    TEST_ASSERT_EQUAL_HEX32(0xA700u, randomize->frame.data);
    TEST_ASSERT_TRUE(randomize->send_twice);

    for (uint8_t i = 0u; i < DALI_COMMISSIONING_START_SEQUENCE_STEPS; i++) {
        TEST_ASSERT_EQUAL_UINT8(DALI_FORWARD_FRAME_BITS,
                                seq.steps[i].frame.bit_length);
        TEST_ASSERT_FALSE(seq.steps[i].needs_reply);
        /* A repeated RANDOMIZE would hand out fresh random addresses. */
        TEST_ASSERT_EQUAL_UINT8(0u, seq.steps[i].retries_left);
    }

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_build_start_sequence(NULL));
}

static uint16_t count_frames_with_opcode(uint8_t opcode)
{
    uint16_t count = 0u;
    for (uint16_t i = 0u; i < s_bus.log_count; i++) {
        if ((uint8_t)((s_bus.log[i].data >> 8u) & 0xFFu) == opcode) {
            count++;
        }
    }
    return count;
}

static void cancel_after_randomized(const DaliCommissioningEvent *event,
                                    void *ctx)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(event);
    if (event->kind == DALI_COMMISSIONING_EVENT_RANDOMISED) {
        s_bus.cancelled = true;
    }
}

void test_cancellation_after_start_uses_safety_terminate(void)
{
    DaliDiscoveryTransport t = transport();
    mock_add_device(0u, 0x000010u);

    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 1u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_CANCELLED,
                      dali_commissioning_commission_unaddressed(
                          &t,
                          &options,
                          &result,
                          cancel_after_randomized,
                          NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_CANCELLED, result.last_error);
    TEST_ASSERT_TRUE(result.termination_required);
    TEST_ASSERT_TRUE(result.termination_attempted);
    TEST_ASSERT_TRUE(result.terminate_tx_succeeded);
    TEST_ASSERT_FALSE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL(DALI_OK, result.cleanup_error);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.cleanup_count);
    TEST_ASSERT_EQUAL_UINT16(2u, count_frames_with_opcode(0xA1u));
    TEST_ASSERT_FALSE(s_bus.devices[0].active);
}

void test_opening_sequence_unknown_progress_still_uses_safety_terminate(void)
{
    DaliDiscoveryTransport t = transport();
    s_bus.sequence_timeout_with_unknown_progress = true;

    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 1u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT,
                      dali_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, result.last_error);
    TEST_ASSERT_TRUE(result.termination_required);
    TEST_ASSERT_TRUE(result.termination_attempted);
    TEST_ASSERT_TRUE(result.terminate_tx_succeeded);
    TEST_ASSERT_FALSE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL(DALI_OK, result.cleanup_error);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.cleanup_count);
    TEST_ASSERT_EQUAL_UINT16(1u, count_frames_with_opcode(0xA1u));
}

void test_cancelled_fallback_cleanup_reports_unconfirmed_initialisation_state(void)
{
    DaliDiscoveryTransport t = transport();
    t.transact_cleanup = NULL;
    mock_add_device(0u, 0x000010u);

    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 1u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_CANCELLED,
                      dali_commissioning_commission_unaddressed(
                          &t,
                          &options,
                          &result,
                          cancel_after_randomized,
                          NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_CANCELLED, result.last_error);
    TEST_ASSERT_TRUE(result.termination_attempted);
    TEST_ASSERT_FALSE(result.terminate_tx_succeeded);
    TEST_ASSERT_TRUE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL(DALI_ERR_CANCELLED, result.cleanup_error);
    TEST_ASSERT_EQUAL_UINT8(0u, s_bus.cleanup_count);
}

void test_cleanup_failure_preserves_primary_and_marks_initialisation_unknown(void)
{
    DaliDiscoveryTransport t = transport();
    mock_add_device(0u, 0x000010u);
    s_bus.cleanup_error = DALI_ERR_BUS_STUCK;

    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 1u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_CANCELLED,
                      dali_commissioning_commission_unaddressed(
                          &t,
                          &options,
                          &result,
                          cancel_after_randomized,
                          NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_CANCELLED, result.last_error);
    TEST_ASSERT_TRUE(result.termination_required);
    TEST_ASSERT_TRUE(result.termination_attempted);
    TEST_ASSERT_FALSE(result.terminate_tx_succeeded);
    TEST_ASSERT_TRUE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, result.cleanup_error);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.cleanup_count);
    TEST_ASSERT_EQUAL_UINT16(2u, count_frames_with_opcode(0xA1u));
    TEST_ASSERT_TRUE(s_bus.devices[0].active);
}

void test_cleanup_failure_is_primary_after_otherwise_successful_workflow(void)
{
    DaliDiscoveryTransport t = transport();
    s_bus.cleanup_error = DALI_ERR_BUS_STUCK;

    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 1u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK,
                      dali_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_TRUE(result.no_more_devices);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, result.last_error);
    TEST_ASSERT_TRUE(result.termination_attempted);
    TEST_ASSERT_FALSE(result.terminate_tx_succeeded);
    TEST_ASSERT_TRUE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, result.cleanup_error);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.cleanup_count);
}

void test_failed_start_closes_initialisation_state(void)
{
    DaliDiscoveryTransport t = transport();
    mock_add_device(0u, 0x000010u);

    /* RANDOMIZE fails after INITIALISE has already been accepted. */
    s_bus.fail_opcode = 0xA7u;
    s_bus.fail_err    = DALI_ERR_BUS_STUCK;

    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 0u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK,
                      dali_commissioning_commission_unaddressed(&t, &options,
                                                                &result, NULL, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, result.last_error);
    TEST_ASSERT_EQUAL_UINT8(0u, result.assigned_count);
    TEST_ASSERT_TRUE(result.termination_required);
    TEST_ASSERT_TRUE(result.termination_attempted);
    TEST_ASSERT_TRUE(result.terminate_tx_succeeded);
    TEST_ASSERT_FALSE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL(DALI_OK, result.cleanup_error);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.cleanup_count);

    /* Gear left in initialisation state stays there for fifteen minutes, so the
     * opening TERMINATE must be followed by a second one on the way out. */
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.initialise_count);
    TEST_ASSERT_EQUAL_UINT16(2u, count_frames_with_opcode(0xA1u));
}

void test_failed_opening_terminate_still_attempts_safety_terminate(void)
{
    DaliDiscoveryTransport t = transport();
    mock_add_device(0u, 0x000010u);

    /* A transport error cannot prove that no partial waveform or later admitted
     * sequence step reached the bus. A second TERMINATE is harmless and keeps
     * the failure path conservative. */
    s_bus.fail_opcode = 0xA1u;
    s_bus.fail_err    = DALI_ERR_BUS_STUCK;

    DaliCommissioningOptions options = {
        .first_short_address = 0u,
        .max_devices = 0u,
        .used_address_mask = 0u,
        .query_short_address = false,
    };
    DaliCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK,
                      dali_commissioning_commission_unaddressed(&t, &options,
                                                                &result, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, s_bus.initialise_count);
    TEST_ASSERT_EQUAL_UINT16(2u, count_frames_with_opcode(0xA1u));
    TEST_ASSERT_TRUE(result.termination_required);
    TEST_ASSERT_TRUE(result.termination_attempted);
    TEST_ASSERT_FALSE(result.terminate_tx_succeeded);
    TEST_ASSERT_TRUE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, result.cleanup_error);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.cleanup_count);
}

static void assert_search_steps(const DaliSequence *seq)
{
    /* random address 0x123456 splits into H=0x12, M=0x34, L=0x56 */
    TEST_ASSERT_EQUAL_HEX32(0xB112u,
        seq->steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRH].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xB334u,
        seq->steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRM].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xB556u,
        seq->steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRL].frame.data);

    for (uint8_t i = 0u; i < DALI_COMMISSIONING_SEARCH_SEQUENCE_STEPS; i++) {
        TEST_ASSERT_EQUAL_UINT8(DALI_FORWARD_FRAME_BITS,
                                seq->steps[i].frame.bit_length);
        TEST_ASSERT_FALSE(seq->steps[i].needs_reply);
        TEST_ASSERT_FALSE(seq->steps[i].send_twice);
        TEST_ASSERT_EQUAL_UINT8(0u, seq->steps[i].retries_left);
    }
}

void test_build_search_sequence_layout(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_build_search_sequence(0x123456u, &seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_SEARCH_SEQUENCE_STEPS,
                            seq.step_count);
    assert_search_steps(&seq);
}

void test_build_search_compare_sequence_layout(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_build_search_compare_sequence(0x123456u,
                                                                        &seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_SEARCH_COMPARE_SEQUENCE_STEPS,
                            seq.step_count);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(DALI_SEQUENCE_MAX_STEPS, seq.step_count);
    assert_search_steps(&seq);

    const DaliSequenceStep *compare =
        &seq.steps[DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE];
    TEST_ASSERT_EQUAL_HEX32(0xA900u, compare->frame.data);
    TEST_ASSERT_TRUE(compare->needs_reply);
    TEST_ASSERT_FALSE(compare->send_twice);
    /* COMPARE is idempotent, and a YES lost to noise would mislead the search. */
    TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_QUERY_RETRIES_LEFT,
                            compare->retries_left);
}

void test_build_program_verify_sequence_layout(void)
{
    DaliSequence seq;

    /* short address 18 encodes as (18 << 1) | 1 = 0x25 */
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_build_program_verify_sequence(18u, &seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_PROGRAM_VERIFY_SEQUENCE_STEPS,
                            seq.step_count);

    const DaliSequenceStep *program =
        &seq.steps[DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_PROGRAM];
    TEST_ASSERT_EQUAL_HEX32(0xB725u, program->frame.data);
    TEST_ASSERT_FALSE(program->needs_reply);
    TEST_ASSERT_FALSE(program->send_twice);
    TEST_ASSERT_EQUAL_UINT8(0u, program->retries_left);

    const DaliSequenceStep *verify =
        &seq.steps[DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY];
    TEST_ASSERT_EQUAL_HEX32(0xB925u, verify->frame.data);
    TEST_ASSERT_TRUE(verify->needs_reply);
    TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_QUERY_RETRIES_LEFT,
                            verify->retries_left);

    /* Both steps must address the same device. */
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(program->frame.data & 0xFFu),
                           (uint8_t)(verify->frame.data & 0xFFu));
}

void test_sequence_builders_reject_bad_arguments(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_build_search_sequence(0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_build_search_sequence(
                          DALI_RANDOM_ADDRESS_MAX + 1u, &seq));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_build_search_sequence(
                          DALI_RANDOM_ADDRESS_MAX, &seq));

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_build_search_compare_sequence(0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_build_search_compare_sequence(
                          DALI_RANDOM_ADDRESS_MAX + 1u, &seq));

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_build_program_verify_sequence(0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_build_program_verify_sequence(
                          DALI_SHORT_ADDRESS_COUNT, &seq));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_build_program_verify_sequence(
                          DALI_SHORT_ADDRESS_COUNT - 1u, &seq));
}

/* Build a completed result by hand so the readers are tested without a bus. */
static void result_reset(DaliSequenceResult *result, uint8_t steps_run)
{
    memset(result, 0, sizeof(*result));
    result->result      = DALI_OK;
    result->failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
    result->steps_run   = steps_run;
}

static void seed_reply(DaliSequenceResult *result,
                       uint8_t step,
                       uint8_t value,
                       uint8_t bit_length)
{
    result->replies[step] = (DaliFrame){ .data = value, .bit_length = bit_length };
    result->reply_mask |= (uint8_t)(1u << step);
}

static void result_fail(DaliSequenceResult *result, DaliError err, uint8_t step)
{
    result->result      = err;
    result->failed_step = step;
    result->steps_run   = (uint8_t)(step + 1u);
}

void test_compare_from_sequence_reads_yes_and_no(void)
{
    DaliSequenceResult result;
    bool yes = false;

    result_reset(&result, DALI_COMMISSIONING_SEARCH_COMPARE_SEQUENCE_STEPS);
    seed_reply(&result, DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE,
               DALI_YES_RESPONSE, DALI_BACKWARD_FRAME_BITS);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_compare_from_sequence(&result, &yes));
    TEST_ASSERT_TRUE(yes);

    /* NO is silence. A decoded non-YES byte is invalid/ambiguous traffic. */
    result_reset(&result, DALI_COMMISSIONING_SEARCH_COMPARE_SEQUENCE_STEPS);
    seed_reply(&result, DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE,
               0x00u, DALI_BACKWARD_FRAME_BITS);
    yes = true;
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_commissioning_compare_from_sequence(&result, &yes));
    TEST_ASSERT_TRUE(yes);

    /* A silent reply window on the COMPARE step is the standard NO. */
    result_reset(&result, 0u);
    result_fail(&result, DALI_ERR_TIMEOUT,
                DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE);
    yes = true;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_compare_from_sequence(&result, &yes));
    TEST_ASSERT_FALSE(yes);
}

void test_rx_activity_means_yes_only_for_compare(void)
{
    DaliSequenceResult result;
    bool answer = false;

    result_reset(&result, 0u);
    result_fail(&result,
                DALI_ERR_RX_ACTIVITY,
                DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_compare_from_sequence(&result,
                                                                &answer));
    TEST_ASSERT_TRUE(answer);

    result_reset(&result, 0u);
    result_fail(&result,
                DALI_ERR_RX_ACTIVITY,
                DALI_COMMISSIONING_SEARCH_STEP_ADDRM);
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY,
                      dali_commissioning_compare_from_sequence(&result,
                                                                &answer));

    result_reset(&result, 0u);
    result_fail(&result,
                DALI_ERR_RX_ACTIVITY,
                DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY);
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY,
                      dali_commissioning_verify_from_sequence(&result,
                                                               &answer));

    DaliDiscoveryTransport activity_transport = {
        .transact = mock_rx_activity_transact,
    };
    answer = false;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_compare(&activity_transport, &answer));
    TEST_ASSERT_TRUE(answer);

    answer = false;
    TEST_ASSERT_EQUAL(DALI_ERR_RX_ACTIVITY,
                      dali_commissioning_verify_short_address(
                          &activity_transport,
                          7u,
                          &answer));
}

void test_decoded_non_yes_reply_is_never_commissioning_no(void)
{
    DaliDiscoveryTransport non_yes_transport = {
        .transact = mock_non_yes_transact,
    };
    bool answer = true;

    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_commissioning_compare(&non_yes_transport, &answer));
    TEST_ASSERT_TRUE(answer);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_commissioning_verify_short_address(
                          &non_yes_transport, 7u, &answer));
}

void test_compare_from_sequence_does_not_mistake_a_failed_write_for_no(void)
{
    DaliSequenceResult result;
    bool yes = true;

    /* A search-address write that never made it out must surface as an error;
     * reporting NO would send the binary search past a real device. */
    result_reset(&result, 0u);
    result_fail(&result, DALI_ERR_TIMEOUT, DALI_COMMISSIONING_SEARCH_STEP_ADDRM);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT,
                      dali_commissioning_compare_from_sequence(&result, &yes));
    TEST_ASSERT_TRUE(yes);

    /* A non-timeout failure on the COMPARE step is also a real error. */
    result_reset(&result, 0u);
    result_fail(&result, DALI_ERR_BUS_STUCK,
                DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK,
                      dali_commissioning_compare_from_sequence(&result, &yes));

    /* So is a nominally successful sequence with no COMPARE reply. */
    result_reset(&result, DALI_COMMISSIONING_SEARCH_COMPARE_SEQUENCE_STEPS);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_commissioning_compare_from_sequence(&result, &yes));

    /* And a reply that came back the wrong width. */
    result_reset(&result, DALI_COMMISSIONING_SEARCH_COMPARE_SEQUENCE_STEPS);
    seed_reply(&result, DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE,
               DALI_YES_RESPONSE, DALI_FORWARD_FRAME_BITS);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_commissioning_compare_from_sequence(&result, &yes));

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_compare_from_sequence(&result, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_commissioning_compare_from_sequence(NULL, &yes));
}

void test_verify_from_sequence_reads_confirmation_and_failures(void)
{
    DaliSequenceResult result;
    bool verified = false;

    result_reset(&result, DALI_COMMISSIONING_PROGRAM_VERIFY_SEQUENCE_STEPS);
    seed_reply(&result, DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY,
               DALI_YES_RESPONSE, DALI_BACKWARD_FRAME_BITS);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_verify_from_sequence(&result, &verified));
    TEST_ASSERT_TRUE(verified);

    /* A silent VERIFY means the device did not confirm — not a bus failure. */
    result_reset(&result, 0u);
    result_fail(&result, DALI_ERR_TIMEOUT,
                DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY);
    verified = true;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_verify_from_sequence(&result, &verified));
    TEST_ASSERT_FALSE(verified);

    result_reset(&result, DALI_COMMISSIONING_PROGRAM_VERIFY_SEQUENCE_STEPS);
    seed_reply(&result, DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY,
               0x00u, DALI_BACKWARD_FRAME_BITS);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_commissioning_verify_from_sequence(&result, &verified));

    /* A PROGRAM that never went out must not be reported as "not confirmed",
     * which would look like a device problem rather than a transport one. */
    result_reset(&result, 0u);
    result_fail(&result, DALI_ERR_TIMEOUT,
                DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_PROGRAM);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT,
                      dali_commissioning_verify_from_sequence(&result, &verified));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_start_sequence_layout);
    RUN_TEST(test_failed_start_closes_initialisation_state);
    RUN_TEST(test_failed_opening_terminate_still_attempts_safety_terminate);
    RUN_TEST(test_cancellation_after_start_uses_safety_terminate);
    RUN_TEST(test_opening_sequence_unknown_progress_still_uses_safety_terminate);
    RUN_TEST(test_cancelled_fallback_cleanup_reports_unconfirmed_initialisation_state);
    RUN_TEST(test_cleanup_failure_preserves_primary_and_marks_initialisation_unknown);
    RUN_TEST(test_cleanup_failure_is_primary_after_otherwise_successful_workflow);
    RUN_TEST(test_build_search_sequence_layout);
    RUN_TEST(test_build_search_compare_sequence_layout);
    RUN_TEST(test_build_program_verify_sequence_layout);
    RUN_TEST(test_sequence_builders_reject_bad_arguments);
    RUN_TEST(test_compare_from_sequence_reads_yes_and_no);
    RUN_TEST(test_rx_activity_means_yes_only_for_compare);
    RUN_TEST(test_decoded_non_yes_reply_is_never_commissioning_no);
    RUN_TEST(test_compare_from_sequence_does_not_mistake_a_failed_write_for_no);
    RUN_TEST(test_verify_from_sequence_reads_confirmation_and_failures);
    RUN_TEST(test_short_address_encoding_round_trip);
    RUN_TEST(test_set_search_address_sends_h_m_l_special_commands);
    RUN_TEST(test_set_search_address_rejects_frame_only_transport_without_traffic);
    RUN_TEST(test_compare_timeout_is_no);
    RUN_TEST(test_find_next_random_address_binary_searches_lowest_active_device);
    RUN_TEST(test_commission_unaddressed_assigns_free_short_addresses_in_order);
    RUN_TEST(test_commission_unaddressed_respects_max_devices);
    RUN_TEST(test_commission_unaddressed_returns_without_tx_when_address_space_full);
    RUN_TEST(test_commission_unaddressed_rejects_frame_only_transport_without_traffic);
    RUN_TEST(test_inventory_used_mask_marks_control_gear_only);
    RUN_TEST(test_inventory_used_mask_reserves_undecodable_addresses);
    RUN_TEST(test_commission_skips_undecodable_addresses);
    RUN_TEST(test_opening_settles_after_randomize_before_first_compare);
    RUN_TEST(test_commission_rejects_transport_without_settle_wait_and_sends_nothing);
    RUN_TEST(test_invalid_arguments_are_rejected);
    return UNITY_END();
}
