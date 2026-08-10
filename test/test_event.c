#include "unity.h"

#include "dali_event.h"

void setUp(void) {}
void tearDown(void) {}

static DaliInputEvent parse_24(uint32_t raw)
{
    DaliFrame frame = {
        .data = raw,
        .bit_length = DALI_EXTENDED_FRAME_BITS,
    };
    DaliInputEvent event;

    TEST_ASSERT_EQUAL(DALI_OK, dali_event_parse_frame(&frame, &event));
    return event;
}

void test_parse_device_source(void)
{
    /* Independent Part-103 vector: short 63, type 1, short press. */
    DaliInputEvent event = parse_24(0x7E0402u);

    TEST_ASSERT_EQUAL(DALI_EVENT_FRAME_INPUT_24BIT, event.frame_kind);
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_DEVICE, event.source.scheme);
    TEST_ASSERT_TRUE(event.source.has_device_address);
    TEST_ASSERT_EQUAL_UINT8(63u, event.source.device_address);
    TEST_ASSERT_TRUE(event.source.has_instance_type);
    TEST_ASSERT_EQUAL_UINT8(DALI_DT301_INSTANCE_TYPE,
                            event.source.instance_type);
    TEST_ASSERT_FALSE(event.source.has_instance);
    TEST_ASSERT_EQUAL_HEX16(DALI_DT301_EVENT_SHORT_PRESS,
                            event.event_information);
    TEST_ASSERT_EQUAL_STRING("short-press", dali_event_action_name(&event));
}

void test_parse_device_instance_source_keeps_type_ambiguous(void)
{
    /* Device/instance scheme does not carry the instance type on the wire. */
    DaliInputEvent event = parse_24(0x028402u);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_DEVICE_INSTANCE, event.source.scheme);
    TEST_ASSERT_TRUE(event.source.has_device_address);
    TEST_ASSERT_EQUAL_UINT8(1u, event.source.device_address);
    TEST_ASSERT_TRUE(event.source.has_instance);
    TEST_ASSERT_EQUAL_UINT8(1u, event.source.instance);
    TEST_ASSERT_FALSE(event.source.has_instance_type);
    TEST_ASSERT_EQUAL_HEX16(0x002u, event.event_information);
    TEST_ASSERT_EQUAL_STRING("event", dali_event_action_name(&event));
    TEST_ASSERT_FALSE(dali_event_is_switch_mapping_candidate(&event));
}

void test_parse_device_group_source(void)
{
    DaliInputEvent event = parse_24(0x9E0409u);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_DEVICE_GROUP, event.source.scheme);
    TEST_ASSERT_TRUE(event.source.has_device_group);
    TEST_ASSERT_EQUAL_UINT8(15u, event.source.device_group);
    TEST_ASSERT_TRUE(event.source.has_instance_type);
    TEST_ASSERT_EQUAL_UINT8(1u, event.source.instance_type);
    TEST_ASSERT_EQUAL_HEX16(DALI_DT301_EVENT_LONG_PRESS_START,
                            event.event_information);
    TEST_ASSERT_EQUAL_STRING("long-press-start",
                             dali_event_action_name(&event));
}

void test_parse_instance_source(void)
{
    DaliInputEvent event = parse_24(0x82C401u);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_INSTANCE, event.source.scheme);
    TEST_ASSERT_TRUE(event.source.has_instance_type);
    TEST_ASSERT_EQUAL_UINT8(1u, event.source.instance_type);
    TEST_ASSERT_TRUE(event.source.has_instance);
    TEST_ASSERT_EQUAL_UINT8(17u, event.source.instance);
    TEST_ASSERT_EQUAL_HEX16(DALI_DT301_EVENT_BUTTON_PRESSED,
                            event.event_information);
}

void test_parse_instance_group_source_and_double_press(void)
{
    DaliInputEvent event = parse_24(0xFE0405u);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_INSTANCE_GROUP, event.source.scheme);
    TEST_ASSERT_TRUE(event.source.has_instance_group);
    TEST_ASSERT_EQUAL_UINT8(31u, event.source.instance_group);
    TEST_ASSERT_TRUE(event.source.has_instance_type);
    TEST_ASSERT_EQUAL_UINT8(1u, event.source.instance_type);
    TEST_ASSERT_EQUAL_HEX16(DALI_DT301_EVENT_DOUBLE_PRESS,
                            event.event_information);
    TEST_ASSERT_TRUE(dali_event_is_push_button_double_press(&event));
    TEST_ASSERT_TRUE(dali_event_is_switch_mapping_candidate(&event));
    TEST_ASSERT_EQUAL_STRING("double-press", dali_event_action_name(&event));
}

void test_event_information_is_full_ten_bits(void)
{
    /* Device short 1, type 4, event information 0x2AA. */
    DaliInputEvent event = parse_24(0x0212AAu);

    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_DEVICE, event.source.scheme);
    TEST_ASSERT_EQUAL_UINT8(1u, event.source.device_address);
    TEST_ASSERT_EQUAL_UINT8(4u, event.source.instance_type);
    TEST_ASSERT_EQUAL_HEX16(0x02AAu, event.event_information);
    TEST_ASSERT_EQUAL_STRING("event", dali_event_action_name(&event));
}

void test_parse_power_notification(void)
{
    DaliInputEvent event = parse_24(0xFEF2EAu);

    TEST_ASSERT_EQUAL(DALI_EVENT_FRAME_POWER_NOTIFICATION_24BIT,
                      event.frame_kind);
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_POWER_NOTIFICATION,
                      event.source.scheme);
    TEST_ASSERT_TRUE(event.source.has_device_group);
    TEST_ASSERT_EQUAL_UINT8(5u, event.source.device_group);
    TEST_ASSERT_TRUE(event.source.has_device_address);
    TEST_ASSERT_EQUAL_UINT8(42u, event.source.device_address);
    TEST_ASSERT_EQUAL_STRING("power-notification",
                             dali_event_action_name(&event));
}

void test_parse_power_notification_without_optional_sources(void)
{
    DaliInputEvent event = parse_24(0xFEE000u);

    TEST_ASSERT_EQUAL(DALI_EVENT_FRAME_POWER_NOTIFICATION_24BIT,
                      event.frame_kind);
    TEST_ASSERT_FALSE(event.source.has_device_group);
    TEST_ASSERT_EQUAL_UINT8(0u, event.source.device_group);
    TEST_ASSERT_FALSE(event.source.has_device_address);
    TEST_ASSERT_EQUAL_UINT8(0u, event.source.device_address);
}

void test_rejects_power_notification_payload_without_validity_flag(void)
{
    DaliInputEvent event;
    DaliFrame group_without_valid = {
        .data = 0xFEE080u,
        .bit_length = DALI_EXTENDED_FRAME_BITS,
    };
    DaliFrame address_without_valid = {
        .data = 0xFEE001u,
        .bit_length = DALI_EXTENDED_FRAME_BITS,
    };

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_event_parse_frame(&group_without_valid, &event));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_event_parse_frame(&address_without_valid, &event));
}

void test_rejects_command_and_reserved_source_frames(void)
{
    DaliInputEvent event;
    DaliFrame command = {
        .data = 0x0B0303u,
        .bit_length = DALI_EXTENDED_FRAME_BITS,
    };
    DaliFrame reserved = {
        .data = 0xCC8402u,
        .bit_length = DALI_EXTENDED_FRAME_BITS,
    };

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_event_parse_frame(&command, &event));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_event_parse_frame(&reserved, &event));
}

void test_parse_legacy_16bit_command_frame(void)
{
    DaliFrame frame = {
        .data = 0x8B10u,
        .bit_length = DALI_FORWARD_FRAME_BITS,
    };
    DaliInputEvent event;

    TEST_ASSERT_EQUAL(DALI_OK, dali_event_parse_frame(&frame, &event));
    TEST_ASSERT_EQUAL(DALI_EVENT_FRAME_LEGACY_16BIT, event.frame_kind);
    TEST_ASSERT_EQUAL_HEX32(frame.data, event.raw.data);
    TEST_ASSERT_EQUAL_UINT8(DALI_FORWARD_FRAME_BITS, event.raw.bit_length);
    TEST_ASSERT_EQUAL_HEX8(0x8Bu, event.address_byte);
    TEST_ASSERT_EQUAL(DALI_EVENT_ADDRESS_GROUP, event.address_kind);
    TEST_ASSERT_EQUAL_UINT8(5u, event.address);
    TEST_ASSERT_TRUE(event.address_selector);
    TEST_ASSERT_EQUAL_HEX8(0x10u, event.legacy_data);
    TEST_ASSERT_FALSE(event.source.has_instance);
    TEST_ASSERT_EQUAL(DALI_EVENT_SOURCE_NONE, event.source.scheme);
    TEST_ASSERT_FALSE(dali_event_is_push_button_double_press(&event));
    TEST_ASSERT_TRUE(dali_event_is_switch_mapping_candidate(&event));
    TEST_ASSERT_EQUAL_STRING("legacy-16bit",
                             dali_event_frame_kind_name(event.frame_kind));
    TEST_ASSERT_EQUAL_STRING("go-to-scene", dali_event_action_name(&event));
}

void test_parse_legacy_16bit_dapc_frame(void)
{
    DaliFrame frame = {
        .data = 0x0A7Fu,
        .bit_length = DALI_FORWARD_FRAME_BITS,
    };
    DaliInputEvent event;

    TEST_ASSERT_EQUAL(DALI_OK, dali_event_parse_frame(&frame, &event));
    TEST_ASSERT_EQUAL(DALI_EVENT_ADDRESS_SHORT, event.address_kind);
    TEST_ASSERT_EQUAL_UINT8(5u, event.address);
    TEST_ASSERT_FALSE(event.address_selector);
    TEST_ASSERT_EQUAL_HEX8(0x7Fu, event.legacy_data);
    TEST_ASSERT_EQUAL_STRING("dapc-level", dali_event_action_name(&event));
}

void test_parse_rejects_legacy_special_command_frame(void)
{
    /* Part-102 PING is a special command, not a group-6 OFF event. */
    DaliFrame frame = {
        .data = 0xAD00u,
        .bit_length = DALI_FORWARD_FRAME_BITS,
    };
    DaliInputEvent event;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_event_parse_frame(&frame, &event));
}

void test_parse_rejects_backward_frame_and_null(void)
{
    DaliFrame frame = {
        .data = 0x03u,
        .bit_length = DALI_BACKWARD_FRAME_BITS,
    };
    DaliInputEvent event;

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_event_parse_frame(&frame, &event));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_event_parse_frame(NULL, &event));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID, dali_event_parse_frame(&frame, NULL));
}

void test_parse_rejects_data_outside_declared_frame_width(void)
{
    DaliInputEvent event;
    DaliFrame over_16_bits = {
        .data = 0x018B10u,
        .bit_length = DALI_FORWARD_FRAME_BITS,
    };
    DaliFrame over_24_bits = {
        .data = 0x01028402u,
        .bit_length = DALI_EXTENDED_FRAME_BITS,
    };

    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_event_parse_frame(&over_16_bits, &event));
    TEST_ASSERT_EQUAL(DALI_ERR_INVALID,
                      dali_event_parse_frame(&over_24_bits, &event));
}

void test_dt301_names_use_standard_sparse_values(void)
{
    TEST_ASSERT_EQUAL_STRING("button-released", dali_dt301_event_name(0x000u));
    TEST_ASSERT_EQUAL_STRING("button-pressed", dali_dt301_event_name(0x001u));
    TEST_ASSERT_EQUAL_STRING("short-press", dali_dt301_event_name(0x002u));
    TEST_ASSERT_EQUAL_STRING("double-press", dali_dt301_event_name(0x005u));
    TEST_ASSERT_EQUAL_STRING("long-press-start", dali_dt301_event_name(0x009u));
    TEST_ASSERT_EQUAL_STRING("long-press-repeat", dali_dt301_event_name(0x00Bu));
    TEST_ASSERT_EQUAL_STRING("long-press-stop", dali_dt301_event_name(0x00Cu));
    TEST_ASSERT_EQUAL_STRING("button-free", dali_dt301_event_name(0x00Eu));
    TEST_ASSERT_EQUAL_STRING("button-stuck", dali_dt301_event_name(0x00Fu));
    TEST_ASSERT_EQUAL_STRING("unknown", dali_dt301_event_name(0x003u));
}

void test_event_queue_fifo_and_overflow(void)
{
    DaliInputEventQueue queue;
    DaliInputEventRecord record = {
        .event = parse_24(0x028402u),
        .timestamp_us = 1234u,
    };
    DaliInputEventRecord out;

    TEST_ASSERT_EQUAL(DALI_OK, dali_event_queue_init(&queue));
    TEST_ASSERT_EQUAL_UINT8(0u, dali_event_queue_count(&queue));
    TEST_ASSERT_FALSE(dali_event_queue_pop(&queue, &out));

    TEST_ASSERT_EQUAL(DALI_OK, dali_event_queue_push(&queue, &record));
    TEST_ASSERT_EQUAL_UINT8(1u, dali_event_queue_count(&queue));
    TEST_ASSERT_TRUE(dali_event_queue_pop(&queue, &out));
    TEST_ASSERT_EQUAL_UINT32(1234u, out.timestamp_us);
    TEST_ASSERT_EQUAL_HEX32(0x028402u, out.event.raw.data);
    TEST_ASSERT_EQUAL_UINT8(0u, dali_event_queue_count(&queue));

    for (uint8_t i = 0u; i < DALI_EVENT_QUEUE_CAPACITY; i++) {
        record.timestamp_us = i;
        TEST_ASSERT_EQUAL(DALI_OK, dali_event_queue_push(&queue, &record));
    }
    TEST_ASSERT_EQUAL(DALI_ERR_OVERFLOW, dali_event_queue_push(&queue, &record));
    TEST_ASSERT_EQUAL_UINT32(1u, dali_event_queue_dropped(&queue));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_device_source);
    RUN_TEST(test_parse_device_instance_source_keeps_type_ambiguous);
    RUN_TEST(test_parse_device_group_source);
    RUN_TEST(test_parse_instance_source);
    RUN_TEST(test_parse_instance_group_source_and_double_press);
    RUN_TEST(test_event_information_is_full_ten_bits);
    RUN_TEST(test_parse_power_notification);
    RUN_TEST(test_parse_power_notification_without_optional_sources);
    RUN_TEST(test_rejects_power_notification_payload_without_validity_flag);
    RUN_TEST(test_rejects_command_and_reserved_source_frames);
    RUN_TEST(test_parse_legacy_16bit_command_frame);
    RUN_TEST(test_parse_legacy_16bit_dapc_frame);
    RUN_TEST(test_parse_rejects_legacy_special_command_frame);
    RUN_TEST(test_parse_rejects_backward_frame_and_null);
    RUN_TEST(test_parse_rejects_data_outside_declared_frame_width);
    RUN_TEST(test_dt301_names_use_standard_sparse_values);
    RUN_TEST(test_event_queue_fifo_and_overflow);
    return UNITY_END();
}
