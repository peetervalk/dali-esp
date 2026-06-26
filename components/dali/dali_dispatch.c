#include "dali_dispatch.h"
#include <stddef.h>

static bool key_matches(const DaliDispatchKey *key, const DaliInputEvent *event)
{
    if (key->frame_kind   != event->frame_kind)   return false;
    if (key->address_kind != event->address_kind) return false;
    if (key->address      != event->address)      return false;
    if (key->event_code   != DALI_DISPATCH_OPCODE_ANY &&
        key->event_code   != event->event_code)   return false;
    if (event->frame_kind == DALI_EVENT_FRAME_INPUT_24BIT &&
        key->instance     != DALI_DISPATCH_INSTANCE_ANY &&
        key->instance     != event->instance)     return false;
    return true;
}

static bool toggle_get(const DaliDispatchToggleState *state, DaliTarget out)
{
    if (state == NULL) return false;
    if (out.type == DALI_ADDR_GROUP && out.address < DALI_GROUP_COUNT)
        return ((state->group_on >> out.address) & 1u) != 0u;
    if (out.type == DALI_ADDR_SHORT && out.address < DALI_SHORT_ADDRESS_COUNT)
        return ((state->short_on >> out.address) & 1u) != 0u;
    return false;
}

static void toggle_set(DaliDispatchToggleState *state, DaliTarget out, bool on)
{
    if (state == NULL) return;
    if (out.type == DALI_ADDR_GROUP && out.address < DALI_GROUP_COUNT) {
        if (on) state->group_on |=  (uint16_t)(1u << out.address);
        else    state->group_on &= ~(uint16_t)(1u << out.address);
    } else if (out.type == DALI_ADDR_SHORT && out.address < DALI_SHORT_ADDRESS_COUNT) {
        if (on) state->short_on |=  ((uint64_t)1u << out.address);
        else    state->short_on &= ~((uint64_t)1u << out.address);
    }
}

/*
 * Translate a legacy 16-bit command opcode into a dali_control_* call.
 * Also keeps toggle state current for on/off transitions so that any TOGGLE
 * entries on overlapping targets stay consistent.
 *
 * Returns DALI_ERR_INVALID for DAPC frames (address_selector = 0) or
 * unrecognised opcodes — caller treats these as no-match.
 */
static DaliError apply_mirror(const DaliInputEvent    *event,
                              DaliTarget               out,
                              DaliDispatchToggleState *state)
{
    if (!event->address_selector) {
        uint8_t level = event->event_code;
        if (level == 0xFFu) return DALI_ERR_INVALID;
        toggle_set(state, out, level != 0u);
        return dali_control_set_level(out, level);
    }

    uint8_t op = event->event_code;
    switch (op) {
        case 0x00u:
            toggle_set(state, out, false);
            return dali_control_off(out);
        case 0x01u:
            return dali_control_up(out);
        case 0x02u:
            return dali_control_down(out);
        case 0x03u:
            return dali_control_step_up(out);
        case 0x04u:
            return dali_control_step_down(out);
        case 0x05u:
            toggle_set(state, out, true);
            return dali_control_recall_max(out);
        case 0x06u:
            toggle_set(state, out, true);
            return dali_control_recall_min(out);
        case 0x07u:
            toggle_set(state, out, false);
            return dali_control_step_down_and_off(out);
        case 0x08u:
            toggle_set(state, out, true);
            return dali_control_on_and_step_up(out);
        case 0x0Au:
            return dali_control_go_to_last_active_level(out);
        default:
            if (op >= 0x10u && op <= 0x1Fu)
                return dali_control_go_to_scene(out, op - 0x10u);
            return DALI_ERR_INVALID;
    }
}

void dali_dispatch_seed_toggle(DaliDispatchToggleState *state,
                               DaliTarget               target,
                               bool                     is_on)
{
    toggle_set(state, target, is_on);
}

static void result_set(DaliDispatchResult *r, DaliTarget tgt, bool on, uint8_t lvl)
{
    if (r == NULL) return;
    r->has_state = true;
    r->matched   = true;
    r->target    = tgt;
    r->is_on     = on;
    r->level     = lvl;
}

static void result_unknown(DaliDispatchResult *r, DaliTarget tgt)
{
    if (r == NULL) return;
    r->has_state = false;
    r->matched   = true;
    r->target    = tgt;
}

/*
 * Observe a legacy 16-bit command frame without transmitting anything.
 * This is for direct-control couplers: the original frame already commanded
 * the lamps, so the controller only infers state for HA/toggle bookkeeping.
 */
static DaliError observe_legacy(const DaliInputEvent    *event,
                                DaliTarget               out,
                                DaliDispatchToggleState *state,
                                DaliDispatchResult      *result_out)
{
    if (!event->address_selector) {
        uint8_t level = event->event_code;
        if (level == 0xFFu) {
            result_unknown(result_out, out);
            return DALI_OK;
        }
        toggle_set(state, out, level != 0u);
        result_set(result_out, out, level != 0u, level);
        return DALI_OK;
    }

    uint8_t op = event->event_code;
    switch (op) {
        case 0x00u:
            toggle_set(state, out, false);
            result_set(result_out, out, false, 0u);
            return DALI_OK;
        case 0x01u:
        case 0x02u:
        case 0x03u:
        case 0x04u:
            result_unknown(result_out, out);
            return DALI_OK;
        case 0x05u:
            toggle_set(state, out, true);
            result_set(result_out, out, true, 254u);
            return DALI_OK;
        case 0x06u:
            toggle_set(state, out, true);
            result_unknown(result_out, out);
            return DALI_OK;
        case 0x07u:
            toggle_set(state, out, false);
            result_set(result_out, out, false, 0u);
            return DALI_OK;
        case 0x08u:
            toggle_set(state, out, true);
            result_unknown(result_out, out);
            return DALI_OK;
        case 0x0Au:
            result_unknown(result_out, out);
            return DALI_OK;
        default:
            if (op >= 0x10u && op <= 0x1Fu) {
                result_unknown(result_out, out);
                return DALI_OK;
            }
            return DALI_ERR_INVALID;
    }
}

DaliError dali_dispatch(const DaliDispatchEntry  *table,
                        uint8_t                   count,
                        const DaliInputEvent     *event,
                        DaliDispatchToggleState  *toggle_state,
                        DaliDispatchResult       *result_out)
{
    if (table == NULL || event == NULL || count == 0u) return DALI_ERR_INVALID;
    if (result_out) { result_out->has_state = false; result_out->matched = false; }

    for (uint8_t i = 0u; i < count; i++) {
        const DaliDispatchEntry *e = &table[i];
        if (!key_matches(&e->key, event)) continue;

        DaliError err;
        switch (e->action) {
            case DALI_DISPATCH_ACTION_OBSERVE:
                return observe_legacy(event, e->output, toggle_state, result_out);

            case DALI_DISPATCH_ACTION_MIRROR: {
                err = apply_mirror(event, e->output, NULL);
                if (err == DALI_OK) {
                    observe_legacy(event, e->output, toggle_state, result_out);
                }
                return err;
            }

            case DALI_DISPATCH_ACTION_RECALL_MAX:
                err = dali_control_recall_max(e->output);
                if (err == DALI_OK) {
                    toggle_set(toggle_state, e->output, true);
                    result_set(result_out, e->output, true, 254u);
                }
                return err;

            case DALI_DISPATCH_ACTION_RECALL_MIN:
                err = dali_control_recall_min(e->output);
                if (err == DALI_OK) {
                    toggle_set(toggle_state, e->output, true);
                    result_unknown(result_out, e->output);
                }
                return err;

            case DALI_DISPATCH_ACTION_OFF:
                err = dali_control_off(e->output);
                if (err == DALI_OK) {
                    toggle_set(toggle_state, e->output, false);
                    result_set(result_out, e->output, false, 0u);
                }
                return err;

            case DALI_DISPATCH_ACTION_GO_TO_LAST:
                err = dali_control_go_to_last_active_level(e->output);
                if (err == DALI_OK) result_unknown(result_out, e->output);
                return err;

            case DALI_DISPATCH_ACTION_DIM_UP:
                err = dali_control_up(e->output);
                if (err == DALI_OK) result_unknown(result_out, e->output);
                return err;

            case DALI_DISPATCH_ACTION_DIM_DOWN:
                err = dali_control_down(e->output);
                if (err == DALI_OK) result_unknown(result_out, e->output);
                return err;

            case DALI_DISPATCH_ACTION_SCENE:
                err = dali_control_go_to_scene(e->output, e->scene);
                if (err == DALI_OK) result_unknown(result_out, e->output);
                return err;

            case DALI_DISPATCH_ACTION_TOGGLE: {
                bool on = toggle_get(toggle_state, e->output);
                err = on ? dali_control_off(e->output)
                         : dali_control_recall_max(e->output);
                if (err == DALI_OK) {
                    toggle_set(toggle_state, e->output, !on);
                    result_set(result_out, e->output, !on, !on ? 254u : 0u);
                }
                return err;
            }

            default:
                return DALI_ERR_INVALID;
        }
    }

    return DALI_OK; /* no match — silently ignore */
}
