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

static DaliDiscoveryTransport transport(void)
{
    return (DaliDiscoveryTransport){
        .transact = mock_transact,
        .ctx = NULL,
    };
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
    TEST_ASSERT_EQUAL_UINT16(0u, s_bus.log_count);
}

void test_inventory_used_mask_marks_present_devices(void)
{
    DaliDiscoveryInventory inventory;

    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_inventory_reset(&inventory));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_inventory_store_status(&inventory,
                                                           4u,
                                                           0x00u));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_inventory_store_status(&inventory,
                                                           63u,
                                                           0x00u));

    uint64_t mask = dali_commissioning_used_mask_from_inventory(&inventory);
    TEST_ASSERT_NOT_EQUAL(0u, (mask & ((uint64_t)1u << 4u)));
    TEST_ASSERT_NOT_EQUAL(0u, (mask & ((uint64_t)1u << 63u)));
    TEST_ASSERT_EQUAL(0u, (mask & ((uint64_t)1u << 5u)));
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

    /* Any other value is not YES. */
    result_reset(&result, DALI_COMMISSIONING_SEARCH_COMPARE_SEQUENCE_STEPS);
    seed_reply(&result, DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE,
               0x00u, DALI_BACKWARD_FRAME_BITS);
    yes = true;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_compare_from_sequence(&result, &yes));
    TEST_ASSERT_FALSE(yes);

    /* A silent reply window on the COMPARE step is the standard NO. */
    result_reset(&result, 0u);
    result_fail(&result, DALI_ERR_TIMEOUT,
                DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE);
    yes = true;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_commissioning_compare_from_sequence(&result, &yes));
    TEST_ASSERT_FALSE(yes);
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
    RUN_TEST(test_build_search_sequence_layout);
    RUN_TEST(test_build_search_compare_sequence_layout);
    RUN_TEST(test_build_program_verify_sequence_layout);
    RUN_TEST(test_sequence_builders_reject_bad_arguments);
    RUN_TEST(test_compare_from_sequence_reads_yes_and_no);
    RUN_TEST(test_compare_from_sequence_does_not_mistake_a_failed_write_for_no);
    RUN_TEST(test_verify_from_sequence_reads_confirmation_and_failures);
    RUN_TEST(test_short_address_encoding_round_trip);
    RUN_TEST(test_set_search_address_sends_h_m_l_special_commands);
    RUN_TEST(test_compare_timeout_is_no);
    RUN_TEST(test_find_next_random_address_binary_searches_lowest_active_device);
    RUN_TEST(test_commission_unaddressed_assigns_free_short_addresses_in_order);
    RUN_TEST(test_commission_unaddressed_respects_max_devices);
    RUN_TEST(test_commission_unaddressed_returns_without_tx_when_address_space_full);
    RUN_TEST(test_inventory_used_mask_marks_present_devices);
    RUN_TEST(test_invalid_arguments_are_rejected);
    return UNITY_END();
}
