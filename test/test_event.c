#include "unity.h"

#include "dali_event.h"

void setUp(void) {}
void tearDown(void) {}

void test_parse_short_address_event(void)
{
    DaliFrame frame = {
        .data = 0x0B0303u,
        .bit_length = DALI_EXTENDED_FRAME_BITS,
    };
    DaliInputEvent event;

    TEST_ASSERT_EQUAL(DALI_OK, dali_event_parse_frame(&frame, &event));
    TEST_ASSERT_EQUAL(DALI_EVENT_FRAME_INPUT_24BIT, event.frame_kind);
    TEST_ASSERT_EQUAL_HEX32(frame.data, event.raw.data);
    TEST_ASSERT_EQUAL_UINT8(DALI_EXTENDED_FRAME_BITS, event.raw.bit_length);
    TEST_ASSERT_EQUAL_HEX8(0x0Bu, event.address_byte);
    TEST_ASSERT_EQUAL(DALI_EVENT_ADDRESS_SHORT, event.address_kind);
    TEST_ASSERT_EQUAL_UINT8(5u, event.address);
    TEST_ASSERT_TRUE(event.address_selector);
    TEST_ASSERT_TRUE(event.has_instance);
    TEST_ASSERT_EQUAL_UINT8(3u, event.instance);
    TEST_ASSERT_EQUAL_HEX8(0x03u, event.event_code);
    TEST_ASSERT_TRUE(dali_event_is_push_button_double_press(&event));
    TEST_ASSERT_TRUE(dali_event_is_switch_mapping_candidate(&event));
    TEST_ASSERT_EQUAL_STRING("input-24bit",
                             dali_event_frame_kind_name(event.frame_kind));
    TEST_ASSERT_EQUAL_STRING("double-press", dali_event_action_name(&event));
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
    TEST_ASSERT_FALSE(event.has_instance);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, event.instance);
    TEST_ASSERT_EQUAL_HEX8(0x10u, event.event_code);
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
    TEST_ASSERT_EQUAL(DALI_EVENT_FRAME_LEGACY_16BIT, event.frame_kind);
    TEST_ASSERT_EQUAL(DALI_EVENT_ADDRESS_SHORT, event.address_kind);
    TEST_ASSERT_EQUAL_UINT8(5u, event.address);
    TEST_ASSERT_FALSE(event.address_selector);
    TEST_ASSERT_FALSE(event.has_instance);
    TEST_ASSERT_EQUAL_HEX8(0x7Fu, event.event_code);
    TEST_ASSERT_EQUAL_STRING("dapc-level", dali_event_action_name(&event));
}

void test_parse_rejects_backward_frame(void)
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

void test_event_queue_fifo_and_overflow(void)
{
    DaliInputEventQueue queue;
    DaliInputEventRecord record = {
        .event = {
            .raw = { .data = 0x0B0003u, .bit_length = DALI_EXTENDED_FRAME_BITS },
            .address_kind = DALI_EVENT_ADDRESS_SHORT,
            .address = 5u,
            .instance = 0u,
            .event_code = 0x03u,
        },
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
    TEST_ASSERT_EQUAL_HEX32(0x0B0003u, out.event.raw.data);
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
    RUN_TEST(test_parse_short_address_event);
    RUN_TEST(test_parse_legacy_16bit_command_frame);
    RUN_TEST(test_parse_legacy_16bit_dapc_frame);
    RUN_TEST(test_parse_rejects_backward_frame);
    RUN_TEST(test_event_queue_fifo_and_overflow);
    return UNITY_END();
}
