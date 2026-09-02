#include "unity.h"
#include "dali_device_commissioning.h"

#include <string.h>

#define MOCK_DEVICE_COUNT 4u
#define MOCK_LOG_MAX      520u
#define MOCK_UNADDRESSED  0xFFu

/*
 * A mock Part 103 bus.
 *
 * Every assertion inside it is deliberate: the mock is where the encoding traps
 * get caught. It rejects a Part 102-encoded PROGRAM SHORT ADDRESS, rejects the
 * Part 102 INITIALISE sentinel, and fails loudly on any 16-bit frame that is not
 * the cross-part TERMINATE — because a device walk emitting gear frames is
 * exactly the bug this module exists to avoid.
 */
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
} MockTxLog;

typedef struct {
    MockDevice devices[MOCK_DEVICE_COUNT];
    MockTxLog  log[MOCK_LOG_MAX];
    uint16_t   log_count;
    uint32_t   search_address;
    uint8_t    initialise_count;
    uint8_t    randomise_count;
    uint8_t    withdraw_count;
    uint8_t    deaddress_count;
    uint8_t    terminate_count;
    /* Part 102 TERMINATE — the cross-part guard, and the only 16-bit frame this
     * bus expects to see. */
    uint8_t    gear_terminate_count;
    bool       gear_terminate_fails;
    uint8_t    cleanup_count;
    DaliError  cleanup_error;
    uint8_t    fail_opcode;      /* 0xFF = none */
    DaliError  fail_err;
    bool       withdraw_is_noop;
    uint8_t    delay_count;
    uint32_t   delay_total_ms;
    bool       cancelled;
} MockDeviceBus;

static MockDeviceBus s_bus;

static void mock_add_device(uint8_t slot, uint32_t random_address)
{
    TEST_ASSERT_LESS_THAN_UINT8(MOCK_DEVICE_COUNT, slot);
    s_bus.devices[slot] = (MockDevice){
        .present        = true,
        .active         = false,
        .random_address = random_address,
        .short_address  = MOCK_UNADDRESSED,
    };
}

static void mock_log_tx(const DaliFrame *frame, bool needs_reply, bool send_twice)
{
    TEST_ASSERT_LESS_THAN_UINT16(MOCK_LOG_MAX, s_bus.log_count);
    s_bus.log[s_bus.log_count++] = (MockTxLog){
        .data        = frame->data,
        .bit_length  = frame->bit_length,
        .needs_reply = needs_reply,
        .send_twice  = send_twice,
    };
}

static bool mock_selected(const MockDevice *device)
{
    return device != NULL && device->present && device->active &&
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
    if (s_bus.fail_opcode != 0xFFu && opcode == s_bus.fail_opcode) {
        return s_bus.fail_err;
    }

    switch (opcode) {
        case 0x00u:  /* TERMINATE */
            for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                s_bus.devices[i].active = false;
            }
            s_bus.terminate_count++;
            return DALI_OK;

        case 0x01u:  /* INITIALISE */
            /* 0x00 selects unaddressed devices. The Part 102 sentinel 0xFF
             * would mean "every control device" here, so a walk that used it
             * would open a window over the whole bus. */
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(
                DALI_DEVICE_INITIALISE_UNADDRESSED_PARAM, param,
                "Part 103 INITIALISE is inverted against Part 102");
            s_bus.initialise_count++;
            for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                s_bus.devices[i].active =
                    s_bus.devices[i].present &&
                    s_bus.devices[i].short_address == MOCK_UNADDRESSED;
            }
            return DALI_OK;

        case 0x02u:  /* RANDOMISE */
            s_bus.randomise_count++;
            return DALI_OK;

        case 0x04u:  /* WITHDRAW */
            for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                if (mock_selected(&s_bus.devices[i])) {
                    if (!s_bus.withdraw_is_noop) {
                        s_bus.devices[i].active = false;
                    }
                    s_bus.withdraw_count++;
                }
            }
            return DALI_OK;

        case 0x05u:
            s_bus.search_address =
                (s_bus.search_address & 0x00FFFFu) | ((uint32_t)param << 16u);
            return DALI_OK;
        case 0x06u:
            s_bus.search_address =
                (s_bus.search_address & 0xFF00FFu) | ((uint32_t)param << 8u);
            return DALI_OK;
        case 0x07u:
            s_bus.search_address = (s_bus.search_address & 0xFFFF00u) | param;
            return DALI_OK;

        case 0x08u:  /* PROGRAM SHORT ADDRESS — raw 6-bit, not (a << 1) | 1 */
            if (param == DALI_DEVICE_NO_SHORT_ADDRESS) {
                s_bus.deaddress_count++;
                for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                    if (mock_selected(&s_bus.devices[i])) {
                        s_bus.devices[i].short_address = MOCK_UNADDRESSED;
                    }
                }
                return DALI_OK;
            }
            TEST_ASSERT_LESS_THAN_UINT8_MESSAGE(
                DALI_SHORT_ADDRESS_COUNT, param,
                "Part 103 PROGRAM SHORT ADDRESS takes the raw 6-bit address");
            for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                if (mock_selected(&s_bus.devices[i])) {
                    s_bus.devices[i].short_address = param;
                }
            }
            return DALI_OK;

        default:
            TEST_FAIL_MESSAGE("unexpected Part 103 special, no-reply");
            return DALI_ERR_INVALID;
    }
}

static DaliError mock_special_reply(uint8_t opcode, uint8_t param, DaliFrame *reply_out)
{
    TEST_ASSERT_NOT_NULL(reply_out);

    switch (opcode) {
        case 0x03u:  /* COMPARE */
            if (!mock_any_selected()) {
                return DALI_ERR_TIMEOUT;
            }
            *reply_out = (DaliFrame){ .data = DALI_YES_RESPONSE,
                                      .bit_length = DALI_BACKWARD_FRAME_BITS };
            return DALI_OK;

        case 0x09u: {  /* VERIFY SHORT ADDRESS — raw address as the parameter */
            TEST_ASSERT_LESS_THAN_UINT8_MESSAGE(
                DALI_SHORT_ADDRESS_COUNT, param,
                "Part 103 VERIFY SHORT ADDRESS takes the raw 6-bit address");
            uint8_t responders = 0u;
            for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
                if (mock_selected(&s_bus.devices[i]) &&
                    s_bus.devices[i].short_address == param) {
                    responders++;
                }
            }
            if (responders == 0u) {
                return DALI_ERR_TIMEOUT;
            }
            /* Two devices answering one reply window overlap and do not decode:
             * what an equal random address looks like from the master's side. */
            if (responders > 1u) {
                return DALI_ERR_RX_ACTIVITY;
            }
            *reply_out = (DaliFrame){ .data = DALI_YES_RESPONSE,
                                      .bit_length = DALI_BACKWARD_FRAME_BITS };
            return DALI_OK;
        }

        case 0x0Au: {  /* QUERY SHORT ADDRESS — answers raw, 0xFF for none */
            MockDevice *device = mock_first_selected();
            if (device == NULL) {
                return DALI_ERR_TIMEOUT;
            }
            *reply_out = (DaliFrame){ .data = device->short_address,
                                      .bit_length = DALI_BACKWARD_FRAME_BITS };
            return DALI_OK;
        }

        default:
            TEST_FAIL_MESSAGE("unexpected Part 103 special, reply");
            return DALI_ERR_INVALID;
    }
}

/* The Part 102 TERMINATE the device walk sends as its cross-part guard. Any
 * other 16-bit frame means the walk built a gear command by mistake. */
static DaliError mock_gear_frame(const DaliFrame *frame, bool needs_reply, bool send_twice)
{
    TEST_ASSERT_FALSE(needs_reply);
    TEST_ASSERT_FALSE(send_twice);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(
        0xA100u, frame->data,
        "the only 16-bit frame a device walk may send is Part 102 TERMINATE");
    s_bus.gear_terminate_count++;
    return s_bus.gear_terminate_fails ? DALI_ERR_BUS_STUCK : DALI_OK;
}

static DaliError mock_transact(const DaliFrame *frame,
                               bool             needs_reply,
                               uint8_t          retries_left,
                               bool             send_twice,
                               DaliFrame       *reply_out,
                               void            *ctx)
{
    (void)ctx;
    if (s_bus.cancelled) {
        return DALI_ERR_CANCELLED;
    }
    TEST_ASSERT_NOT_NULL(frame);
    mock_log_tx(frame, needs_reply, send_twice);

    /* A frame expecting no answer has nothing to retry for, and retrying a
     * search-address write or a PROGRAM would re-send device state. */
    if (!needs_reply) {
        TEST_ASSERT_EQUAL_UINT8(0u, retries_left);
    }

    if (frame->bit_length == DALI_FORWARD_FRAME_BITS) {
        return mock_gear_frame(frame, needs_reply, send_twice);
    }

    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, frame->bit_length);
    const uint8_t addr_byte = (uint8_t)((frame->data >> 16u) & 0xFFu);
    const uint8_t opcode    = (uint8_t)((frame->data >> 8u) & 0xFFu);
    const uint8_t param     = (uint8_t)(frame->data & 0xFFu);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xC1u, addr_byte,
                                   "Part 103 specials carry 0xC1 as byte 2");

    if (needs_reply) {
        TEST_ASSERT_FALSE(send_twice);
        return mock_special_reply(opcode, param, reply_out);
    }
    return mock_special_no_reply(opcode, param);
}

static DaliError mock_cleanup_transact(const DaliFrame *frame,
                                       bool             needs_reply,
                                       uint8_t          retries_left,
                                       bool             send_twice,
                                       DaliFrame       *reply_out,
                                       void            *ctx)
{
    (void)ctx;
    (void)retries_left;
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_FALSE(needs_reply);
    TEST_ASSERT_NULL(reply_out);
    mock_log_tx(frame, false, send_twice);

    if (frame->bit_length == DALI_FORWARD_FRAME_BITS) {
        return mock_gear_frame(frame, needs_reply, send_twice);
    }

    s_bus.cleanup_count++;
    if (s_bus.cleanup_error != DALI_OK) {
        return s_bus.cleanup_error;
    }
    return mock_special_no_reply((uint8_t)((frame->data >> 8u) & 0xFFu),
                                 (uint8_t)(frame->data & 0xFFu));
}

static DaliError mock_transact_sequence(const DaliSequence *seq,
                                        DaliSequenceResult *result_out,
                                        void               *ctx)
{
    if (seq == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliSequenceResult result = {
        .result      = DALI_OK,
        .failed_step = DALI_SEQUENCE_NO_FAILED_STEP,
    };

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
}

static DaliTransport transport(void)
{
    return (DaliTransport){
        .transact          = mock_transact,
        .transact_sequence = mock_transact_sequence,
        .ctx               = NULL,
        .transact_cleanup  = mock_cleanup_transact,
        .delay_ms          = mock_delay_ms,
    };
}

void setUp(void)
{
    memset(&s_bus, 0, sizeof(s_bus));
    s_bus.fail_opcode = 0xFFu;
    for (uint8_t i = 0u; i < MOCK_DEVICE_COUNT; i++) {
        s_bus.devices[i].short_address = MOCK_UNADDRESSED;
    }
}

void tearDown(void) {}

static DaliDeviceCommissioningOptions default_options(void)
{
    DaliDeviceCommissioningOptions options;
    memset(&options, 0, sizeof(options));
    return options;
}

/* --------------------------------------------------------------------------
 * Sequence layout and the encoding traps
 * -------------------------------------------------------------------------*/

void test_start_sequence_uses_the_inverted_initialise_parameter(void)
{
    DaliSequence seq;
    TEST_ASSERT_EQUAL(DALI_OK, dali_device_commissioning_build_start_sequence(&seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_START_SEQUENCE_STEPS, seq.step_count);

    /* TERMINATE, INITIALISE(0x00), RANDOMISE — all 0xC1-prefixed. */
    TEST_ASSERT_EQUAL_HEX32(0xC10000u,
                            seq.steps[DALI_COMMISSIONING_START_STEP_TERMINATE].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC10100u,
                            seq.steps[DALI_COMMISSIONING_START_STEP_INITIALISE].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC10200u,
                            seq.steps[DALI_COMMISSIONING_START_STEP_RANDOMISE].frame.data);

    /* The trap: the Part 102 sentinel is 0xFF and means "unaddressed" there.
     * Here 0xFF would mean every control device. */
    const uint8_t param =
        (uint8_t)(seq.steps[DALI_COMMISSIONING_START_STEP_INITIALISE].frame.data & 0xFFu);
    TEST_ASSERT_EQUAL_HEX8(0x00u, param);
    TEST_ASSERT_NOT_EQUAL_UINT8(DALI_INITIALISE_UNADDRESSED_PARAM, param);

    TEST_ASSERT_TRUE(seq.steps[DALI_COMMISSIONING_START_STEP_INITIALISE].send_twice);
    TEST_ASSERT_TRUE(seq.steps[DALI_COMMISSIONING_START_STEP_RANDOMISE].send_twice);
    TEST_ASSERT_FALSE(seq.steps[DALI_COMMISSIONING_START_STEP_TERMINATE].send_twice);
}

void test_program_verify_sequence_carries_the_raw_short_address(void)
{
    DaliSequence seq;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_build_program_verify_sequence(5u, &seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_PROGRAM_VERIFY_SEQUENCE_STEPS,
                            seq.step_count);

    /* 5, not (5 << 1) | 1 == 11. The Part 102 encoding here would program
     * address 11 and report nothing wrong. */
    TEST_ASSERT_EQUAL_HEX32(0xC10805u,
                            seq.steps[DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_PROGRAM].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC10905u,
                            seq.steps[DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY].frame.data);
    TEST_ASSERT_NOT_EQUAL_UINT8(
        dali_commissioning_encode_short_address(5u),
        (uint8_t)(seq.steps[DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_PROGRAM].frame.data & 0xFFu));

    TEST_ASSERT_TRUE(seq.steps[DALI_COMMISSIONING_PROGRAM_VERIFY_STEP_VERIFY].needs_reply);
}

void test_search_compare_sequence_layout_matches_the_shared_classifier(void)
{
    DaliSequence seq;
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_build_search_compare_sequence(
                          0x123456u, &seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_COMMISSIONING_SEARCH_COMPARE_SEQUENCE_STEPS,
                            seq.step_count);
    TEST_ASSERT_EQUAL_HEX32(0xC10512u, seq.steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRH].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC10634u, seq.steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRM].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC10756u, seq.steps[DALI_COMMISSIONING_SEARCH_STEP_ADDRL].frame.data);
    TEST_ASSERT_EQUAL_HEX32(0xC10300u,
                            seq.steps[DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE].frame.data);
    /* COMPARE sits at the index the shared classifier reads, which is what lets
     * the three-outcome logic be shared rather than reimplemented. */
    TEST_ASSERT_TRUE(seq.steps[DALI_COMMISSIONING_SEARCH_COMPARE_STEP_COMPARE].needs_reply);
}

void test_sequence_builders_reject_bad_arguments(void)
{
    DaliSequence seq;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_device_commissioning_build_start_sequence(NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_device_commissioning_build_search_compare_sequence(
                          DALI_RANDOM_ADDRESS_MAX + 1u, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_device_commissioning_build_program_verify_sequence(64u, &seq));
    /* 0xFF is the de-address sentinel and must stay buildable. */
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_build_program_verify_sequence(
                          DALI_DEVICE_NO_SHORT_ADDRESS, &seq));
}

/* --------------------------------------------------------------------------
 * The walk
 * -------------------------------------------------------------------------*/

void test_commission_assigns_free_addresses_in_order(void)
{
    mock_add_device(0u, 0x0000F0u);
    mock_add_device(1u, 0x00A000u);
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));

    TEST_ASSERT_EQUAL_UINT8(2u, result.assigned_count);
    TEST_ASSERT_EQUAL_UINT8(0u, result.assignments[0].short_address);
    TEST_ASSERT_EQUAL_UINT8(1u, result.assignments[1].short_address);
    TEST_ASSERT_EQUAL_UINT32(0x0000F0u, result.assignments[0].random_address);
    TEST_ASSERT_TRUE(result.no_more_devices);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.initialise_count);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.randomise_count);
    /* Every device ends with the address the run reported. */
    TEST_ASSERT_EQUAL_UINT8(0u, s_bus.devices[0].short_address);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.devices[1].short_address);
}

void test_commission_starts_from_the_requested_address(void)
{
    mock_add_device(0u, 0x000010u);
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    options.first_short_address = 7u;
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(1u, result.assigned_count);
    TEST_ASSERT_EQUAL_UINT8(7u, result.assignments[0].short_address);
}

void test_commission_skips_addresses_the_used_mask_reserves(void)
{
    mock_add_device(0u, 0x000010u);
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    options.used_address_mask = 0x7u;   /* 0, 1, 2 taken */
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(3u, result.assignments[0].short_address);
}

void test_commission_settles_after_randomise_before_the_first_compare(void)
{
    mock_add_device(0u, 0x000010u);
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.delay_count);
    TEST_ASSERT_EQUAL_UINT32(DALI_COMMISSIONING_RANDOMISE_SETTLE_MS,
                             s_bus.delay_total_ms);
}

void test_commission_refuses_a_transport_that_cannot_wait_and_sends_nothing(void)
{
    mock_add_device(0u, 0x000010u);
    DaliTransport t = transport();
    t.delay_ms = NULL;
    DaliDeviceCommissioningOptions options = default_options();
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, s_bus.initialise_count);
    TEST_ASSERT_EQUAL_UINT16(0u, s_bus.log_count);
}

void test_commission_reports_a_full_address_space_without_traffic(void)
{
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    options.used_address_mask = ~(uint64_t)0u;
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_TRUE(result.address_space_full);
    TEST_ASSERT_EQUAL_UINT8(0u, result.assigned_count);
    TEST_ASSERT_EQUAL_UINT16(0u, s_bus.log_count);
}

void test_commission_terminates_on_the_way_out(void)
{
    mock_add_device(0u, 0x000010u);
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_TRUE(result.termination_attempted);
    TEST_ASSERT_TRUE(result.terminate_tx_succeeded);
    TEST_ASSERT_FALSE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.cleanup_count);
}

void test_a_failed_terminate_leaves_the_addressing_state_unknown(void)
{
    mock_add_device(0u, 0x000010u);
    DaliTransport t = transport();
    s_bus.cleanup_error = DALI_ERR_BUS_STUCK;
    DaliDeviceCommissioningOptions options = default_options();
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_TRUE(result.initialisation_state_unknown);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, result.cleanup_error);
}

/* --------------------------------------------------------------------------
 * Cross-part guard
 * -------------------------------------------------------------------------*/

void test_without_the_option_no_gear_frame_is_sent(void)
{
    mock_add_device(0u, 0x000010u);
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, s_bus.gear_terminate_count);
    TEST_ASSERT_FALSE(result.cross_part_terminate_requested);
}

void test_the_cross_part_guard_brackets_the_run_with_part_102_terminate(void)
{
    mock_add_device(0u, 0x000010u);
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    options.terminate_control_gear = true;
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    /* Before INITIALISE, again after the opening, once more in the unwind. */
    TEST_ASSERT_EQUAL_UINT8(3u, s_bus.gear_terminate_count);
    TEST_ASSERT_TRUE(result.cross_part_terminate_requested);
    TEST_ASSERT_TRUE(result.cross_part_terminate_attempted);
    TEST_ASSERT_EQUAL(DALI_OK, result.cross_part_error);
}

void test_a_failed_cross_part_terminate_does_not_abort_the_run(void)
{
    mock_add_device(0u, 0x000010u);
    DaliTransport t = transport();
    s_bus.gear_terminate_fails = true;
    DaliDeviceCommissioningOptions options = default_options();
    options.terminate_control_gear = true;
    DaliDeviceCommissioningResult result;

    /* Hardening, not a precondition: the addressing still happened. */
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT8(1u, result.assigned_count);
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK, result.cross_part_error);
}

/* --------------------------------------------------------------------------
 * Equal random addresses
 * -------------------------------------------------------------------------*/

void test_two_devices_sharing_a_random_address_are_recovered_and_the_run_continues(void)
{
    mock_add_device(0u, 0x000010u);
    mock_add_device(1u, 0x000010u);   /* the twins */
    mock_add_device(2u, 0x00B000u);
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));

    TEST_ASSERT_EQUAL_UINT8(1u, result.duplicate_count);
    TEST_ASSERT_EQUAL_UINT32(0x000010u, result.duplicate_random_addresses[0]);
    TEST_ASSERT_FALSE(result.duplicate_recovery_failed);
    TEST_ASSERT_EQUAL_UINT8(1u, s_bus.deaddress_count);

    /* The third device still gets addressed, and takes the address the pair
     * gave back rather than the one after it. */
    TEST_ASSERT_EQUAL_UINT8(1u, result.assigned_count);
    TEST_ASSERT_EQUAL_UINT8(0u, result.assignments[0].short_address);
    TEST_ASSERT_EQUAL_UINT8(MOCK_UNADDRESSED, s_bus.devices[0].short_address);
    TEST_ASSERT_EQUAL_UINT8(MOCK_UNADDRESSED, s_bus.devices[1].short_address);
    TEST_ASSERT_EQUAL_UINT8(0u, s_bus.devices[2].short_address);
}

void test_a_pair_that_will_not_withdraw_stops_the_run_rather_than_looping(void)
{
    mock_add_device(0u, 0x000010u);
    mock_add_device(1u, 0x000010u);
    s_bus.withdraw_is_noop = true;
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_TRUE(result.duplicate_recovery_failed);
}

/* --------------------------------------------------------------------------
 * Query short address
 * -------------------------------------------------------------------------*/

void test_query_short_address_reads_the_raw_value(void)
{
    mock_add_device(0u, 0x000010u);
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    options.query_short_address = true;
    DaliDeviceCommissioningResult result;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));
    TEST_ASSERT_TRUE(result.assignments[0].has_query_short);
    /* Raw in this space, so the raw and decoded fields agree rather than
     * differing by the Part 102 encoding. */
    TEST_ASSERT_EQUAL_UINT8(0u, result.assignments[0].query_short_raw);
    TEST_ASSERT_EQUAL_UINT8(0u, result.assignments[0].query_short_address);
}

/* --------------------------------------------------------------------------
 * used_mask
 * -------------------------------------------------------------------------*/

void test_used_mask_counts_control_devices_only(void)
{
    DaliDiscoveryInventory inv;
    memset(&inv, 0, sizeof(inv));
    inv.valid = true;

    inv.devices[3].present          = true;
    inv.devices[3].has_input_device = true;   /* a device: reserves */
    inv.devices[9].present          = true;
    inv.devices[9].has_control_gear = true;   /* gear only: reserves nothing */

    const uint64_t mask =
        dali_device_commissioning_used_mask_from_inventory(&inv);
    TEST_ASSERT_EQUAL_UINT64(((uint64_t)1u << 3), mask);
}

void test_used_mask_reserves_undecodable_device_activity(void)
{
    DaliDiscoveryInventory inv;
    memset(&inv, 0, sizeof(inv));
    inv.valid = true;
    /* Not `present`, but something answered: the address is taken even though
     * nothing could be read from it. */
    inv.devices[5].has_undecodable_device_activity = true;

    const uint64_t mask =
        dali_device_commissioning_used_mask_from_inventory(&inv);
    TEST_ASSERT_EQUAL_UINT64(((uint64_t)1u << 5), mask);
    TEST_ASSERT_EQUAL_UINT64(0u,
                             dali_device_commissioning_used_mask_from_inventory(NULL));
}

void test_used_mask_is_independent_of_the_gear_mask(void)
{
    /* The same numeric address occupied in both spaces by different units. Each
     * mask must see only its own. */
    DaliDiscoveryInventory inv;
    memset(&inv, 0, sizeof(inv));
    inv.valid = true;
    inv.devices[0].present          = true;
    inv.devices[0].has_control_gear = true;
    inv.devices[4].present          = true;
    inv.devices[4].has_input_device = true;

    TEST_ASSERT_EQUAL_UINT64(((uint64_t)1u << 0),
                             dali_commissioning_used_mask_from_inventory(&inv));
    TEST_ASSERT_EQUAL_UINT64(((uint64_t)1u << 4),
                             dali_device_commissioning_used_mask_from_inventory(&inv));
}

/* --------------------------------------------------------------------------
 * Argument checking
 * -------------------------------------------------------------------------*/

void test_invalid_arguments_are_rejected(void)
{
    DaliTransport t = transport();
    DaliDeviceCommissioningOptions options = default_options();
    DaliDeviceCommissioningResult result;
    uint32_t random_address = 0u;
    bool found = false;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_device_commissioning_commission_unaddressed(
                          NULL, &options, &result, NULL, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_device_commissioning_commission_unaddressed(
                          &t, NULL, &result, NULL, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, NULL, NULL, NULL));

    options.first_short_address = DALI_SHORT_ADDRESS_COUNT;
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_device_commissioning_commission_unaddressed(
                          &t, &options, &result, NULL, NULL));

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_device_commissioning_find_next_random_address(
                          NULL, &random_address, &found));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_device_commissioning_program_short_address(&t, 64u));
    /* 0xFF stays legal: it is the de-address sentinel. */
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_device_commissioning_program_short_address(
                          &t, DALI_DEVICE_NO_SHORT_ADDRESS));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_start_sequence_uses_the_inverted_initialise_parameter);
    RUN_TEST(test_program_verify_sequence_carries_the_raw_short_address);
    RUN_TEST(test_search_compare_sequence_layout_matches_the_shared_classifier);
    RUN_TEST(test_sequence_builders_reject_bad_arguments);
    RUN_TEST(test_commission_assigns_free_addresses_in_order);
    RUN_TEST(test_commission_starts_from_the_requested_address);
    RUN_TEST(test_commission_skips_addresses_the_used_mask_reserves);
    RUN_TEST(test_commission_settles_after_randomise_before_the_first_compare);
    RUN_TEST(test_commission_refuses_a_transport_that_cannot_wait_and_sends_nothing);
    RUN_TEST(test_commission_reports_a_full_address_space_without_traffic);
    RUN_TEST(test_commission_terminates_on_the_way_out);
    RUN_TEST(test_a_failed_terminate_leaves_the_addressing_state_unknown);
    RUN_TEST(test_without_the_option_no_gear_frame_is_sent);
    RUN_TEST(test_the_cross_part_guard_brackets_the_run_with_part_102_terminate);
    RUN_TEST(test_a_failed_cross_part_terminate_does_not_abort_the_run);
    RUN_TEST(test_two_devices_sharing_a_random_address_are_recovered_and_the_run_continues);
    RUN_TEST(test_a_pair_that_will_not_withdraw_stops_the_run_rather_than_looping);
    RUN_TEST(test_query_short_address_reads_the_raw_value);
    RUN_TEST(test_used_mask_counts_control_devices_only);
    RUN_TEST(test_used_mask_reserves_undecodable_device_activity);
    RUN_TEST(test_used_mask_is_independent_of_the_gear_mask);
    RUN_TEST(test_invalid_arguments_are_rejected);
    return UNITY_END();
}
