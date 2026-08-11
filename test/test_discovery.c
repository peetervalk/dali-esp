#include "unity.h"
#include "dali_discovery.h"

#include <string.h>

typedef struct {
    bool      present[DALI_SHORT_ADDRESS_COUNT];
    uint8_t   status[DALI_SHORT_ADDRESS_COUNT];
    bool      malformed[DALI_SHORT_ADDRESS_COUNT];
    int       bus_error_addr;
    uint32_t  tx_count;
    uint32_t  query_next_device_type_count;
    uint32_t  gear_memory_read_count;
    uint32_t  found_cb_count;
    uint8_t   found_cb_last_addr;
} MockDiscoveryBus;

typedef struct {
    uint32_t  data;
    uint8_t   bit_length;
    DaliError err;
    uint8_t   reply;
    uint8_t   reply_bits;
    bool      consumed;  /* entries are used once, enabling sequential reads of the same frame */
} MockScriptReply;

static MockDiscoveryBus s_bus;
static MockScriptReply  s_script[160];
static uint8_t          s_script_count;

static bool is_status_query(const DaliFrame *frame, uint8_t *addr_out)
{
    if (frame == NULL || frame->bit_length != DALI_FORWARD_FRAME_BITS) {
        return false;
    }

    uint8_t address_byte = (uint8_t)((frame->data >> 8u) & 0xFFu);
    uint8_t opcode = (uint8_t)(frame->data & 0xFFu);
    if ((address_byte & 0x01u) == 0u || opcode != 0x90u) {
        return false;
    }

    uint8_t addr = (uint8_t)(address_byte >> 1u);
    if (addr >= DALI_SHORT_ADDRESS_COUNT) {
        return false;
    }

    if (addr_out != NULL) {
        *addr_out = addr;
    }
    return true;
}

static void add_reply(uint32_t data,
                      uint8_t bit_length,
                      DaliError err,
                      uint8_t reply,
                      uint8_t reply_bits)
{
    TEST_ASSERT_LESS_THAN_UINT8((uint8_t)(sizeof(s_script) / sizeof(s_script[0])),
                                s_script_count);
    s_script[s_script_count++] = (MockScriptReply){
        .data = data,
        .bit_length = bit_length,
        .err = err,
        .reply = reply,
        .reply_bits = reply_bits,
    };
}

static DaliError mock_transact(const DaliFrame *frame,
                               bool needs_reply,
                               uint8_t retries_left,
                               bool send_twice,
                               DaliFrame *reply_out,
                               void *ctx)
{
    MockDiscoveryBus *bus = (MockDiscoveryBus *)ctx;
    (void)retries_left;
    TEST_ASSERT_NOT_NULL(bus);
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_FALSE(send_twice);
    bus->tx_count++;
    if (frame->bit_length == DALI_FORWARD_FRAME_BITS &&
        (uint8_t)(frame->data & 0xFFu) == 0xA7u) {
        bus->query_next_device_type_count++;
    }
    if (frame->bit_length == DALI_FORWARD_FRAME_BITS &&
        (uint8_t)(frame->data & 0xFFu) == 0xC5u) {
        bus->gear_memory_read_count++;
    }

    /* No-reply frames (e.g. DTR1/DTR0 memory setup) — just accept and return. */
    if (!needs_reply) {
        return DALI_OK;
    }

    TEST_ASSERT_NOT_NULL(reply_out);

    uint8_t addr = 0u;
    if (is_status_query(frame, &addr)) {
        if (bus->bus_error_addr == (int)addr) {
            return DALI_ERR_BUS_STUCK;
        }
        if (bus->malformed[addr]) {
            *reply_out = (DaliFrame){
                .data = bus->status[addr],
                .bit_length = DALI_FORWARD_FRAME_BITS,
            };
            return DALI_OK;
        }
        if (!bus->present[addr]) {
            return DALI_ERR_TIMEOUT;
        }
        *reply_out = (DaliFrame){
            .data = bus->status[addr],
            .bit_length = DALI_BACKWARD_FRAME_BITS,
        };
        return DALI_OK;
    }

    for (uint8_t i = 0u; i < s_script_count; i++) {
        if (!s_script[i].consumed &&
            s_script[i].data == frame->data &&
            s_script[i].bit_length == frame->bit_length) {
            s_script[i].consumed = true;
            if (s_script[i].err == DALI_OK) {
                *reply_out = (DaliFrame){
                    .data = s_script[i].reply,
                    .bit_length = s_script[i].reply_bits,
                };
            }
            return s_script[i].err;
        }
    }

    return DALI_ERR_TIMEOUT;
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

/*
 * Enrichment detects "multiple device types" with a standalone QUERY DEVICE
 * TYPE, then runs the enumeration as one sequence that re-issues that query as
 * its own first step. Both replies have to be scripted.
 */
static void add_multi_device_type_replies(void)
{
    add_reply(0x0B99u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0xFFu,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0B99u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0xFFu,
              DALI_BACKWARD_FRAME_BITS);
}

/*
 * The sequence has a fixed step count, so every QUERY NEXT DEVICE TYPE step is
 * transmitted regardless of where the list ended. Real gear keeps answering
 * 0xFE once it is exhausted; pad the script so the mock does the same and the
 * frame count stays independent of where parsing stopped.
 */
static void add_next_device_type_end_replies(uint8_t already_scripted)
{
    for (uint8_t i = already_scripted;
         i < DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS;
         i++) {
        add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0xFEu,
                  DALI_BACKWARD_FRAME_BITS);
    }
}

static void found_cb(uint8_t addr,
                     const DaliDiscoveryDeviceInfo *device,
                     void *ctx)
{
    MockDiscoveryBus *bus = (MockDiscoveryBus *)ctx;
    TEST_ASSERT_NOT_NULL(bus);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->present);
    TEST_ASSERT_TRUE(device->has_status);
    bus->found_cb_count++;
    bus->found_cb_last_addr = addr;
}

static DaliDiscoveryTransport transport(void)
{
    return (DaliDiscoveryTransport){
        .transact = mock_transact,
        .transact_sequence = mock_transact_sequence,
        .ctx = &s_bus,
    };
}

void setUp(void)
{
    memset(&s_bus, 0, sizeof(s_bus));
    memset(&s_script, 0, sizeof(s_script));
    s_bus.bus_error_addr = -1;
    s_script_count = 0u;
}

void tearDown(void) {}

void test_scan_records_responders_and_callback(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;

    s_bus.present[5] = true;
    s_bus.status[5] = 0x80u;
    s_bus.present[12] = true;
    s_bus.status[12] = 0x00u;

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, found_cb, &s_bus, &found));

    TEST_ASSERT_TRUE(inventory.valid);
    TEST_ASSERT_EQUAL_UINT8(2u, found);
    TEST_ASSERT_EQUAL_UINT8(2u, inventory.found_count);
    /* 64 status + per found device (all queries timeout):
     *  groups-0-7(1), device_type(1), version(1), actual_level(1), num_instances(1),
     *  bank0 identity attempt: DTR1(1)+DTR0(1)+READ(1),
     *  scene-levels: QUERY_SCENE_LEVEL 0-15(16)
     * + 62 QUERY_NUMBER_OF_INSTANCES probes for the 62 absent addresses */
    TEST_ASSERT_EQUAL_UINT32(2u * DALI_SHORT_ADDRESS_COUNT + 2u * 24u - 2u,
                             s_bus.tx_count);
    TEST_ASSERT_EQUAL_UINT32(2u, s_bus.found_cb_count);
    TEST_ASSERT_EQUAL_UINT8(12u, s_bus.found_cb_last_addr);

    const DaliDiscoveryDeviceInfo *device =
        dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->present);
    TEST_ASSERT_TRUE(device->has_status);
    TEST_ASSERT_EQUAL_HEX8(0x80u, device->status);
    TEST_ASSERT_FALSE(dali_discovery_inventory_get(&inventory, 6u)->present);
}

void test_scan_ignores_timeouts_and_malformed_slots(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;

    s_bus.malformed[2] = true;
    s_bus.status[2] = 0xAAu;
    s_bus.present[3] = true;
    s_bus.status[3] = 0x55u;

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_TRUE(inventory.valid);
    TEST_ASSERT_EQUAL_UINT8(1u, found);
    TEST_ASSERT_FALSE(dali_discovery_inventory_get(&inventory, 2u)->present);
    TEST_ASSERT_TRUE(dali_discovery_inventory_get(&inventory, 3u)->present);
}

void test_scan_aborts_on_bus_error(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 99u;

    s_bus.bus_error_addr = 4;
    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_ERR_BUS_STUCK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_FALSE(inventory.valid);
    TEST_ASSERT_EQUAL_UINT8(99u, found);
    /* 5 status queries (0..4) + 4 instance probes for absent addresses 0..3
     * before the bus-error at address 4 terminates the scan. */
    TEST_ASSERT_EQUAL_UINT32(9u, s_bus.tx_count);
}

void test_query_status_rejects_bad_reply_width(void)
{
    uint8_t status = 0u;
    s_bus.malformed[7] = true;

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_discovery_query_status(&t, 7u, &status));
}

void test_query_input_device_clamps_and_keeps_optional_timeouts(void)
{
    DaliDiscoveryInputDevice input;
    DaliDiscoveryTransport t = transport();

    add_reply(0x0BFE35u,
              DALI_EXTENDED_FRAME_BITS,
              DALI_OK,
              DALI_INPUT_MAX_INSTANCES + 2u,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0B0080u,
              DALI_EXTENDED_FRAME_BITS,
              DALI_OK,
              DALI_INPUT_INSTANCE_TYPE_LIGHT,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0B0086u,
              DALI_EXTENDED_FRAME_BITS,
              DALI_OK,
              DALI_YES_RESPONSE,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0B0081u,
              DALI_EXTENDED_FRAME_BITS,
              DALI_OK,
              12u,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0B0083u,
              DALI_EXTENDED_FRAME_BITS,
              DALI_OK,
              0x34u,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0B0082u,
              DALI_EXTENDED_FRAME_BITS,
              DALI_OK,
              0u,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0B0180u,
              DALI_EXTENDED_FRAME_BITS,
              DALI_OK,
              DALI_INPUT_INSTANCE_TYPE_OCCUPANCY,
              DALI_BACKWARD_FRAME_BITS);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_query_input_device(&t, 5u, &input));

    TEST_ASSERT_TRUE(input.device.has_instance_count);
    TEST_ASSERT_EQUAL_UINT8(DALI_INPUT_MAX_INSTANCES + 2u,
                            input.device.instance_count);
    TEST_ASSERT_EQUAL_UINT8(DALI_INPUT_MAX_INSTANCES,
                            dali_discovery_input_visible_instance_count(&input));

    TEST_ASSERT_EQUAL(DALI_OK, input.instance_type_errors[0]);
    TEST_ASSERT_TRUE(input.device.instances[0].has_type);
    TEST_ASSERT_EQUAL(DALI_INPUT_ROLE_LIGHT, input.device.instances[0].role);
    TEST_ASSERT_TRUE(input.device.instances[0].has_enabled);
    TEST_ASSERT_TRUE(input.device.instances[0].enabled);
    TEST_ASSERT_TRUE(input.device.instances[0].has_resolution);
    TEST_ASSERT_EQUAL_UINT8(12u, input.device.instances[0].resolution);
    TEST_ASSERT_TRUE(input.device.instances[0].has_status);
    TEST_ASSERT_EQUAL_HEX8(0x34u, input.device.instances[0].status);
    TEST_ASSERT_TRUE(input.device.instances[0].has_error);
    TEST_ASSERT_EQUAL_UINT8(0u, input.device.instances[0].error);

    TEST_ASSERT_EQUAL(DALI_OK, input.instance_type_errors[1]);
    TEST_ASSERT_EQUAL(DALI_INPUT_ROLE_OCCUPANCY, input.device.instances[1].role);
    TEST_ASSERT_FALSE(input.device.instances[1].has_enabled);
    TEST_ASSERT_FALSE(input.device.instances[1].has_resolution);

    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT, input.instance_type_errors[2]);
}

void test_query_input_device_records_type_errors(void)
{
    DaliDiscoveryInputDevice input;
    DaliDiscoveryTransport t = transport();

    add_reply(0x0BFE35u,
              DALI_EXTENDED_FRAME_BITS,
              DALI_OK,
              1u,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0B0080u,
              DALI_EXTENDED_FRAME_BITS,
              DALI_OK,
              DALI_INPUT_INSTANCE_TYPE_LIGHT,
              DALI_FORWARD_FRAME_BITS);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_query_input_device(&t, 5u, &input));

    TEST_ASSERT_TRUE(input.device.has_instance_count);
    TEST_ASSERT_EQUAL_UINT8(1u, input.device.instance_count);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED, input.instance_type_errors[0]);
    TEST_ASSERT_FALSE(input.device.instances[0].has_type);
}

void test_inventory_update_input_device_marks_present(void)
{
    DaliDiscoveryInventory inventory;
    DaliDiscoveryInputDevice input;

    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_inventory_reset(&inventory));
    input = (DaliDiscoveryInputDevice){0};
    input.device.address = 9u;
    input.device.has_instance_count = true;
    input.device.instance_count = 2u;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_inventory_update_input_device(&inventory,
                                                                  &input));

    const DaliDiscoveryDeviceInfo *device =
        dali_discovery_inventory_get(&inventory, 9u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->present);
    TEST_ASSERT_TRUE(device->has_input_device);
    TEST_ASSERT_TRUE(device->has_instance_count);
    TEST_ASSERT_EQUAL_UINT8(2u, device->instance_count);
    TEST_ASSERT_EQUAL_UINT8(1u, inventory.found_count);
}

void test_query_device_type_returns_value(void)
{
    /* addr 5: address byte = 0x0B, QUERY DEVICE TYPE opcode = 0x99 → frame 0x0B99 */
    add_reply(0x0B99u, DALI_FORWARD_FRAME_BITS, DALI_OK, 8u, DALI_BACKWARD_FRAME_BITS);

    uint8_t type = 0xFFu;
    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_query_device_type(&t, 5u, &type));
    TEST_ASSERT_EQUAL_UINT8(8u, type);
    TEST_ASSERT_EQUAL_STRING("colour", dali_discovery_device_type_name(type));
}

void test_query_version_returns_value(void)
{
    /* addr 5: QUERY VERSION NUMBER opcode = 0x97 → frame 0x0B97 */
    add_reply(0x0B97u, DALI_FORWARD_FRAME_BITS, DALI_OK, 4u, DALI_BACKWARD_FRAME_BITS);

    uint8_t version = 0u;
    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_query_version(&t, 5u, &version));
    TEST_ASSERT_EQUAL_UINT8(4u, version);
}

void test_query_actual_level_returns_value(void)
{
    /* addr 5: QUERY ACTUAL LEVEL opcode = 0xA0 → frame 0x0BA0 */
    add_reply(0x0BA0u, DALI_FORWARD_FRAME_BITS, DALI_OK, 254u, DALI_BACKWARD_FRAME_BITS);

    uint8_t level = 0u;
    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_query_actual_level(&t, 5u, &level));
    TEST_ASSERT_EQUAL_UINT8(254u, level);
}

void test_scan_stores_device_type_version_and_level(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;

    s_bus.present[5] = true;
    s_bus.status[5] = 0x00u;

    add_reply(0x0B99u, DALI_FORWARD_FRAME_BITS, DALI_OK, 8u,   DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0B97u, DALI_FORWARD_FRAME_BITS, DALI_OK, 4u,   DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BA0u, DALI_FORWARD_FRAME_BITS, DALI_OK, 254u, DALI_BACKWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_EQUAL_UINT8(1u, found);
    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_device_type);
    TEST_ASSERT_EQUAL_UINT8(8u, device->device_type);
    TEST_ASSERT_EQUAL_UINT8(1u, device->device_type_count);
    TEST_ASSERT_EQUAL_UINT8(8u, device->device_types[0]);
    TEST_ASSERT_FALSE(device->device_types_truncated);
    TEST_ASSERT_EQUAL_UINT32(0u, s_bus.query_next_device_type_count);
    TEST_ASSERT_TRUE(device->has_version);
    TEST_ASSERT_EQUAL_UINT8(4u, device->version);
    TEST_ASSERT_TRUE(device->has_actual_level);
    TEST_ASSERT_EQUAL_UINT8(254u, device->actual_level);
    TEST_ASSERT_FALSE(device->has_input_device);
}

void test_query_groups_returns_bitmask(void)
{
    /* addr 5: address byte = (5<<1)|1 = 0x0B
     * QUERY GROUPS 0-7  frame = 0x0BC0
     * QUERY GROUPS 8-15 frame = 0x0BC1 */
    add_reply(0x0BC0u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0xA3u, DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BC1u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0x01u, DALI_BACKWARD_FRAME_BITS);

    uint16_t groups = 0xFFFFu;
    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_query_groups(&t, 5u, &groups));
    TEST_ASSERT_EQUAL_HEX16(0x01A3u, groups);
}

/* ---------------------------------------------------------------------------
 * Sequence builders and result readers
 *
 * Frame expectations are derived from IEC 62386-102 rather than from the
 * builders: ENABLE DEVICE TYPE is special command 0xC1 carrying the type as its
 * data byte; QUERY GROUPS 0-7 / 8-15 are opcodes 0xC0 / 0xC1 on the addressed
 * frame; QUERY DEVICE TYPE / QUERY NEXT DEVICE TYPE are 0x99 / 0xA7.
 * --------------------------------------------------------------------------*/

void test_build_device_type_query_sequence_layout(void)
{
    /* DT6 QUERY FAILURE STATUS for addr 5 = address byte 0x0B, opcode 0xEC. */
    DaliFrame query = { .data = 0x0BECu, .bit_length = DALI_FORWARD_FRAME_BITS };
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_build_device_type_query_sequence(6u, &query, &seq));

    TEST_ASSERT_EQUAL_UINT8(DALI_DISCOVERY_DT_SEQUENCE_STEPS, seq.step_count);

    const DaliSequenceStep *enable = &seq.steps[DALI_DISCOVERY_DT_STEP_ENABLE];
    TEST_ASSERT_EQUAL_HEX32(0xC106u, enable->frame.data);
    TEST_ASSERT_EQUAL_UINT8(DALI_FORWARD_FRAME_BITS, enable->frame.bit_length);
    TEST_ASSERT_FALSE(enable->needs_reply);
    TEST_ASSERT_FALSE(enable->send_twice);
    TEST_ASSERT_EQUAL_UINT8(0u, enable->retries_left);

    const DaliSequenceStep *step = &seq.steps[DALI_DISCOVERY_DT_STEP_QUERY];
    TEST_ASSERT_EQUAL_HEX32(0x0BECu, step->frame.data);
    TEST_ASSERT_TRUE(step->needs_reply);
    TEST_ASSERT_FALSE(step->send_twice);
    /* ENABLE DEVICE TYPE is consumed by the next command, so the query must not
     * be retransmitted on its own. */
    TEST_ASSERT_EQUAL_UINT8(0u, step->retries_left);
}

void test_build_device_type_query_sequence_carries_the_requested_type(void)
{
    DaliFrame query = { .data = 0x0BF9u, .bit_length = DALI_FORWARD_FRAME_BITS };
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_build_device_type_query_sequence(8u, &query, &seq));
    TEST_ASSERT_EQUAL_HEX32(0xC108u,
                            seq.steps[DALI_DISCOVERY_DT_STEP_ENABLE].frame.data);
}

void test_build_device_type_query_sequence_rejects_bad_arguments(void)
{
    DaliFrame query = { .data = 0x0BECu, .bit_length = DALI_FORWARD_FRAME_BITS };
    DaliFrame backward = { .data = 0x12u, .bit_length = DALI_BACKWARD_FRAME_BITS };
    DaliFrame empty = { .data = 0u, .bit_length = 0u };
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_build_device_type_query_sequence(6u, NULL, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_build_device_type_query_sequence(6u, &query, NULL));
    /* Only a 16-bit forward frame can be answered under an enabled type. */
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_build_device_type_query_sequence(6u, &backward, &seq));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_build_device_type_query_sequence(6u, &empty, &seq));
}

void test_build_groups_sequence_layout(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_build_groups_sequence(5u, &seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_DISCOVERY_GROUPS_SEQUENCE_STEPS, seq.step_count);

    const DaliSequenceStep *low = &seq.steps[DALI_DISCOVERY_GROUPS_STEP_0_7];
    TEST_ASSERT_EQUAL_HEX32(0x0BC0u, low->frame.data);
    TEST_ASSERT_EQUAL_UINT8(DALI_FORWARD_FRAME_BITS, low->frame.bit_length);
    TEST_ASSERT_TRUE(low->needs_reply);

    const DaliSequenceStep *high = &seq.steps[DALI_DISCOVERY_GROUPS_STEP_8_15];
    TEST_ASSERT_EQUAL_HEX32(0x0BC1u, high->frame.data);
    TEST_ASSERT_TRUE(high->needs_reply);

    /* Both are read-only, so retrying a single step is safe here. */
    TEST_ASSERT_GREATER_THAN_UINT8(0u, low->retries_left);
    TEST_ASSERT_GREATER_THAN_UINT8(0u, high->retries_left);
}

void test_build_groups_sequence_rejects_bad_arguments(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_discovery_build_groups_sequence(0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_build_groups_sequence(DALI_SHORT_ADDRESS_COUNT, &seq));
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_build_groups_sequence(DALI_SHORT_ADDRESS_COUNT - 1u,
                                                           &seq));
}

void test_build_device_types_sequence_layout(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_build_device_types_sequence(5u, &seq));
    TEST_ASSERT_EQUAL_UINT8(DALI_DISCOVERY_DEVICE_TYPES_SEQUENCE_STEPS,
                            seq.step_count);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(DALI_SEQUENCE_MAX_STEPS, seq.step_count);

    /* Step 0 restarts the answer sequence inside the atomic block. */
    const DaliSequenceStep *first =
        &seq.steps[DALI_DISCOVERY_DEVICE_TYPES_STEP_FIRST];
    TEST_ASSERT_EQUAL_HEX32(0x0B99u, first->frame.data);
    TEST_ASSERT_TRUE(first->needs_reply);
    TEST_ASSERT_EQUAL_UINT8(0u, first->retries_left);

    for (uint8_t i = 0u; i < DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS; i++) {
        const DaliSequenceStep *step = &seq.steps[1u + i];
        TEST_ASSERT_EQUAL_HEX32(0x0BA7u, step->frame.data);
        TEST_ASSERT_EQUAL_UINT8(DALI_FORWARD_FRAME_BITS, step->frame.bit_length);
        TEST_ASSERT_TRUE(step->needs_reply);
        /* QUERY NEXT DEVICE TYPE advances the device's enumeration, so no step
         * may retry on its own. */
        TEST_ASSERT_EQUAL_UINT8(0u, step->retries_left);
    }
}

void test_build_device_types_sequence_rejects_bad_arguments(void)
{
    DaliSequence seq;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_build_device_types_sequence(0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_build_device_types_sequence(DALI_SHORT_ADDRESS_COUNT,
                                                                  &seq));
}

/* Build a completed result by hand so the readers are tested without a bus. */
static void seed_reply(DaliSequenceResult *result, uint8_t step, uint8_t value,
                       uint8_t bit_length)
{
    result->replies[step] = (DaliFrame){ .data = value, .bit_length = bit_length };
    result->reply_mask |= (uint8_t)(1u << step);
    if (step >= result->steps_run) {
        result->steps_run = (uint8_t)(step + 1u);
    }
}

static void result_reset(DaliSequenceResult *result)
{
    memset(result, 0, sizeof(*result));
    result->result = DALI_OK;
    result->failed_step = DALI_SEQUENCE_NO_FAILED_STEP;
}

void test_groups_from_sequence_assembles_low_and_high_bytes(void)
{
    DaliSequenceResult result;
    result_reset(&result);
    seed_reply(&result, DALI_DISCOVERY_GROUPS_STEP_0_7, 0xA3u,
               DALI_BACKWARD_FRAME_BITS);
    seed_reply(&result, DALI_DISCOVERY_GROUPS_STEP_8_15, 0x01u,
               DALI_BACKWARD_FRAME_BITS);

    uint16_t groups = 0xFFFFu;
    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_groups_from_sequence(&result, &groups));
    TEST_ASSERT_EQUAL_HEX16(0x01A3u, groups);
}

void test_groups_from_sequence_reports_failure_and_missing_replies(void)
{
    DaliSequenceResult result;
    uint16_t groups = 0xFFFFu;

    /* A failed sequence surfaces its own error rather than a partial mask. */
    result_reset(&result);
    result.result = DALI_ERR_TIMEOUT;
    result.failed_step = DALI_DISCOVERY_GROUPS_STEP_8_15;
    seed_reply(&result, DALI_DISCOVERY_GROUPS_STEP_0_7, 0xA3u,
               DALI_BACKWARD_FRAME_BITS);
    TEST_ASSERT_EQUAL(DALI_ERR_TIMEOUT,
                      dali_discovery_groups_from_sequence(&result, &groups));

    /* A nominally successful sequence missing the high byte is malformed. */
    result_reset(&result);
    seed_reply(&result, DALI_DISCOVERY_GROUPS_STEP_0_7, 0xA3u,
               DALI_BACKWARD_FRAME_BITS);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_discovery_groups_from_sequence(&result, &groups));

    /* So is a reply that came back the wrong width. */
    result_reset(&result);
    seed_reply(&result, DALI_DISCOVERY_GROUPS_STEP_0_7, 0xA3u,
               DALI_BACKWARD_FRAME_BITS);
    seed_reply(&result, DALI_DISCOVERY_GROUPS_STEP_8_15, 0x01u,
               DALI_FORWARD_FRAME_BITS);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_discovery_groups_from_sequence(&result, &groups));

    TEST_ASSERT_EQUAL_HEX16(0xFFFFu, groups);
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_groups_from_sequence(&result, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_groups_from_sequence(NULL, &groups));
}

void test_device_types_from_sequence_collects_ascending_list(void)
{
    DaliSequenceResult result;
    DaliDiscoveryDeviceInfo device;

    result_reset(&result);
    memset(&device, 0, sizeof(device));
    seed_reply(&result, 0u, 0xFFu, DALI_BACKWARD_FRAME_BITS);
    seed_reply(&result, 1u, 6u, DALI_BACKWARD_FRAME_BITS);
    seed_reply(&result, 2u, 8u, DALI_BACKWARD_FRAME_BITS);
    seed_reply(&result, 3u, 0xFEu, DALI_BACKWARD_FRAME_BITS);

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_device_types_from_sequence(&result, &device));
    TEST_ASSERT_TRUE(device.has_device_type);
    TEST_ASSERT_EQUAL_UINT8(6u, device.device_type);
    TEST_ASSERT_EQUAL_UINT8(2u, device.device_type_count);
    TEST_ASSERT_EQUAL_UINT8(6u, device.device_types[0]);
    TEST_ASSERT_EQUAL_UINT8(8u, device.device_types[1]);
    TEST_ASSERT_FALSE(device.device_types_truncated);
}

void test_device_types_from_sequence_keeps_types_gathered_before_a_failure(void)
{
    DaliSequenceResult result;
    DaliDiscoveryDeviceInfo device;

    /* The sequence aborted at step 3, but steps 0-2 already answered. Those
     * types are still valid and must not be thrown away. */
    result_reset(&result);
    memset(&device, 0, sizeof(device));
    result.result = DALI_ERR_TIMEOUT;
    result.failed_step = 3u;
    seed_reply(&result, 0u, 0xFFu, DALI_BACKWARD_FRAME_BITS);
    seed_reply(&result, 1u, 6u, DALI_BACKWARD_FRAME_BITS);
    seed_reply(&result, 2u, 8u, DALI_BACKWARD_FRAME_BITS);
    result.steps_run = 4u;

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_device_types_from_sequence(&result, &device));
    TEST_ASSERT_EQUAL_UINT8(2u, device.device_type_count);
    TEST_ASSERT_EQUAL_UINT8(6u, device.device_types[0]);
    TEST_ASSERT_EQUAL_UINT8(8u, device.device_types[1]);
}

void test_device_types_from_sequence_handles_single_type_and_no_types(void)
{
    DaliSequenceResult result;
    DaliDiscoveryDeviceInfo device;

    /* The leading query answered a concrete type this time — take it and stop. */
    result_reset(&result);
    memset(&device, 0, sizeof(device));
    seed_reply(&result, 0u, 6u, DALI_BACKWARD_FRAME_BITS);
    seed_reply(&result, 1u, 8u, DALI_BACKWARD_FRAME_BITS);
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_device_types_from_sequence(&result, &device));
    TEST_ASSERT_EQUAL_UINT8(1u, device.device_type_count);
    TEST_ASSERT_EQUAL_UINT8(6u, device.device_types[0]);

    /* 0xFE from the leading query means the device has no types at all. */
    result_reset(&result);
    memset(&device, 0, sizeof(device));
    seed_reply(&result, 0u, 0xFEu, DALI_BACKWARD_FRAME_BITS);
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_discovery_device_types_from_sequence(&result, &device));
    TEST_ASSERT_EQUAL_UINT8(0u, device.device_type_count);

    /* No reply at all is equally unusable. */
    result_reset(&result);
    memset(&device, 0, sizeof(device));
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_discovery_device_types_from_sequence(&result, &device));

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_device_types_from_sequence(NULL, &device));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_device_types_from_sequence(&result, NULL));
}

void test_device_types_from_sequence_marks_truncation_at_capacity(void)
{
    DaliSequenceResult result;
    DaliDiscoveryDeviceInfo device;

    result_reset(&result);
    memset(&device, 0, sizeof(device));
    seed_reply(&result, 0u, 0xFFu, DALI_BACKWARD_FRAME_BITS);
    for (uint8_t i = 0u; i < DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS; i++) {
        seed_reply(&result, (uint8_t)(1u + i), i, DALI_BACKWARD_FRAME_BITS);
    }

    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_device_types_from_sequence(&result, &device));
    TEST_ASSERT_EQUAL_UINT8(DALI_DISCOVERY_MAX_DEVICE_TYPES,
                            device.device_type_count);
    TEST_ASSERT_TRUE(device.device_types_truncated);
}

void test_u8_from_sequence_boundaries(void)
{
    DaliSequenceResult result;
    uint8_t value = 0u;

    result_reset(&result);
    seed_reply(&result, 1u, 0x5Au, DALI_BACKWARD_FRAME_BITS);

    TEST_ASSERT_EQUAL(DALI_OK, dali_discovery_u8_from_sequence(&result, 1u, &value));
    TEST_ASSERT_EQUAL_HEX8(0x5Au, value);

    /* A step that produced no reply, and one past the end of the array. */
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_discovery_u8_from_sequence(&result, 0u, &value));
    TEST_ASSERT_EQUAL(DALI_ERR_MALFORMED,
                      dali_discovery_u8_from_sequence(&result,
                                                      DALI_SEQUENCE_MAX_STEPS,
                                                      &value));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_u8_from_sequence(&result, 1u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_u8_from_sequence(NULL, 1u, &value));
}

void test_scan_stores_group_membership(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;

    s_bus.present[5] = true;
    s_bus.status[5] = 0x00u;

    /* addr 5 QUERY GROUPS 0-7 → groups 0 and 2 (0x05), 8-15 → group 9 (0x02) */
    add_reply(0x0BC0u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0x05u, DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BC1u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0x02u, DALI_BACKWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_EQUAL_UINT8(1u, found);
    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_groups);
    TEST_ASSERT_EQUAL_HEX16(0x0205u, device->groups);
    TEST_ASSERT_TRUE(
        dali_discovery_inventory_has_complete_group_data(&inventory));
}

void test_scan_detects_input_device_by_instance_count(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;

    s_bus.present[7] = true;
    s_bus.status[7] = 0x00u;

    /* addr 7: address byte = (7<<1)|1 = 0x0F
     * QUERY NUMBER OF INSTANCES is a 24-bit frame: address_byte=0x0F, instance_byte=0xFE, opcode=0x35
     * data = (0x0F << 16) | (0xFE << 8) | 0x35 = 0x0FFE35, DALI_EXTENDED_FRAME_BITS */
    add_reply(0x0FFE35u, DALI_EXTENDED_FRAME_BITS, DALI_OK, 2u, DALI_BACKWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_EQUAL_UINT8(1u, found);
    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 7u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_input_device);
    TEST_ASSERT_TRUE(device->has_instance_count);
    TEST_ASSERT_EQUAL_UINT8(2u, device->instance_count);
}

/* Pure input device: does NOT answer QUERY STATUS (no gear), but does answer
 * QUERY NUMBER OF INSTANCES. Covers the Steinel-style pure control device. */
void test_scan_detects_pure_input_device_without_gear_status(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;

    /* addr 3 is NOT in s_bus.present[], so QUERY STATUS returns DALI_ERR_TIMEOUT. */

    /* addr 3: address byte = (3<<1)|1 = 0x07
     * QUERY NUMBER OF INSTANCES: data = (0x07 << 16) | (0xFE << 8) | 0x35 = 0x07FE35 */
    add_reply(0x07FE35u, DALI_EXTENDED_FRAME_BITS, DALI_OK, 3u, DALI_BACKWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_EQUAL_UINT8(1u, found);
    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 3u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->present);
    TEST_ASSERT_FALSE(device->has_status);  /* no gear status */
    TEST_ASSERT_TRUE(device->has_input_device);
    TEST_ASSERT_TRUE(device->has_instance_count);
    TEST_ASSERT_EQUAL_UINT8(3u, device->instance_count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_bus.gear_memory_read_count);
    TEST_ASSERT_TRUE(
        dali_discovery_inventory_has_complete_group_data(&inventory));
}

void test_scan_classifies_gear_when_status_reply_is_missed(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;

    /* QUERY STATUS times out, but the control-device and gear address spaces
     * can both contain address 3. Positive replies from both roles must merge. */
    add_reply(0x07FE35u, DALI_EXTENDED_FRAME_BITS, DALI_OK, 2u,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x07C0u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0x04u,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x07C1u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0x00u,
              DALI_BACKWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_EQUAL_UINT8(1u, found);
    const DaliDiscoveryDeviceInfo *device =
        dali_discovery_inventory_get(&inventory, 3u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_FALSE(device->has_status);
    TEST_ASSERT_TRUE(device->has_input_device);
    TEST_ASSERT_TRUE(device->has_control_gear);
    TEST_ASSERT_TRUE(device->has_groups);
    TEST_ASSERT_EQUAL_HEX16(0x0004u, device->groups);
    TEST_ASSERT_TRUE(
        dali_discovery_inventory_has_complete_group_data(&inventory));
}

void test_scan_tolerates_group_query_timeout(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;

    s_bus.present[3] = true;
    s_bus.status[3] = 0x00u;
    /* no group scripts → group queries timeout; scan should still succeed */

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_EQUAL_UINT8(1u, found);
    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 3u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->present);
    TEST_ASSERT_FALSE(device->has_groups);
    TEST_ASSERT_FALSE(
        dali_discovery_inventory_has_complete_group_data(&inventory));
}

void test_scan_rejects_frame_only_transport_without_traffic(void)
{
    DaliDiscoveryTransport frame_only = {
        .transact = mock_transact,
        .ctx = &s_bus,
    };
    DaliDiscoveryInventory inventory;
    uint8_t found = 0xA5u;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_scan(&inventory,
                                          &frame_only,
                                          NULL,
                                          NULL,
                                          &found));
    TEST_ASSERT_EQUAL_UINT32(0u, s_bus.tx_count);
    TEST_ASSERT_EQUAL_HEX8(0xA5u, found);
}

void test_invalid_arguments_are_rejected(void)
{
    DaliDiscoveryInventory inventory;
    DaliDiscoveryInputDevice input;
    DaliDiscoveryTransport t = transport();
    DaliFrame frame = {
        .data = 0x0B90u,
        .bit_length = DALI_FORWARD_FRAME_BITS,
    };
    uint8_t value = 0u;
    uint16_t groups = 0u;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_inventory_reset(NULL));
    TEST_ASSERT_NULL(dali_discovery_inventory_get(NULL, 0u));
    TEST_ASSERT_NULL(dali_discovery_inventory_get(&inventory,
                                                  DALI_SHORT_ADDRESS_COUNT));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_inventory_store_status(NULL, 0u, 0u));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_inventory_store_groups(NULL, 0u, 0u));
    TEST_ASSERT_FALSE(
        dali_discovery_inventory_has_complete_group_data(NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_u8(NULL, &frame, &value));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_u8(&t, NULL, &value));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_u8(&t, &frame, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_status(&t,
                                                  DALI_SHORT_ADDRESS_COUNT,
                                                  &value));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_groups(NULL, 0u, &groups));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_groups(&t, 0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_device_type(NULL, 0u, &value));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_device_type(&t, 0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_version(NULL, 0u, &value));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_version(&t, 0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_actual_level(NULL, 0u, &value));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_actual_level(&t, 0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_scan(NULL, &t, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_input_device(&t, 0u, NULL));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_query_input_device(&t,
                                                        DALI_SHORT_ADDRESS_COUNT,
                                                        &input));
    TEST_ASSERT_EQUAL_UINT8(0u,
                            dali_discovery_input_visible_instance_count(NULL));
}

void test_scan_enriches_dt6_device(void)
{
    /* addr 5 responds as device type 6 (LED). Enrichment should query failure
     * status and features via ENABLE DEVICE TYPE 6 + query pairs.
     * The ENABLE frames are no-reply; only the query replies need scripting.
     *
     * Frame data for addr 5 (address byte = (5<<1)|1 = 0x0B):
     *   QUERY DEVICE TYPE:    0x0B99 → 6
     *   QUERY FAILURE STATUS: 0x0BF1 → 0x03 (short + open circuit)
     *   QUERY FEATURES:       0x0BF0 → 0x05
     */
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;
    s_bus.status[5] = 0x00u;

    add_reply(0x0B99u, DALI_FORWARD_FRAME_BITS, DALI_OK, 6u,    DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0B97u, DALI_FORWARD_FRAME_BITS, DALI_OK, 2u,    DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BA0u, DALI_FORWARD_FRAME_BITS, DALI_OK, 200u,  DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BF1u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0x03u, DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BF0u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0x05u, DALI_BACKWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_EQUAL_UINT8(1u, found);
    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_device_type);
    TEST_ASSERT_EQUAL_UINT8(6u, device->device_type);
    TEST_ASSERT_TRUE(device->has_dt6);
    TEST_ASSERT_EQUAL_HEX8(0x03u, device->dt6_failure_status);
    TEST_ASSERT_EQUAL_HEX8(0x05u, device->dt6_features);
}

void test_scan_skips_dt6_enrichment_for_non_dt6_devices(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[3] = true;
    s_bus.status[3] = 0x00u;

    /* device type 0 (fluorescent) — no DT6 enrichment */
    add_reply(0x0799u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0u, DALI_BACKWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 3u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_FALSE(device->has_dt6);
}

void test_has_device_type_helper(void)
{
    DaliDiscoveryDeviceInfo device = {0};

    TEST_ASSERT_FALSE(dali_discovery_has_device_type(NULL, 6u));
    TEST_ASSERT_FALSE(dali_discovery_has_device_type(&device, 6u));

    device.has_device_type = true;
    device.device_type_count = 1u;
    device.device_types[0] = 6u;
    device.device_type = 6u;
    TEST_ASSERT_TRUE(dali_discovery_has_device_type(&device, 6u));
    TEST_ASSERT_FALSE(dali_discovery_has_device_type(&device, 8u));

    device.device_type_count = 2u;
    device.device_types[1] = 8u;
    TEST_ASSERT_TRUE(dali_discovery_has_device_type(&device, 6u));
    TEST_ASSERT_TRUE(dali_discovery_has_device_type(&device, 8u));
    TEST_ASSERT_FALSE(dali_discovery_has_device_type(&device, 0u));
}

void test_scan_enriches_multi_device_type_gear(void)
{
    /* A QUERY DEVICE TYPE reply of MASK (0xFF) means multiple types. The gear
     * then returns each type through QUERY NEXT DEVICE TYPE and 0xFE at end. */
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;
    s_bus.status[5] = 0x00u;

    add_multi_device_type_replies();
    add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 6u,    DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 8u,    DALI_BACKWARD_FRAME_BITS);
    add_next_device_type_end_replies(2u);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_EQUAL_UINT8(1u, found);
    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_device_type);
    TEST_ASSERT_EQUAL_UINT8(6u, device->device_type);
    TEST_ASSERT_EQUAL_UINT8(2u, device->device_type_count);
    TEST_ASSERT_EQUAL_UINT8(6u, device->device_types[0]);
    TEST_ASSERT_EQUAL_UINT8(8u, device->device_types[1]);
    TEST_ASSERT_FALSE(device->device_types_truncated);
    TEST_ASSERT_EQUAL_UINT32(DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS,
                             s_bus.query_next_device_type_count);
    TEST_ASSERT_TRUE(dali_discovery_has_device_type(device, 6u));
    TEST_ASSERT_TRUE(dali_discovery_has_device_type(device, 8u));
    TEST_ASSERT_FALSE(dali_discovery_has_device_type(device, 0u));
}

void test_scan_handles_gear_with_no_device_types(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;

    /* DALI-2 defines 0xFE from QUERY DEVICE TYPE as "no device types". */
    add_reply(0x0B99u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0xFEu,
              DALI_BACKWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device =
        dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_FALSE(device->has_device_type);
    TEST_ASSERT_EQUAL_UINT8(0u, device->device_type_count);
    TEST_ASSERT_FALSE(device->device_types_truncated);
    TEST_ASSERT_EQUAL_UINT32(0u, s_bus.query_next_device_type_count);
}

void test_scan_ignores_malformed_device_type_reply(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;

    add_reply(0x0B99u, DALI_FORWARD_FRAME_BITS, DALI_OK, 6u,
              DALI_FORWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device =
        dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_FALSE(device->has_device_type);
    TEST_ASSERT_EQUAL_UINT8(0u, device->device_type_count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_bus.query_next_device_type_count);
}

void test_scan_stops_on_invalid_next_device_type_mask(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;

    add_multi_device_type_replies();
    add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 1u,
              DALI_BACKWARD_FRAME_BITS);
    /* MASK is only valid as the initial "multiple types" result. */
    add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0xFFu,
              DALI_BACKWARD_FRAME_BITS);
    /* Later steps still answer, proving MASK ends the list rather than the
     * sequence simply running out of replies. */
    add_next_device_type_end_replies(2u);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device =
        dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_device_type);
    TEST_ASSERT_EQUAL_UINT8(1u, device->device_type_count);
    TEST_ASSERT_EQUAL_UINT8(1u, device->device_type);
    TEST_ASSERT_FALSE(device->device_types_truncated);
    TEST_ASSERT_EQUAL_UINT32(DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS,
                             s_bus.query_next_device_type_count);
}

void test_scan_stops_on_malformed_next_device_type_reply(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;

    add_multi_device_type_replies();
    add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 1u,
              DALI_FORWARD_FRAME_BITS);
    /* A well-formed tail must not rescue a list that already went bad. */
    add_next_device_type_end_replies(1u);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device =
        dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_FALSE(device->has_device_type);
    TEST_ASSERT_EQUAL_UINT8(0u, device->device_type_count);
    TEST_ASSERT_FALSE(device->device_types_truncated);
    TEST_ASSERT_EQUAL_UINT32(DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS,
                             s_bus.query_next_device_type_count);
}

void test_scan_stops_on_non_increasing_device_types(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;

    add_multi_device_type_replies();
    add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 3u,
              DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 2u,
              DALI_BACKWARD_FRAME_BITS);
    add_next_device_type_end_replies(2u);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device =
        dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_device_type);
    TEST_ASSERT_EQUAL_UINT8(1u, device->device_type_count);
    TEST_ASSERT_EQUAL_UINT8(3u, device->device_type);
    TEST_ASSERT_FALSE(device->device_types_truncated);
    TEST_ASSERT_EQUAL_UINT32(DALI_DISCOVERY_DEVICE_TYPES_NEXT_STEPS,
                             s_bus.query_next_device_type_count);
}

void test_scan_marks_excess_device_types_truncated(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;

    add_multi_device_type_replies();
    for (uint8_t type = 0u; type <= DALI_DISCOVERY_MAX_DEVICE_TYPES; type++) {
        add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, type,
                  DALI_BACKWARD_FRAME_BITS);
    }

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device =
        dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_device_type);
    TEST_ASSERT_EQUAL_UINT8(DALI_DISCOVERY_MAX_DEVICE_TYPES,
                            device->device_type_count);
    for (uint8_t type = 0u; type < DALI_DISCOVERY_MAX_DEVICE_TYPES; type++) {
        TEST_ASSERT_EQUAL_UINT8(type, device->device_types[type]);
    }
    TEST_ASSERT_TRUE(device->device_types_truncated);
    TEST_ASSERT_EQUAL_UINT32(DALI_DISCOVERY_MAX_DEVICE_TYPES + 1u,
                             s_bus.query_next_device_type_count);
}

void test_scan_does_not_mark_exact_device_type_capacity_truncated(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;

    add_multi_device_type_replies();
    for (uint8_t type = 0u; type < DALI_DISCOVERY_MAX_DEVICE_TYPES; type++) {
        add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, type,
                  DALI_BACKWARD_FRAME_BITS);
    }
    add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0xFEu,
              DALI_BACKWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device =
        dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_EQUAL_UINT8(DALI_DISCOVERY_MAX_DEVICE_TYPES,
                            device->device_type_count);
    TEST_ASSERT_FALSE(device->device_types_truncated);
    TEST_ASSERT_EQUAL_UINT32(DALI_DISCOVERY_MAX_DEVICE_TYPES + 1u,
                             s_bus.query_next_device_type_count);
}

void test_scan_records_scene_levels(void)
{
    /* addr 5: scenes 0 and 7 are configured; scene 2 is MASK (0xFF).
     * Unscripted scenes time out and stay 0 in the array.
     * QUERY_SCENE_LEVEL opcode base = 0xB0, addr 5 address byte = 0x0B */
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;
    s_bus.status[5] = 0x00u;

    add_reply(0x0BB0u, DALI_FORWARD_FRAME_BITS, DALI_OK, 120u,  DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BB2u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0xFFu, DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BB7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 200u,  DALI_BACKWARD_FRAME_BITS);

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_scene_levels);
    TEST_ASSERT_EQUAL_UINT8(120u,  device->scene_levels[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, device->scene_levels[2]);
    TEST_ASSERT_EQUAL_UINT8(200u,  device->scene_levels[7]);
}

void test_scan_reads_bank0_identity(void)
{
    /* addr 5 present. Bank 0 identity read: offsets 0x03..0x14 via 0x0BC5.
     * DTR1/DTR0 setup frames are no-reply and handled by the mock without scripting. */
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;
    s_bus.status[5] = 0x00u;

    const uint8_t identity_bytes[18] = {
        0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu,
        0x02u, 0x05u,
        0x10u, 0x32u, 0x54u, 0x76u, 0x98u, 0xBAu, 0xDCu, 0xFEu,
        0x03u, 0x07u,
    };
    const uint8_t expected_gtin[6] = {
        0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu,
    };
    const uint8_t expected_identification[8] = {
        0x10u, 0x32u, 0x54u, 0x76u, 0x98u, 0xBAu, 0xDCu, 0xFEu,
    };

    /* READ_MEMORY_LOCATION at addr 5: address byte = (5<<1)|1 = 0x0B, opcode = 0xC5 */
    for (uint8_t i = 0u; i < (uint8_t)sizeof(identity_bytes); i++) {
        add_reply(0x0BC5u, DALI_FORWARD_FRAME_BITS, DALI_OK, identity_bytes[i],
                  DALI_BACKWARD_FRAME_BITS);
    }

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_EQUAL_UINT8(1u, found);
    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_identity);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_gtin, device->identity.gtin, 6u);
    TEST_ASSERT_EQUAL_UINT8(2u, device->identity.fw_major);
    TEST_ASSERT_EQUAL_UINT8(5u, device->identity.fw_minor);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_identification,
                                  device->identity.serial, 8u);
    TEST_ASSERT_EQUAL_UINT8(3u, device->identity.hw_major);
    TEST_ASSERT_EQUAL_UINT8(7u, device->identity.hw_minor);
    TEST_ASSERT_EQUAL_UINT32(18u, s_bus.gear_memory_read_count);
}

void test_scan_skips_identity_when_bank0_absent(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[3] = true;
    s_bus.status[3] = 0x00u;
    /* No READ_MEMORY_LOCATION replies scripted. The first 0x03 read timeout
     * causes read_bank0_identity to return early,
     * leaving has_identity false. */

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 3u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->present);
    TEST_ASSERT_FALSE(device->has_identity);
    TEST_ASSERT_EQUAL_UINT32(1u, s_bus.gear_memory_read_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scan_records_responders_and_callback);
    RUN_TEST(test_scan_ignores_timeouts_and_malformed_slots);
    RUN_TEST(test_scan_aborts_on_bus_error);
    RUN_TEST(test_query_status_rejects_bad_reply_width);
    RUN_TEST(test_query_input_device_clamps_and_keeps_optional_timeouts);
    RUN_TEST(test_query_input_device_records_type_errors);
    RUN_TEST(test_inventory_update_input_device_marks_present);
    RUN_TEST(test_query_device_type_returns_value);
    RUN_TEST(test_query_version_returns_value);
    RUN_TEST(test_query_actual_level_returns_value);
    RUN_TEST(test_scan_stores_device_type_version_and_level);
    RUN_TEST(test_query_groups_returns_bitmask);
    RUN_TEST(test_build_device_type_query_sequence_layout);
    RUN_TEST(test_build_device_type_query_sequence_carries_the_requested_type);
    RUN_TEST(test_build_device_type_query_sequence_rejects_bad_arguments);
    RUN_TEST(test_build_groups_sequence_layout);
    RUN_TEST(test_build_groups_sequence_rejects_bad_arguments);
    RUN_TEST(test_build_device_types_sequence_layout);
    RUN_TEST(test_build_device_types_sequence_rejects_bad_arguments);
    RUN_TEST(test_groups_from_sequence_assembles_low_and_high_bytes);
    RUN_TEST(test_groups_from_sequence_reports_failure_and_missing_replies);
    RUN_TEST(test_device_types_from_sequence_collects_ascending_list);
    RUN_TEST(test_device_types_from_sequence_keeps_types_gathered_before_a_failure);
    RUN_TEST(test_device_types_from_sequence_handles_single_type_and_no_types);
    RUN_TEST(test_device_types_from_sequence_marks_truncation_at_capacity);
    RUN_TEST(test_u8_from_sequence_boundaries);
    RUN_TEST(test_scan_stores_group_membership);
    RUN_TEST(test_scan_detects_input_device_by_instance_count);
    RUN_TEST(test_scan_detects_pure_input_device_without_gear_status);
    RUN_TEST(test_scan_classifies_gear_when_status_reply_is_missed);
    RUN_TEST(test_scan_tolerates_group_query_timeout);
    RUN_TEST(test_scan_enriches_dt6_device);
    RUN_TEST(test_scan_skips_dt6_enrichment_for_non_dt6_devices);
    RUN_TEST(test_scan_reads_bank0_identity);
    RUN_TEST(test_scan_skips_identity_when_bank0_absent);
    RUN_TEST(test_has_device_type_helper);
    RUN_TEST(test_scan_enriches_multi_device_type_gear);
    RUN_TEST(test_scan_handles_gear_with_no_device_types);
    RUN_TEST(test_scan_ignores_malformed_device_type_reply);
    RUN_TEST(test_scan_stops_on_invalid_next_device_type_mask);
    RUN_TEST(test_scan_stops_on_malformed_next_device_type_reply);
    RUN_TEST(test_scan_stops_on_non_increasing_device_types);
    RUN_TEST(test_scan_marks_excess_device_types_truncated);
    RUN_TEST(test_scan_does_not_mark_exact_device_type_capacity_truncated);
    RUN_TEST(test_scan_records_scene_levels);
    RUN_TEST(test_scan_rejects_frame_only_transport_without_traffic);
    RUN_TEST(test_invalid_arguments_are_rejected);
    return UNITY_END();
}
