#include "dali_input_config.h"

/* IEC 62386-103:2022 common instance configuration opcodes. */
#define INPUT_SET_EVENT_PRIORITY          0x61u
#define INPUT_ENABLE_INSTANCE             0x62u
#define INPUT_DISABLE_INSTANCE            0x63u
#define INPUT_SET_PRIMARY_INSTANCE_GROUP  0x64u
#define INPUT_SET_INSTANCE_GROUP_1        0x65u
#define INPUT_SET_INSTANCE_GROUP_2        0x66u
#define INPUT_SET_EVENT_SCHEME            0x67u
#define INPUT_SET_EVENT_FILTER            0x68u
#define INPUT_SET_INSTANCE_TYPE           0x69u
#define INPUT_SET_INSTANCE_CONFIGURATION  0x6Au

/* IEC 62386-301:2017, push button / binary input, instance type 1. */
#define PB_SET_SHORT_TIMER         0x00u
#define PB_SET_DOUBLE_TIMER        0x01u
#define PB_SET_REPEAT_TIMER        0x02u
#define PB_SET_STUCK_TIMER         0x03u
#define PB_QUERY_SHORT_TIMER       0x0Au
#define PB_QUERY_SHORT_TIMER_MIN   0x0Bu
#define PB_QUERY_DOUBLE_TIMER      0x0Cu
#define PB_QUERY_DOUBLE_TIMER_MIN  0x0Du
#define PB_QUERY_REPEAT_TIMER      0x0Eu
#define PB_QUERY_STUCK_TIMER       0x0Fu

/* IEC 62386-303:2017 + Amendment 1:2024, occupancy, instance type 3. */
#define OCC_CATCH_MOVEMENT          0x20u
#define OCC_SET_HOLD_TIMER          0x21u
#define OCC_SET_REPORT_TIMER        0x22u
#define OCC_SET_DEADTIME_TIMER      0x23u
#define OCC_CANCEL_HOLD_TIMER       0x24u
#define OCC_SET_DETECTION_RANGE     0x25u
#define OCC_SET_SENSITIVITY         0x26u
#define OCC_QUERY_CAPABILITIES      0x29u
#define OCC_QUERY_DETECTION_RANGE   0x2Au
#define OCC_QUERY_SENSITIVITY       0x2Bu
#define OCC_QUERY_DEADTIME_TIMER    0x2Cu
#define OCC_QUERY_HOLD_TIMER        0x2Du
#define OCC_QUERY_REPORT_TIMER      0x2Eu
#define OCC_QUERY_CATCHING          0x2Fu

/* IEC 62386-304:2017 + Amendment 1:2024, light sensor, instance type 4. */
#define LIGHT_SET_REPORT_TIMER       0x30u
#define LIGHT_SET_HYSTERESIS         0x31u
#define LIGHT_SET_DEADTIME_TIMER     0x32u
#define LIGHT_SET_HYSTERESIS_MIN     0x33u
#define LIGHT_QUERY_HYSTERESIS_MIN   0x3Cu
#define LIGHT_QUERY_DEADTIME_TIMER   0x3Du
#define LIGHT_QUERY_REPORT_TIMER     0x3Eu
#define LIGHT_QUERY_HYSTERESIS       0x3Fu

DaliError dali_input_build_config_sequence(DaliFrame      command,
                                           bool           send_twice,
                                           bool           expects_reply,
                                           const uint8_t *dtr,
                                           uint8_t        dtr_count,
                                           DaliSequence  *out)
{
    if (out == NULL ||
        command.bit_length != DALI_EXTENDED_FRAME_BITS ||
        dtr_count > DALI_INPUT_CONFIG_MAX_DTR_BYTES ||
        (dtr_count > 0u && dtr == NULL)) {
        return DALI_ERR_INVALID;
    }

    *out = (DaliSequence){0};

    uint8_t step = 0u;
    for (uint8_t i = 0u; i < dtr_count; i++) {
        DaliFrame frame;
        DaliError err = dali_build_control_device_dtr_data((DaliDtrRegister)i,
                                                           dtr[i],
                                                           &frame);
        if (err != DALI_OK) {
            return err;
        }
        out->steps[step++] = (DaliSequenceStep){ .frame = frame };
    }

    out->steps[step++] = (DaliSequenceStep){
        .frame       = command,
        .needs_reply = expects_reply,
        .send_twice  = send_twice,
    };

    out->step_count = step;
    return DALI_OK;
}

/* Common IEC 62386-103 instance configuration. */

DaliFrame dali_input_build_set_event_priority(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INPUT_SET_EVENT_PRIORITY);
}

DaliFrame dali_input_build_enable_instance(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INPUT_ENABLE_INSTANCE);
}

DaliFrame dali_input_build_disable_instance(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INPUT_DISABLE_INSTANCE);
}

DaliFrame dali_input_build_set_primary_group(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INPUT_SET_PRIMARY_INSTANCE_GROUP);
}

DaliFrame dali_input_build_set_instance_group1(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INPUT_SET_INSTANCE_GROUP_1);
}

DaliFrame dali_input_build_set_instance_group2(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INPUT_SET_INSTANCE_GROUP_2);
}

DaliFrame dali_input_build_set_event_scheme(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INPUT_SET_EVENT_SCHEME);
}

DaliFrame dali_input_build_set_event_filter(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INPUT_SET_EVENT_FILTER);
}

DaliFrame dali_input_build_set_instance_type(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INPUT_SET_INSTANCE_TYPE);
}

DaliFrame dali_input_build_set_instance_configuration(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INPUT_SET_INSTANCE_CONFIGURATION);
}

/* Part 301 push button / binary input, instance type 1. */

DaliFrame dali_input_pb_build_set_short_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_SET_SHORT_TIMER);
}

DaliFrame dali_input_pb_build_set_double_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_SET_DOUBLE_TIMER);
}

DaliFrame dali_input_pb_build_set_repeat_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_SET_REPEAT_TIMER);
}

DaliFrame dali_input_pb_build_set_stuck_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_SET_STUCK_TIMER);
}

DaliFrame dali_input_pb_build_query_short_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_QUERY_SHORT_TIMER);
}

DaliFrame dali_input_pb_build_query_short_timer_min(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_QUERY_SHORT_TIMER_MIN);
}

DaliFrame dali_input_pb_build_query_double_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_QUERY_DOUBLE_TIMER);
}

DaliFrame dali_input_pb_build_query_double_timer_min(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_QUERY_DOUBLE_TIMER_MIN);
}

DaliFrame dali_input_pb_build_query_repeat_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_QUERY_REPEAT_TIMER);
}

DaliFrame dali_input_pb_build_query_stuck_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_QUERY_STUCK_TIMER);
}

/* Part 303 occupancy sensor, instance type 3. */

DaliFrame dali_input_occ_build_catch_movement(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_CATCH_MOVEMENT);
}

DaliFrame dali_input_occ_build_set_hold_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_SET_HOLD_TIMER);
}

DaliFrame dali_input_occ_build_set_report_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_SET_REPORT_TIMER);
}

DaliFrame dali_input_occ_build_set_deadtime(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_SET_DEADTIME_TIMER);
}

DaliFrame dali_input_occ_build_cancel_hold_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_CANCEL_HOLD_TIMER);
}

DaliFrame dali_input_occ_build_set_detection_range(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_SET_DETECTION_RANGE);
}

DaliFrame dali_input_occ_build_set_sensitivity(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_SET_SENSITIVITY);
}

DaliFrame dali_input_occ_build_query_capabilities(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_QUERY_CAPABILITIES);
}

DaliFrame dali_input_occ_build_query_detection_range(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_QUERY_DETECTION_RANGE);
}

DaliFrame dali_input_occ_build_query_sensitivity(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_QUERY_SENSITIVITY);
}

DaliFrame dali_input_occ_build_query_deadtime(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_QUERY_DEADTIME_TIMER);
}

DaliFrame dali_input_occ_build_query_hold_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_QUERY_HOLD_TIMER);
}

DaliFrame dali_input_occ_build_query_report_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_QUERY_REPORT_TIMER);
}

DaliFrame dali_input_occ_build_query_catching(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_QUERY_CATCHING);
}

/* Part 304 light sensor, instance type 4. */

DaliFrame dali_input_light_build_set_report_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_SET_REPORT_TIMER);
}

DaliFrame dali_input_light_build_set_hysteresis(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_SET_HYSTERESIS);
}

DaliFrame dali_input_light_build_set_deadtime(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_SET_DEADTIME_TIMER);
}

DaliFrame dali_input_light_build_set_hysteresis_min(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_SET_HYSTERESIS_MIN);
}

DaliFrame dali_input_light_build_query_hysteresis_min(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_QUERY_HYSTERESIS_MIN);
}

DaliFrame dali_input_light_build_query_deadtime(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_QUERY_DEADTIME_TIMER);
}

DaliFrame dali_input_light_build_query_report_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_QUERY_REPORT_TIMER);
}

DaliFrame dali_input_light_build_query_hysteresis(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_QUERY_HYSTERESIS);
}
