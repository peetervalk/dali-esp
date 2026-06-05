#include "dali_control.h"
#include "dali_protocol.h"

#define DALI_DAPC_MAX_LEVEL 254u

static DaliError enqueue_frame(const DaliFrame *frame,
                               bool needs_reply,
                               DaliSchedCompletionCb cb,
                               void *cb_ctx)
{
    if (frame == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliTransaction txn = {
        .frame        = *frame,
        .needs_reply  = needs_reply,
        .send_twice   = false,
        .retries_left = needs_reply ? DALI_MAX_RETRIES : 0u,
        .on_complete  = cb,
        .cb_ctx       = cb_ctx,
    };
    return dali_sched_enqueue(&txn);
}

DaliError dali_control_validate_target(DaliTarget target)
{
    switch (target.type) {
        case DALI_ADDR_SHORT:
            return target.address < 64u ? DALI_OK : DALI_ERR_INVALID;

        case DALI_ADDR_GROUP:
            return target.address < 16u ? DALI_OK : DALI_ERR_INVALID;

        case DALI_ADDR_BROADCAST:
            return DALI_OK;

        default:
            return DALI_ERR_INVALID;
    }
}

uint8_t dali_control_ha_brightness_to_dapc(uint8_t brightness)
{
    return (uint8_t)(((uint32_t)brightness * DALI_DAPC_MAX_LEVEL + 128u) / 255u);
}

uint8_t dali_control_percent_to_dapc(uint8_t percent)
{
    uint8_t clipped = percent > 100u ? 100u : percent;
    uint8_t brightness = (uint8_t)(((uint32_t)clipped * 255u + 50u) / 100u);
    return dali_control_ha_brightness_to_dapc(brightness);
}

DaliError dali_control_build_dapc(DaliTarget target, uint8_t level, DaliFrame *out)
{
    if (out == NULL || level > DALI_DAPC_MAX_LEVEL) {
        return DALI_ERR_INVALID;
    }
    DaliError err = dali_control_validate_target(target);
    if (err != DALI_OK) {
        return err;
    }

    switch (target.type) {
        case DALI_ADDR_SHORT:
            *out = dali_cmd_dapc(target.address, level);
            return DALI_OK;

        case DALI_ADDR_GROUP:
            *out = dali_cmd_group_dapc(target.address, level);
            return DALI_OK;

        case DALI_ADDR_BROADCAST:
            *out = dali_cmd_broadcast_dapc(level);
            return DALI_OK;

        default:
            return DALI_ERR_INVALID;
    }
}

DaliError dali_control_build_off(DaliTarget target, DaliFrame *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    DaliError err = dali_control_validate_target(target);
    if (err != DALI_OK) {
        return err;
    }

    switch (target.type) {
        case DALI_ADDR_SHORT:
            *out = dali_cmd_off(target.address);
            return DALI_OK;

        case DALI_ADDR_GROUP:
            *out = dali_cmd_group_off(target.address);
            return DALI_OK;

        case DALI_ADDR_BROADCAST:
            *out = dali_cmd_broadcast_off();
            return DALI_OK;

        default:
            return DALI_ERR_INVALID;
    }
}

DaliError dali_control_build_recall_max(DaliTarget target, DaliFrame *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    DaliError err = dali_control_validate_target(target);
    if (err != DALI_OK) {
        return err;
    }

    switch (target.type) {
        case DALI_ADDR_SHORT:
            *out = dali_cmd_recall_max(target.address);
            return DALI_OK;

        case DALI_ADDR_GROUP:
            *out = dali_cmd_group_recall_max(target.address);
            return DALI_OK;

        case DALI_ADDR_BROADCAST:
            *out = dali_cmd_broadcast_recall_max();
            return DALI_OK;

        default:
            return DALI_ERR_INVALID;
    }
}

DaliError dali_control_build_recall_min(DaliTarget target, DaliFrame *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    DaliError err = dali_control_validate_target(target);
    if (err != DALI_OK) {
        return err;
    }

    switch (target.type) {
        case DALI_ADDR_SHORT:
            *out = dali_cmd_recall_min(target.address);
            return DALI_OK;

        case DALI_ADDR_GROUP:
            *out = dali_cmd_group_recall_min(target.address);
            return DALI_OK;

        case DALI_ADDR_BROADCAST:
            *out = dali_cmd_broadcast_recall_min();
            return DALI_OK;

        default:
            return DALI_ERR_INVALID;
    }
}

DaliError dali_control_build_query_status(DaliTarget target, DaliFrame *out)
{
    if (out == NULL) {
        return DALI_ERR_INVALID;
    }
    DaliError err = dali_control_validate_target(target);
    if (err != DALI_OK) {
        return err;
    }

    return dali_build_command(target.type, target.address, DALI_CMD_QUERY_STATUS, 0u, out);
}

DaliError dali_control_set_level(DaliTarget target, uint8_t level)
{
    DaliFrame frame;
    DaliError err = dali_control_build_dapc(target, level, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return enqueue_frame(&frame, false, NULL, NULL);
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
    DaliFrame frame;
    DaliError err = dali_control_build_off(target, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return enqueue_frame(&frame, false, NULL, NULL);
}

DaliError dali_control_recall_max(DaliTarget target)
{
    DaliFrame frame;
    DaliError err = dali_control_build_recall_max(target, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return enqueue_frame(&frame, false, NULL, NULL);
}

DaliError dali_control_recall_min(DaliTarget target)
{
    DaliFrame frame;
    DaliError err = dali_control_build_recall_min(target, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return enqueue_frame(&frame, false, NULL, NULL);
}

DaliError dali_control_query_status(DaliTarget target,
                                    DaliSchedCompletionCb cb,
                                    void *cb_ctx)
{
    if (cb == NULL) {
        return DALI_ERR_INVALID;
    }

    DaliFrame frame;
    DaliError err = dali_control_build_query_status(target, &frame);
    if (err != DALI_OK) {
        return err;
    }
    return enqueue_frame(&frame, true, cb, cb_ctx);
}
