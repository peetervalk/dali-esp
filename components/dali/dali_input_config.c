#include "dali_input_config.h"

/* ---------------------------------------------------------------------------
 * Generic IEC 62386-103 instance configuration opcodes (Table 15)
 * --------------------------------------------------------------------------*/
#define INST_CFG_SET_EVENT_PRIORITY   0x61u
#define INST_CFG_ENABLE_INSTANCE      0x62u
#define INST_CFG_DISABLE_INSTANCE     0x63u
#define INST_CFG_SET_PRIMARY_GROUP    0x64u
#define INST_CFG_SET_INSTANCE_GROUP1  0x65u
#define INST_CFG_SET_INSTANCE_GROUP2  0x66u
#define INST_CFG_SET_EVENT_SCHEME     0x67u
#define INST_CFG_SET_EVENT_FILTER     0x68u
#define INST_CFG_SET_REPORT_TIMER     0x6Eu
#define INST_CFG_SET_HYSTERESIS       0x6Fu
#define INST_CFG_SET_DEADTIME_TIMER   0x70u

/* ---------------------------------------------------------------------------
 * DT301 push-button type-specific opcodes (IEC 62386-301)
 * --------------------------------------------------------------------------*/
#define PB_SET_HYSTERESIS             0xE0u
#define PB_SET_DEADTIME               0xE1u
#define PB_SET_DOUBLE_CLICK_PRIORITY  0xE2u
#define PB_SET_REPEAT_PERIOD          0xE3u
#define PB_SET_HOLD_TIMER             0xE4u

/* ---------------------------------------------------------------------------
 * DT303 occupancy sensor type-specific opcodes (IEC 62386-303)
 * --------------------------------------------------------------------------*/
#define OCC_SET_HOLD_TIMER            0x21u
#define OCC_SET_REPORT_TIMER          0x22u
#define OCC_SET_DEADTIME              0x23u

/* ---------------------------------------------------------------------------
 * DT304 light sensor type-specific opcodes (IEC 62386-304)
 * --------------------------------------------------------------------------*/
#define LIGHT_SET_HYSTERESIS          0xE0u
#define LIGHT_SET_DEADBAND            0xE1u

/* ---------------------------------------------------------------------------
 * Generic IEC 62386-103 instance query opcodes (Table 16)
 * Single send, expects 8-bit reply.
 * 0x80 (instance_type) and 0x81 (resolution) are owned by dali_input_device.c.
 * --------------------------------------------------------------------------*/
#define INST_QUERY_HYSTERESIS         0x82u
#define INST_QUERY_DEADTIME_TIMER     0x83u
#define INST_QUERY_REPORT_TIMER       0x84u

/* DT303 type-specific query opcodes (IEC 62386-303, Table 7) */
#define OCC_QUERY_DEADTIME            0x2Cu
#define OCC_QUERY_HOLD_TIMER          0x2Du
#define OCC_QUERY_REPORT_TIMER        0x2Eu

/* DT304 type-specific query opcodes (IEC 62386-304, Table 7) */
#define LIGHT_QUERY_HYSTERESIS        0xC0u
#define LIGHT_QUERY_DEADBAND          0xC1u

/* ---------------------------------------------------------------------------
 * Generic config builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_input_build_set_event_priority(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_SET_EVENT_PRIORITY);
}

DaliFrame dali_input_build_enable_instance(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_ENABLE_INSTANCE);
}

DaliFrame dali_input_build_disable_instance(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_DISABLE_INSTANCE);
}

DaliFrame dali_input_build_set_primary_group(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_SET_PRIMARY_GROUP);
}

DaliFrame dali_input_build_set_instance_group1(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_SET_INSTANCE_GROUP1);
}

DaliFrame dali_input_build_set_instance_group2(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_SET_INSTANCE_GROUP2);
}

DaliFrame dali_input_build_set_event_scheme(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_SET_EVENT_SCHEME);
}

DaliFrame dali_input_build_set_event_filter(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_SET_EVENT_FILTER);
}

DaliFrame dali_input_build_set_report_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_SET_REPORT_TIMER);
}

DaliFrame dali_input_build_set_hysteresis(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_SET_HYSTERESIS);
}

DaliFrame dali_input_build_set_deadtime_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_CFG_SET_DEADTIME_TIMER);
}

/* ---------------------------------------------------------------------------
 * DT301 push-button builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_input_pb_build_set_hysteresis(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_SET_HYSTERESIS);
}

DaliFrame dali_input_pb_build_set_deadtime(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_SET_DEADTIME);
}

DaliFrame dali_input_pb_build_set_double_click_priority(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_SET_DOUBLE_CLICK_PRIORITY);
}

DaliFrame dali_input_pb_build_set_repeat_period(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_SET_REPEAT_PERIOD);
}

DaliFrame dali_input_pb_build_set_hold_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, PB_SET_HOLD_TIMER);
}

/* ---------------------------------------------------------------------------
 * DT303 occupancy builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_input_occ_build_set_deadtime(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_SET_DEADTIME);
}

DaliFrame dali_input_occ_build_set_report_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_SET_REPORT_TIMER);
}

DaliFrame dali_input_occ_build_set_hold_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_SET_HOLD_TIMER);
}

/* ---------------------------------------------------------------------------
 * DT304 light sensor builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_input_light_build_set_hysteresis(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_SET_HYSTERESIS);
}

DaliFrame dali_input_light_build_set_deadband(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_SET_DEADBAND);
}

/* ---------------------------------------------------------------------------
 * Generic IEC 62386-103 instance query builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_input_build_query_hysteresis(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_QUERY_HYSTERESIS);
}

DaliFrame dali_input_build_query_deadtime_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_QUERY_DEADTIME_TIMER);
}

DaliFrame dali_input_build_query_report_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, INST_QUERY_REPORT_TIMER);
}

/* ---------------------------------------------------------------------------
 * DT303 occupancy sensor query builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_input_occ_build_query_deadtime(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_QUERY_DEADTIME);
}

DaliFrame dali_input_occ_build_query_hold_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_QUERY_HOLD_TIMER);
}

DaliFrame dali_input_occ_build_query_report_timer(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, OCC_QUERY_REPORT_TIMER);
}

/* ---------------------------------------------------------------------------
 * DT304 light sensor query builders
 * --------------------------------------------------------------------------*/

DaliFrame dali_input_light_build_query_hysteresis(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_QUERY_HYSTERESIS);
}

DaliFrame dali_input_light_build_query_deadband(uint8_t addr, uint8_t instance)
{
    return dali_cmd_instance(addr, instance, LIGHT_QUERY_DEADBAND);
}

/* ---------------------------------------------------------------------------
 * Generic IEC 62386-103 instance state query builders (protocol-table backed)
 * --------------------------------------------------------------------------*/

