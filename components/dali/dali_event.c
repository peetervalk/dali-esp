#include "dali_event.h"

#include <string.h>

static DaliEventAddressKind decode_address_kind(uint8_t address_byte)
{
    if (address_byte == DALI_BROADCAST_DAPC_ADDRESS ||
        address_byte == DALI_BROADCAST_COMMAND_ADDRESS) {
        return DALI_EVENT_ADDRESS_BROADCAST;
    }
    if (address_byte <= 0x7Fu) {
        return DALI_EVENT_ADDRESS_SHORT;
    }
    if (address_byte <= 0x9Fu) {
        return DALI_EVENT_ADDRESS_GROUP;
    }
    return DALI_EVENT_ADDRESS_INVALID;
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
    if ((frame->bit_length == DALI_FORWARD_FRAME_BITS &&
         (frame->data & ~0xFFFFu) != 0u) ||
        (frame->bit_length == DALI_EXTENDED_FRAME_BITS &&
         (frame->data & ~0xFFFFFFu) != 0u)) {
        return DALI_ERR_INVALID;
    }

    *out = (DaliInputEvent){
        .raw = *frame,
        .source = {
            .scheme = DALI_EVENT_SOURCE_NONE,
        },
    };

    if (frame->bit_length == DALI_FORWARD_FRAME_BITS) {
        uint8_t address_byte = (uint8_t)((frame->data >> 8u) & 0xFFu);
        DaliEventAddressKind kind = decode_address_kind(address_byte);
        if (kind == DALI_EVENT_ADDRESS_INVALID) {
            return DALI_ERR_INVALID;
        }

        out->frame_kind       = DALI_EVENT_FRAME_LEGACY_16BIT;
        out->address_byte     = address_byte;
        out->address_kind     = kind;
        out->address          = decode_address(address_byte, kind);
        out->address_selector = (address_byte & 0x01u) != 0u;
        out->legacy_data      = (uint8_t)(frame->data & 0xFFu);
        return DALI_OK;
    }

    uint32_t raw = frame->data;

    /* Part 103 bit 16 distinguishes input events (0) from commands (1). */
    if ((raw & 0x010000u) != 0u) {
        return DALI_ERR_INVALID;
    }

    /* The reserved 111 source pattern carries power-cycle notifications. */
    if ((raw & 0xFFE000u) == 0xFEE000u) {
        bool has_group = (raw & 0x001000u) != 0u;
        bool has_address = (raw & 0x000040u) != 0u;
        uint8_t group = (uint8_t)((raw >> 7u) & 0x1Fu);
        uint8_t address = (uint8_t)(raw & 0x3Fu);

        /* Payload fields must be zero when their validity bit is clear. */
        if ((!has_group && group != 0u) ||
            (!has_address && address != 0u)) {
            return DALI_ERR_INVALID;
        }

        out->frame_kind = DALI_EVENT_FRAME_POWER_NOTIFICATION_24BIT;
        out->source.scheme = DALI_EVENT_SOURCE_POWER_NOTIFICATION;
        out->source.has_device_group = has_group;
        out->source.device_group = has_group ? group : 0u;
        out->source.has_device_address = has_address;
        out->source.device_address = has_address ? address : 0u;
        return DALI_OK;
    }

    bool bit_23 = (raw & 0x800000u) != 0u;
    bool bit_22 = (raw & 0x400000u) != 0u;
    bool bit_15 = (raw & 0x008000u) != 0u;

    out->frame_kind = DALI_EVENT_FRAME_INPUT_24BIT;
    out->event_information = (uint16_t)(raw & 0x03FFu);

    if (!bit_23 && !bit_15) {
        out->source.scheme = DALI_EVENT_SOURCE_DEVICE;
        out->source.has_device_address = true;
        out->source.device_address = (uint8_t)((raw >> 17u) & 0x3Fu);
        out->source.has_instance_type = true;
        out->source.instance_type = (uint8_t)((raw >> 10u) & 0x1Fu);
    } else if (!bit_23 && bit_15) {
        out->source.scheme = DALI_EVENT_SOURCE_DEVICE_INSTANCE;
        out->source.has_device_address = true;
        out->source.device_address = (uint8_t)((raw >> 17u) & 0x3Fu);
        out->source.has_instance = true;
        out->source.instance = (uint8_t)((raw >> 10u) & 0x1Fu);
    } else if (bit_23 && !bit_22 && !bit_15) {
        out->source.scheme = DALI_EVENT_SOURCE_DEVICE_GROUP;
        out->source.has_device_group = true;
        out->source.device_group = (uint8_t)((raw >> 17u) & 0x1Fu);
        out->source.has_instance_type = true;
        out->source.instance_type = (uint8_t)((raw >> 10u) & 0x1Fu);
    } else if (bit_23 && !bit_22 && bit_15) {
        out->source.scheme = DALI_EVENT_SOURCE_INSTANCE;
        out->source.has_instance_type = true;
        out->source.instance_type = (uint8_t)((raw >> 17u) & 0x1Fu);
        out->source.has_instance = true;
        out->source.instance = (uint8_t)((raw >> 10u) & 0x1Fu);
    } else if (bit_23 && bit_22 && !bit_15) {
        out->source.scheme = DALI_EVENT_SOURCE_INSTANCE_GROUP;
        out->source.has_instance_group = true;
        out->source.instance_group = (uint8_t)((raw >> 17u) & 0x1Fu);
        out->source.has_instance_type = true;
        out->source.instance_type = (uint8_t)((raw >> 10u) & 0x1Fu);
    } else {
        /* 111 is reserved here; power notifications were handled above. */
        return DALI_ERR_INVALID;
    }

    return DALI_OK;
}

const char *dali_event_frame_kind_name(DaliEventFrameKind kind)
{
    switch (kind) {
        case DALI_EVENT_FRAME_LEGACY_16BIT:
            return "legacy-16bit";
        case DALI_EVENT_FRAME_INPUT_24BIT:
            return "input-24bit";
        case DALI_EVENT_FRAME_POWER_NOTIFICATION_24BIT:
            return "power-notification-24bit";
        case DALI_EVENT_FRAME_INVALID:
        default:
            return "invalid";
    }
}

const char *dali_event_source_scheme_name(DaliEventSourceScheme scheme)
{
    switch (scheme) {
        case DALI_EVENT_SOURCE_INSTANCE:
            return "instance";
        case DALI_EVENT_SOURCE_DEVICE:
            return "device";
        case DALI_EVENT_SOURCE_DEVICE_INSTANCE:
            return "device-instance";
        case DALI_EVENT_SOURCE_DEVICE_GROUP:
            return "device-group";
        case DALI_EVENT_SOURCE_INSTANCE_GROUP:
            return "instance-group";
        case DALI_EVENT_SOURCE_POWER_NOTIFICATION:
            return "power-notification";
        case DALI_EVENT_SOURCE_NONE:
        default:
            return "none";
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

const char *dali_dt301_event_name(uint16_t event_information)
{
    switch (event_information) {
        case DALI_DT301_EVENT_BUTTON_RELEASED:
            return "button-released";
        case DALI_DT301_EVENT_BUTTON_PRESSED:
            return "button-pressed";
        case DALI_DT301_EVENT_SHORT_PRESS:
            return "short-press";
        case DALI_DT301_EVENT_DOUBLE_PRESS:
            return "double-press";
        case DALI_DT301_EVENT_LONG_PRESS_START:
            return "long-press-start";
        case DALI_DT301_EVENT_LONG_PRESS_REPEAT:
            return "long-press-repeat";
        case DALI_DT301_EVENT_LONG_PRESS_STOP:
            return "long-press-stop";
        case DALI_DT301_EVENT_BUTTON_FREE:
            return "button-free";
        case DALI_DT301_EVENT_BUTTON_STUCK:
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
        if (event->source.has_instance_type &&
            event->source.instance_type == DALI_DT301_INSTANCE_TYPE) {
            return dali_dt301_event_name(event->event_information);
        }
        return "event";
    }

    if (event->frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT) {
        return event->address_selector
             ? legacy_command_name(event->legacy_data)
             : "dapc-level";
    }

    if (event->frame_kind == DALI_EVENT_FRAME_POWER_NOTIFICATION_24BIT) {
        return "power-notification";
    }

    return "unknown";
}

bool dali_event_is_push_button_double_press(const DaliInputEvent *event)
{
    return event != NULL &&
           event->frame_kind == DALI_EVENT_FRAME_INPUT_24BIT &&
           event->source.has_instance_type &&
           event->source.instance_type == DALI_DT301_INSTANCE_TYPE &&
           event->event_information == DALI_DT301_EVENT_DOUBLE_PRESS;
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
