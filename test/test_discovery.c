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
    /* 64 status + per found device (all queries timeout):
     *  groups-0-7(1), device_type(1), version(1), actual_level(1), num_instances(1),
     *  bank0: DTR1(1)+DTR0(1)+READ(1),
     *  bank1: DTR1(1)+DTR0(1)+READ(1),
     *  scene-levels: QUERY_SCENE_LEVEL 0-15(16)
     * + 62 QUERY_NUMBER_OF_INSTANCES probes for the 62 absent addresses */
    TEST_ASSERT_EQUAL_UINT32(2u * DALI_SHORT_ADDRESS_COUNT + 2u * 27u - 2u, s_bus.tx_count);
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
    /* addr 5 reports primary type=6 and secondary type=8 via QUERY_NEXT_DEVICE_TYPE.
     * QUERY_NEXT_DEVICE_TYPE opcode=0xA7, addr 5 address byte=0x0B → frame 0x0BA7 */
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;
    s_bus.status[5] = 0x00u;

    add_reply(0x0B99u, DALI_FORWARD_FRAME_BITS, DALI_OK, 6u,    DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 8u,    DALI_BACKWARD_FRAME_BITS);
    add_reply(0x0BA7u, DALI_FORWARD_FRAME_BITS, DALI_OK, 0xFFu, DALI_BACKWARD_FRAME_BITS);

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
    TEST_ASSERT_TRUE(dali_discovery_has_device_type(device, 6u));
    TEST_ASSERT_TRUE(dali_discovery_has_device_type(device, 8u));
    TEST_ASSERT_FALSE(dali_discovery_has_device_type(device, 0u));
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

void test_scan_reads_bank1_identity(void)
{
    /* addr 5: bank0 and bank1 both present.
     * Both reads use the same READ_MEMORY_LOCATION frame (0x0BC5) — the mock
     * consumes entries in order so bank0 reads take the first 18, bank1 takes the next 20. */
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;
    s_bus.status[5] = 0x00u;

    const uint8_t gtin[6]   = {0x04u, 0x00u, 0x78u, 0x1Au, 0x00u, 0x01u};
    const uint8_t serial[8] = {0xAAu, 0xBBu, 0xCCu, 0xDDu,
                                0x11u, 0x22u, 0x33u, 0x44u};
    const uint32_t read_frame = 0x0BC5u;

    /* Bank 0 — 18 bytes (offsets 0x00..0x11) */
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 0x11u, DALI_BACKWARD_FRAME_BITS);
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, DALI_MEMORY_BANK_IMPLEMENTED, DALI_BACKWARD_FRAME_BITS);
    for (uint8_t i = 0u; i < 6u; i++) {
        add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, gtin[i], DALI_BACKWARD_FRAME_BITS);
    }
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 2u, DALI_BACKWARD_FRAME_BITS);  /* fw_major */
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 5u, DALI_BACKWARD_FRAME_BITS);  /* fw_minor */
    for (uint8_t i = 0u; i < 8u; i++) {
        add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, serial[i], DALI_BACKWARD_FRAME_BITS);
    }

    /* Bank 1 — 20 bytes (offsets 0x00..0x13), same identity + hw version */
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 0x13u, DALI_BACKWARD_FRAME_BITS);
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, DALI_MEMORY_BANK_IMPLEMENTED, DALI_BACKWARD_FRAME_BITS);
    for (uint8_t i = 0u; i < 6u; i++) {
        add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, gtin[i], DALI_BACKWARD_FRAME_BITS);
    }
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 2u, DALI_BACKWARD_FRAME_BITS);  /* fw_major */
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 5u, DALI_BACKWARD_FRAME_BITS);  /* fw_minor */
    for (uint8_t i = 0u; i < 8u; i++) {
        add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, serial[i], DALI_BACKWARD_FRAME_BITS);
    }
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 1u, DALI_BACKWARD_FRAME_BITS);  /* hw_major */
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 0u, DALI_BACKWARD_FRAME_BITS);  /* hw_minor */

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_identity);
    TEST_ASSERT_TRUE(device->has_bank1);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(gtin,   device->bank1.gtin,   6u);
    TEST_ASSERT_EQUAL_UINT8(2u, device->bank1.fw_major);
    TEST_ASSERT_EQUAL_UINT8(5u, device->bank1.fw_minor);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(serial, device->bank1.serial, 8u);
    TEST_ASSERT_EQUAL_UINT8(1u, device->bank1.hw_major);
    TEST_ASSERT_EQUAL_UINT8(0u, device->bank1.hw_minor);
}

void test_scan_reads_bank0_identity(void)
{
    /* addr 5 present. Bank 0 read: offset 0x00..0x11 via READ_MEMORY_LOCATION (0x0BC5).
     * DTR1/DTR0 setup frames are no-reply and handled by the mock without scripting. */
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[5] = true;
    s_bus.status[5] = 0x00u;

    const uint8_t gtin[6]   = {0x04u, 0x00u, 0x78u, 0x1Au, 0x00u, 0x01u};
    const uint8_t serial[8] = {0xAAu, 0xBBu, 0xCCu, 0xDDu,
                                0x11u, 0x22u, 0x33u, 0x44u};

    /* READ_MEMORY_LOCATION at addr 5: address byte = (5<<1)|1 = 0x0B, opcode = 0xC5 */
    uint32_t read_frame = 0x0BC5u;
    /* 18 bytes: last_addr, indicator=0xFF, GTIN(6), fw_major, fw_minor, serial(8) */
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 0x11u, DALI_BACKWARD_FRAME_BITS);
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, DALI_MEMORY_BANK_IMPLEMENTED, DALI_BACKWARD_FRAME_BITS);
    for (uint8_t i = 0u; i < 6u; i++) {
        add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, gtin[i], DALI_BACKWARD_FRAME_BITS);
    }
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 2u, DALI_BACKWARD_FRAME_BITS);  /* fw_major */
    add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, 5u, DALI_BACKWARD_FRAME_BITS);  /* fw_minor */
    for (uint8_t i = 0u; i < 8u; i++) {
        add_reply(read_frame, DALI_FORWARD_FRAME_BITS, DALI_OK, serial[i], DALI_BACKWARD_FRAME_BITS);
    }

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    TEST_ASSERT_EQUAL_UINT8(1u, found);
    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 5u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->has_identity);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(gtin,   device->identity.gtin,   6u);
    TEST_ASSERT_EQUAL_UINT8(2u, device->identity.fw_major);
    TEST_ASSERT_EQUAL_UINT8(5u, device->identity.fw_minor);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(serial, device->identity.serial, 8u);
}

void test_scan_skips_identity_when_bank0_absent(void)
{
    DaliDiscoveryInventory inventory;
    uint8_t found = 0u;
    s_bus.present[3] = true;
    s_bus.status[3] = 0x00u;
    /* No READ_MEMORY_LOCATION replies scripted — all 18 reads will timeout.
     * The first read timeout causes read_bank0_identity to return early,
     * leaving has_identity false. */

    DaliDiscoveryTransport t = transport();
    TEST_ASSERT_EQUAL(DALI_OK,
                      dali_discovery_scan(&inventory, &t, NULL, NULL, &found));

    const DaliDiscoveryDeviceInfo *device = dali_discovery_inventory_get(&inventory, 3u);
    TEST_ASSERT_NOT_NULL(device);
    TEST_ASSERT_TRUE(device->present);
    TEST_ASSERT_FALSE(device->has_identity);
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
    RUN_TEST(test_scan_stores_group_membership);
    RUN_TEST(test_scan_detects_input_device_by_instance_count);
    RUN_TEST(test_scan_detects_pure_input_device_without_gear_status);
    RUN_TEST(test_scan_tolerates_group_query_timeout);
    RUN_TEST(test_scan_enriches_dt6_device);
    RUN_TEST(test_scan_skips_dt6_enrichment_for_non_dt6_devices);
    RUN_TEST(test_scan_reads_bank0_identity);
    RUN_TEST(test_scan_skips_identity_when_bank0_absent);
    RUN_TEST(test_has_device_type_helper);
    RUN_TEST(test_scan_enriches_multi_device_type_gear);
    RUN_TEST(test_scan_records_scene_levels);
    RUN_TEST(test_scan_reads_bank1_identity);
    RUN_TEST(test_invalid_arguments_are_rejected);
    return UNITY_END();
}
