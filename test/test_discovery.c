#include "unity.h"
#include "dali_discovery.h"

#include <string.h>

typedef struct {
    bool      present[DALI_SHORT_ADDRESS_COUNT];
    uint8_t   status[DALI_SHORT_ADDRESS_COUNT];
    bool      malformed[DALI_SHORT_ADDRESS_COUNT];
    int       bus_error_addr;
    uint32_t  tx_count;
    uint32_t  found_cb_count;
    uint8_t   found_cb_last_addr;
} MockDiscoveryBus;

typedef struct {
    uint32_t  data;
    uint8_t   bit_length;
    DaliError err;
    uint8_t   reply;
    uint8_t   reply_bits;
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
    TEST_ASSERT_NOT_NULL(bus);
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_TRUE(needs_reply);
    TEST_ASSERT_EQUAL_UINT8(1u, retries_left);
    TEST_ASSERT_FALSE(send_twice);
    TEST_ASSERT_NOT_NULL(reply_out);
    bus->tx_count++;

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
        if (s_script[i].data == frame->data &&
            s_script[i].bit_length == frame->bit_length) {
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
    TEST_ASSERT_EQUAL_UINT32(DALI_SHORT_ADDRESS_COUNT, s_bus.tx_count);
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
    TEST_ASSERT_EQUAL_UINT32(5u, s_bus.tx_count);
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

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_inventory_reset(NULL));
    TEST_ASSERT_NULL(dali_discovery_inventory_get(NULL, 0u));
    TEST_ASSERT_NULL(dali_discovery_inventory_get(&inventory,
                                                  DALI_SHORT_ADDRESS_COUNT));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_discovery_inventory_store_status(NULL, 0u, 0u));
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
    RUN_TEST(test_invalid_arguments_are_rejected);
    return UNITY_END();
}
