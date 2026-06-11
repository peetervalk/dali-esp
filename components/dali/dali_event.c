#include "dali_event.h"

#include <string.h>

static DaliEventAddressKind decode_address_kind(uint8_t address_byte)
{
    if (address_byte == DALI_BROADCAST_DAPC_ADDRESS ||
        address_byte == DALI_BROADCAST_COMMAND_ADDRESS) {
        return DALI_EVENT_ADDRESS_BROADCAST;
    }
    if ((address_byte & 0x80u) != 0u) {
        return DALI_EVENT_ADDRESS_GROUP;
    }
    return DALI_EVENT_ADDRESS_SHORT;
}

static uint8_t decode_address(uint8_t address_byte, DaliEventAddressKind kind)
{
    if (kind == DALI_EVENT_ADDRESS_SHORT) {
        return (uint8_t)((address_byte >> 1u) & DALI_MAX_SHORT_ADDRESS);
    }
    if (kind == DALI_EVENT_ADDRESS_GROUP) {
        return (uint8_t)((address_byte >> 1u) & DALI_MAX_GROUP);
    }
    return 0u;
}

DaliError dali_event_parse_frame(const DaliFrame *frame, DaliInputEvent *out)
{
    if (frame == NULL || out == NULL ||
        (frame->bit_length != DALI_FORWARD_FRAME_BITS &&
         frame->bit_length != DALI_EXTENDED_FRAME_BITS)) {
        return DALI_ERR_INVALID;
    }

    uint8_t address_byte = frame->bit_length == DALI_EXTENDED_FRAME_BITS
                         ? (uint8_t)((frame->data >> 16u) & 0xFFu)
                         : (uint8_t)((frame->data >> 8u) & 0xFFu);
    uint8_t instance = frame->bit_length == DALI_EXTENDED_FRAME_BITS
                     ? (uint8_t)((frame->data >> 8u) & 0xFFu)
                     : 0xFFu;
    uint8_t event_code = (uint8_t)(frame->data & 0xFFu);
    DaliEventAddressKind kind = decode_address_kind(address_byte);

    *out = (DaliInputEvent){
        .frame_kind       = frame->bit_length == DALI_EXTENDED_FRAME_BITS
                          ? DALI_EVENT_FRAME_INPUT_24BIT
                          : DALI_EVENT_FRAME_LEGACY_16BIT,
        .raw              = *frame,
        .address_byte     = address_byte,
        .address_kind     = kind,
        .address          = decode_address(address_byte, kind),
        .address_selector = (address_byte & 0x01u) != 0u,
        .has_instance     = frame->bit_length == DALI_EXTENDED_FRAME_BITS,
        .instance         = instance,
        .event_code       = event_code,
    };
    return DALI_OK;
}

const char *dali_event_frame_kind_name(DaliEventFrameKind kind)
{
    switch (kind) {
        case DALI_EVENT_FRAME_LEGACY_16BIT:
            return "legacy-16bit";
        case DALI_EVENT_FRAME_INPUT_24BIT:
            return "input-24bit";
        case DALI_EVENT_FRAME_INVALID:
        default:
            return "invalid";
    }
}

const char *dali_event_address_kind_name(DaliEventAddressKind kind)
{
    switch (kind) {
        case DALI_EVENT_ADDRESS_SHORT:
            return "short";
        case DALI_EVENT_ADDRESS_GROUP:
            return "group";
        case DALI_EVENT_ADDRESS_BROADCAST:
            return "broadcast";
        case DALI_EVENT_ADDRESS_INVALID:
        default:
            return "invalid";
    }
}

static const char *legacy_command_name(uint8_t opcode)
{
    switch (opcode) {
        case 0x00u:
            return "off";
        case 0x01u:
            return "up";
        case 0x02u:
            return "down";
        case 0x03u:
            return "step-up";
        case 0x04u:
            return "step-down";
        case 0x05u:
            return "recall-max";
        case 0x06u:
            return "recall-min";
        case 0x07u:
            return "step-down-and-off";
        case 0x08u:
            return "on-and-step-up";
        case 0x09u:
            return "enable-dapc-sequence";
        case 0x0Au:
            return "go-to-last-active-level";
        default:
            if (opcode >= 0x10u && opcode <= 0x1Fu) {
                return "go-to-scene";
            }
            return "command";
    }
}

const char *dali_event_code_name(uint8_t event_code)
{
    switch (event_code) {
        case 0x00u:
            return "button-released";
        case 0x01u:
            return "button-pressed";
        case 0x02u:
            return "short-press";
        case 0x03u:
            return "double-press";
        case 0x04u:
            return "long-press-start";
        case 0x05u:
            return "long-press-repeat";
        case 0x06u:
            return "long-press-stop";
        case 0x07u:
            return "button-free";
        case 0x08u:
            return "button-stuck";
        default:
            return "unknown";
    }
}

const char *dali_event_action_name(const DaliInputEvent *event)
{
    if (event == NULL) {
        return "unknown";
    }

    if (event->frame_kind == DALI_EVENT_FRAME_INPUT_24BIT) {
        return dali_event_code_name(event->event_code);
    }

    if (event->frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT) {
        return event->address_selector
             ? legacy_command_name(event->event_code)
             : "dapc-level";
    }

    return "unknown";
}

bool dali_event_is_push_button_double_press(const DaliInputEvent *event)
{
    return event != NULL &&
           event->frame_kind == DALI_EVENT_FRAME_INPUT_24BIT &&
           event->event_code == 0x03u;
}

bool dali_event_is_switch_mapping_candidate(const DaliInputEvent *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->frame_kind == DALI_EVENT_FRAME_INPUT_24BIT) {
        return dali_event_is_push_button_double_press(event);
    }
    return event->frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT;
}

DaliError dali_event_queue_init(DaliInputEventQueue *queue)
{
    if (queue == NULL) {
        return DALI_ERR_INVALID;
    }
    memset(queue, 0, sizeof(*queue));
    return DALI_OK;
}

DaliError dali_event_queue_push(DaliInputEventQueue *queue,
                                const DaliInputEventRecord *record)
{
    if (queue == NULL || record == NULL) {
        return DALI_ERR_INVALID;
    }

    if (queue->count >= DALI_EVENT_QUEUE_CAPACITY) {
        queue->dropped++;
        return DALI_ERR_OVERFLOW;
    }

    queue->records[queue->tail] = *record;
    queue->tail = (uint8_t)((queue->tail + 1u) % DALI_EVENT_QUEUE_CAPACITY);
    queue->count++;
    return DALI_OK;
}

bool dali_event_queue_pop(DaliInputEventQueue *queue, DaliInputEventRecord *out)
{
    if (queue == NULL || out == NULL || queue->count == 0u) {
        return false;
    }

    *out = queue->records[queue->head];
    queue->head = (uint8_t)((queue->head + 1u) % DALI_EVENT_QUEUE_CAPACITY);
    queue->count--;
    return true;
}

uint8_t dali_event_queue_count(const DaliInputEventQueue *queue)
{
    return queue == NULL ? 0u : queue->count;
}

uint32_t dali_event_queue_dropped(const DaliInputEventQueue *queue)
{
    return queue == NULL ? 0u : queue->dropped;
}
