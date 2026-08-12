#include "dali_control.h"
#include "dali_protocol.h"

#include <stddef.h>

#define HA_BRIGHTNESS_MAX       255u
#define HA_BRIGHTNESS_ROUNDING  128u
#define PERCENT_MAX             100u

static DaliError enqueue_frame_with_retries(const DaliFrame *frame,
                                            bool needs_reply,
                                            bool send_twice,
                                            uint8_t retries_left,
                                            DaliSchedCompletionCb cb,
                                            void *cb_ctx)
{
    if (frame == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliTransaction txn = {
        .frame        = *frame,
        .needs_reply  = needs_reply,
        .send_twice   = send_twice,
        .retries_left = needs_reply ? retries_left : 0u,
        .on_complete  = cb,
        .cb_ctx       = cb_ctx,
    };
    return dali_sched_enqueue(&txn);
}

static DaliError enqueue_frame(const DaliFrame *frame,
                               bool needs_reply,
                               bool send_twice,
                               DaliSchedCompletionCb cb,
                               void *cb_ctx)
{
    return enqueue_frame_with_retries(frame,
                                      needs_reply,
                                      send_twice,
                                      needs_reply ? DALI_MAX_RETRIES : 0u,
                                      cb,
                                      cb_ctx);
}

static DaliError build_target_command(DaliTarget target,
                                      DaliCommandId id,
                                      uint8_t param,
                                      DaliFrame *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliError err = dali_control_validate_target(target);
    if (err != DALI_OK) {
        return err;
    }

    return dali_build_command(target.type, target.address, id, param, out);
}

static DaliError enqueue_target_command_cb(DaliTarget target,
                                           DaliCommandId id,
                                           uint8_t param,
                                           DaliSchedCompletionCb cb,
                                           void *cb_ctx)
{
    DaliFrame frame;
    DaliError err = build_target_command(target, id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return enqueue_frame(&frame, false, false, cb, cb_ctx);
}

static DaliError enqueue_target_command(DaliTarget target,
                                        DaliCommandId id,
                                        uint8_t param)
{
    return enqueue_target_command_cb(target, id, param, NULL, NULL);
}

static DaliError enqueue_config_command(DaliTarget target,
                                        DaliCommandId id,
                                        uint8_t param)
{
    const DaliCommandInfo *cmd = dali_command_lookup(id);
    DaliFrame frame;
    DaliError err = dali_control_build_config(target, id, param, &frame);
    if (err != DALI_OK || cmd == NULL) {
        return err;
    }
    return enqueue_frame(&frame, false, cmd->send_twice, NULL, NULL);
}

static DaliError validate_addressed_query_command(DaliCommandId id)
{
    const DaliCommandInfo *cmd = dali_command_lookup(id);
    if (cmd == NULL ||
        cmd->frame_kind != DALI_CMD_FRAME_16BIT ||
        cmd->response_kind == DALI_RESP_NONE) {
        return DALI_ERR_INVALID;
    }
    return DALI_OK;
}

static DaliError validate_addressed_config_command(DaliCommandId id)
{
    const DaliCommandInfo *cmd = dali_command_lookup(id);
    if (cmd == NULL ||
        id < DALI_CMD_RESET ||
        id > DALI_CMD_ENABLE_WRITE_MEMORY ||
        cmd->frame_kind != DALI_CMD_FRAME_16BIT ||
        cmd->response_kind != DALI_RESP_NONE) {
        return DALI_ERR_INVALID;
    }
    return DALI_OK;
}

bool dali_control_config_uses_dtr0(DaliCommandId id)
{
    switch (id) {
        case DALI_CMD_SET_OPERATING_MODE_DTR0:
        case DALI_CMD_RESET_MEMORY_BANK_DTR0:
        case DALI_CMD_SET_MAX_LEVEL_DTR0:
        case DALI_CMD_SET_MIN_LEVEL_DTR0:
        case DALI_CMD_SET_SYSTEM_FAILURE_LEVEL_DTR0:
        case DALI_CMD_SET_POWER_ON_LEVEL_DTR0:
        case DALI_CMD_SET_FADE_TIME_DTR0:
        case DALI_CMD_SET_FADE_RATE_DTR0:
        case DALI_CMD_SET_EXTENDED_FADE_TIME_DTR0:
        case DALI_CMD_SET_SCENE:
        case DALI_CMD_SET_SHORT_ADDRESS_DTR0:
            return true;

        default:
            return false;
    }
}

DaliError dali_control_validate_target(DaliTarget target)
{
    switch (target.type) {
        case DALI_ADDR_SHORT:
            return target.address < DALI_SHORT_ADDRESS_COUNT
                 ? DALI_OK
                 : DALI_ERR_INVALID;

        case DALI_ADDR_GROUP:
            return target.address < DALI_GROUP_COUNT
                 ? DALI_OK
                 : DALI_ERR_INVALID;

        case DALI_ADDR_BROADCAST:
            return DALI_OK;

        default:
            return DALI_ERR_INVALID;
    }
}

uint8_t dali_control_ha_brightness_to_dapc(uint8_t brightness)
{
    return (uint8_t)(((uint32_t)brightness * DALI_DAPC_MAX_LEVEL +
                      HA_BRIGHTNESS_ROUNDING) / HA_BRIGHTNESS_MAX);
}

uint8_t dali_control_percent_to_dapc(uint8_t percent)
{
    uint8_t clipped = percent > PERCENT_MAX ? PERCENT_MAX : percent;
    uint8_t brightness = (uint8_t)(((uint32_t)clipped * HA_BRIGHTNESS_MAX +
                                    (PERCENT_MAX / 2u)) / PERCENT_MAX);
    return dali_control_ha_brightness_to_dapc(brightness);
}

DaliError dali_control_build_dapc(DaliTarget target, uint8_t level, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_DAPC, level, out);
}

DaliError dali_control_build_dapc_mask(DaliTarget target, DaliFrame *out)
{
    DaliError err = dali_control_validate_target(target);
    if (err != DALI_OK) {
        return err;
    }
    return dali_build_dapc_mask(target.type, target.address, out);
}

DaliError dali_control_build_off(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_OFF, 0u, out);
}

DaliError dali_control_build_up(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_UP, 0u, out);
}

DaliError dali_control_build_down(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_DOWN, 0u, out);
}

DaliError dali_control_build_step_up(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_STEP_UP, 0u, out);
}

DaliError dali_control_build_step_down(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_STEP_DOWN, 0u, out);
}

DaliError dali_control_build_continuous_up(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_CONTINUOUS_UP, 0u, out);
}

DaliError dali_control_build_continuous_down(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_CONTINUOUS_DOWN, 0u, out);
}

DaliError dali_control_build_recall_max(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_RECALL_MAX_LEVEL, 0u, out);
}

DaliError dali_control_build_recall_min(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_RECALL_MIN_LEVEL, 0u, out);
}

DaliError dali_control_build_step_down_and_off(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_STEP_DOWN_AND_OFF, 0u, out);
}

DaliError dali_control_build_on_and_step_up(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_ON_AND_STEP_UP, 0u, out);
}

DaliError dali_control_build_enable_dapc_sequence(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_ENABLE_DAPC_SEQUENCE, 0u, out);
}

DaliError dali_control_build_go_to_last_active_level(DaliTarget target, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_GO_TO_LAST_ACTIVE_LEVEL, 0u, out);
}

DaliError dali_control_build_go_to_scene(DaliTarget target, uint8_t scene, DaliFrame *out)
{
    return build_target_command(target, DALI_CMD_GO_TO_SCENE, scene, out);
}

DaliError dali_control_build_config(DaliTarget target,
                                    DaliCommandId id,
                                    uint8_t param,
                                    DaliFrame *out)
{
    DaliError err = validate_addressed_config_command(id);
    if (err != DALI_OK) {
        return err;
    }
    return build_target_command(target, id, param, out);
}

DaliError dali_control_build_query(DaliTarget target,
                                   DaliCommandId id,
                                   uint8_t param,
                                   DaliFrame *out)
{
    DaliError err = validate_addressed_query_command(id);
    if (err != DALI_OK) {
        return err;
    }
    return build_target_command(target, id, param, out);
}

DaliError dali_control_build_query_status(DaliTarget target, DaliFrame *out)
{
    return dali_control_build_query(target, DALI_CMD_QUERY_STATUS, 0u, out);
}

DaliError dali_control_build_dtr(DaliDtrRegister reg, uint8_t value, DaliFrame *out)
{
    return dali_build_dtr_data(reg, value, out);
}

DaliError dali_control_set_dtr(DaliDtrRegister reg, uint8_t value)
{
    DaliFrame frame;
    DaliError err = dali_control_build_dtr(reg, value, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return enqueue_frame(&frame, false, false, NULL, NULL);
}

DaliError dali_control_set_level(DaliTarget target, uint8_t level)
{
    return dali_control_set_level_cb(target, level, NULL, NULL);
}

DaliError dali_control_set_level_cb(DaliTarget target,
                                    uint8_t level,
                                    DaliSchedCompletionCb cb,
                                    void *cb_ctx)
{
    DaliFrame frame;
    DaliError err = dali_control_build_dapc(target, level, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return enqueue_frame(&frame, false, false, cb, cb_ctx);
}

DaliError dali_control_off_cb(DaliTarget target,
                              DaliSchedCompletionCb cb,
                              void *cb_ctx)
{
    return enqueue_target_command_cb(target, DALI_CMD_OFF, 0u, cb, cb_ctx);
}

DaliError dali_control_set_brightness(DaliTarget target, uint8_t brightness)
{
    if (brightness == 0u) {
        return dali_control_off(target);
    }
    return dali_control_set_level(target, dali_control_ha_brightness_to_dapc(brightness));
}

DaliError dali_control_set_percent(DaliTarget target, uint8_t percent)
{
    if (percent == 0u) {
        return dali_control_off(target);
    }
    return dali_control_set_level(target, dali_control_percent_to_dapc(percent));
}

DaliError dali_control_off(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_OFF, 0u);
}

DaliError dali_control_up(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_UP, 0u);
}

DaliError dali_control_down(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_DOWN, 0u);
}

DaliError dali_control_step_up(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_STEP_UP, 0u);
}

DaliError dali_control_step_down(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_STEP_DOWN, 0u);
}

DaliError dali_control_recall_max(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_RECALL_MAX_LEVEL, 0u);
}

DaliError dali_control_recall_min(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_RECALL_MIN_LEVEL, 0u);
}

DaliError dali_control_continuous_up(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_CONTINUOUS_UP, 0u);
}

DaliError dali_control_continuous_down(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_CONTINUOUS_DOWN, 0u);
}

DaliError dali_control_step_down_and_off(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_STEP_DOWN_AND_OFF, 0u);
}

DaliError dali_control_on_and_step_up(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_ON_AND_STEP_UP, 0u);
}

DaliError dali_control_enable_dapc_sequence(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_ENABLE_DAPC_SEQUENCE, 0u);
}

DaliError dali_control_go_to_last_active_level(DaliTarget target)
{
    return enqueue_target_command(target, DALI_CMD_GO_TO_LAST_ACTIVE_LEVEL, 0u);
}

DaliError dali_control_go_to_scene(DaliTarget target, uint8_t scene)
{
    return enqueue_target_command(target, DALI_CMD_GO_TO_SCENE, scene);
}

DaliError dali_control_config(DaliTarget target, DaliCommandId id, uint8_t param)
{
    return enqueue_config_command(target, id, param);
}

DaliError dali_control_config_with_dtr0(DaliTarget target,
                                        DaliCommandId id,
                                        uint8_t dtr0_value,
                                        uint8_t param)
{
    if (!dali_control_config_uses_dtr0(id)) {
        return DALI_ERR_INVALID;
    }

    const DaliCommandInfo *cmd = dali_command_lookup(id);
    if (cmd == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame dtr_frame;
    DaliFrame config_frame;
    DaliError err = dali_control_build_dtr(DALI_DTR0, dtr0_value, &dtr_frame);
    if (err != DALI_OK) {
        return err;
    }
    err = dali_control_build_config(target, id, param, &config_frame);
    if (err != DALI_OK) {
        return err;
    }

    DaliSequence seq = {
        .steps = {
            {
                .frame        = dtr_frame,
                .needs_reply  = false,
                .send_twice   = false,
                .retries_left = 0u,
            },
            {
                .frame        = config_frame,
                .needs_reply  = false,
                .send_twice   = cmd->send_twice,
                .retries_left = 0u,
            },
        },
        .step_count  = 2u,
        .on_complete = NULL,
        .cb_ctx      = NULL,
    };
    return dali_sched_enqueue_sequence(&seq);
}

DaliError dali_control_query(DaliTarget target,
                             DaliCommandId id,
                             uint8_t param,
                             DaliSchedCompletionCb cb,
                             void *cb_ctx)
{
    if (cb == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliError err = dali_control_build_query(target, id, param, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return enqueue_frame_with_retries(
        &frame,
        true,
        false,
        dali_command_response_retry_safe(id) ? DALI_MAX_RETRIES : 0u,
        cb,
        cb_ctx);
}

DaliError dali_control_query_status(DaliTarget target,
                                    DaliSchedCompletionCb cb,
                                    void *cb_ctx)
{
    return dali_control_query(target, DALI_CMD_QUERY_STATUS, 0u, cb, cb_ctx);
}
