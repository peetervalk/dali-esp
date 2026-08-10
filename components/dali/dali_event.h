#pragma once

/*
 * dali_event.h - DALI raw input/controller event parsing and fixed queues
 *
 * Event frames are kept raw-first. Legacy/DALI-1 push-button couplers often
 * behave as controllers and place normal 16-bit gear commands on the bus;
 * those retain the legacy target/data fields below.
 *
 * IEC 62386-103 input notifications are not a three-byte
 * address/instance/event split. Bit 16 distinguishes an event (0) from a
 * command (1), the encoded source pattern selects one of five source schemes,
 * and bits 9:0 carry event information. Bit 22 is a scheme discriminator only
 * when bit 23 is set; otherwise it is the device short-address MSB. The source
 * is represented independently so no consumer has to reinterpret raw bytes.
 *
 * Real-bus captures should remain the authority for vendor quirks.
 */

#include "dali_frame.h"

#define DALI_EVENT_QUEUE_CAPACITY 32u

#define DALI_DT301_INSTANCE_TYPE          1u
#define DALI_DT301_EVENT_BUTTON_RELEASED  0x000u
#define DALI_DT301_EVENT_BUTTON_PRESSED   0x001u
#define DALI_DT301_EVENT_SHORT_PRESS      0x002u
#define DALI_DT301_EVENT_DOUBLE_PRESS     0x005u
#define DALI_DT301_EVENT_LONG_PRESS_START 0x009u
#define DALI_DT301_EVENT_LONG_PRESS_REPEAT 0x00Bu
#define DALI_DT301_EVENT_LONG_PRESS_STOP  0x00Cu
#define DALI_DT301_EVENT_BUTTON_FREE      0x00Eu
#define DALI_DT301_EVENT_BUTTON_STUCK     0x00Fu

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
    DALI_EVENT_FRAME_POWER_NOTIFICATION_24BIT,
} DaliEventFrameKind;

/* Values 0..4 match the Part-103 eventScheme configuration values. */
typedef enum {
    DALI_EVENT_SOURCE_INSTANCE       = 0,
    DALI_EVENT_SOURCE_DEVICE         = 1,
    DALI_EVENT_SOURCE_DEVICE_INSTANCE = 2,
    DALI_EVENT_SOURCE_DEVICE_GROUP   = 3,
    DALI_EVENT_SOURCE_INSTANCE_GROUP = 4,
    DALI_EVENT_SOURCE_POWER_NOTIFICATION = 5,
    DALI_EVENT_SOURCE_NONE           = 0xFF,
} DaliEventSourceScheme;

typedef struct {
    DaliEventSourceScheme scheme;
    bool                  has_device_address;
    uint8_t               device_address;
    bool                  has_device_group;
    uint8_t               device_group;
    bool                  has_instance;
    uint8_t               instance;
    bool                  has_instance_group;
    uint8_t               instance_group;
    bool                  has_instance_type;
    uint8_t               instance_type;
} DaliEventSource;

typedef struct {
    DaliEventFrameKind    frame_kind;
    DaliFrame            raw;
    /* Legacy 16-bit target/data fields. Invalid/zero for Part-103 events. */
    uint8_t              address_byte;
    DaliEventAddressKind address_kind;
    uint8_t              address;
    bool                 address_selector;
    uint8_t              legacy_data;
    /* Canonical Part-103 source and 10-bit event information. */
    DaliEventSource      source;
    uint16_t             event_information;
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
const char *dali_event_source_scheme_name(DaliEventSourceScheme scheme);
const char *dali_dt301_event_name(uint16_t event_information);
const char *dali_event_action_name(const DaliInputEvent *event);
bool dali_event_is_push_button_double_press(const DaliInputEvent *event);
bool dali_event_is_switch_mapping_candidate(const DaliInputEvent *event);

DaliError dali_event_queue_init(DaliInputEventQueue *queue);
DaliError dali_event_queue_push(DaliInputEventQueue *queue,
                                const DaliInputEventRecord *record);
bool dali_event_queue_pop(DaliInputEventQueue *queue, DaliInputEventRecord *out);
uint8_t dali_event_queue_count(const DaliInputEventQueue *queue);
uint32_t dali_event_queue_dropped(const DaliInputEventQueue *queue);
