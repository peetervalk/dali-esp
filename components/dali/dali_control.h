#pragma once

/*
 * dali_control.h - high-level DALI command translator
 *
 * This layer translates application intent into protocol frames and scheduler
 * transactions.  It does not know about ESPHome entities; a later integration
 * layer should map configured entities to DaliTarget values and call here.
 */

#include <stdint.h>
#include "dali_frame.h"
#include "dali_scheduler.h"

typedef struct {
    DaliAddressType type;
    uint8_t         address;  /* short: 0-63, group: 0-15, broadcast: ignored */
} DaliTarget;

/* Validate target address range for short/group/broadcast addressing. */
DaliError dali_control_validate_target(DaliTarget target);

/* Conversion helpers.  Return DALI DAPC levels in the valid 0-254 range. */
uint8_t dali_control_ha_brightness_to_dapc(uint8_t brightness);
uint8_t dali_control_percent_to_dapc(uint8_t percent);

/* Build frames without touching the scheduler.  Useful for tests/diagnostics. */
DaliError dali_control_build_dapc(DaliTarget target, uint8_t level, DaliFrame *out);
DaliError dali_control_build_off(DaliTarget target, DaliFrame *out);
DaliError dali_control_build_recall_max(DaliTarget target, DaliFrame *out);
DaliError dali_control_build_recall_min(DaliTarget target, DaliFrame *out);
DaliError dali_control_build_query_status(DaliTarget target, DaliFrame *out);

/* Build and enqueue common transactions. */
DaliError dali_control_set_level(DaliTarget target, uint8_t level);
DaliError dali_control_set_brightness(DaliTarget target, uint8_t brightness);
DaliError dali_control_set_percent(DaliTarget target, uint8_t percent);
DaliError dali_control_off(DaliTarget target);
DaliError dali_control_recall_max(DaliTarget target);
DaliError dali_control_recall_min(DaliTarget target);
DaliError dali_control_query_status(DaliTarget target,
                                    DaliSchedCompletionCb cb,
                                    void *cb_ctx);
