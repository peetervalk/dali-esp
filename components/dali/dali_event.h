#pragma once

/*
 * dali_event.h - DALI raw input/controller event parsing and fixed queues
 *
 * Event frames are kept raw-first.  DALI-2 input-event frames are decoded from
 * the same 24-bit byte split used by DALI-2 input-device frames:
 *
 *   data = (address_byte << 16) | (instance_byte << 8) | event_byte
 *
 * Legacy/DALI-1 push-button couplers often behave as controllers and place
 * normal 16-bit DALI commands on the bus.  Those frames do not carry a source
 * instance, so the parser exposes target/action details and leaves
 * has_instance false.
 *
 * Real-bus captures should remain the authority for vendor quirks.
 */

#include "dali_frame.h"

#define DALI_EVENT_QUEUE_CAPACITY 32u

typedef enum {
    DALI_EVENT_ADDRESS_INVALID = 0,
    DALI_EVENT_ADDRESS_SHORT,
    DALI_EVENT_ADDRESS_GROUP,
    DALI_EVENT_ADDRESS_BROADCAST,
} DaliEventAddressKind;

typedef enum {
    DALI_EVENT_FRAME_INVALID = 0,
    DALI_EVENT_FRAME_LEGACY_16BIT,
    DALI_EVENT_FRAME_INPUT_24BIT,
} DaliEventFrameKind;

typedef struct {
    DaliEventFrameKind    frame_kind;
    DaliFrame            raw;
    uint8_t              address_byte;
    DaliEventAddressKind address_kind;
    uint8_t              address;
    bool                 address_selector;
    bool                 has_instance;
    uint8_t              instance;
    uint8_t              event_code;
} DaliInputEvent;

typedef struct {
    DaliInputEvent event;
    uint32_t       timestamp_us;
} DaliInputEventRecord;

typedef struct {
    DaliInputEventRecord records[DALI_EVENT_QUEUE_CAPACITY];
    uint8_t              head;
    uint8_t              tail;
    uint8_t              count;
    uint32_t             dropped;
} DaliInputEventQueue;

DaliError dali_event_parse_frame(const DaliFrame *frame, DaliInputEvent *out);

const char *dali_event_frame_kind_name(DaliEventFrameKind kind);
const char *dali_event_address_kind_name(DaliEventAddressKind kind);
const char *dali_event_code_name(uint8_t event_code);
const char *dali_event_action_name(const DaliInputEvent *event);
bool dali_event_is_push_button_double_press(const DaliInputEvent *event);
bool dali_event_is_switch_mapping_candidate(const DaliInputEvent *event);

DaliError dali_event_queue_init(DaliInputEventQueue *queue);
DaliError dali_event_queue_push(DaliInputEventQueue *queue,
                                const DaliInputEventRecord *record);
bool dali_event_queue_pop(DaliInputEventQueue *queue, DaliInputEventRecord *out);
uint8_t dali_event_queue_count(const DaliInputEventQueue *queue);
uint32_t dali_event_queue_dropped(const DaliInputEventQueue *queue);
