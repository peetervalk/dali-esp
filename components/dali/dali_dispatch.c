#include "dali_dispatch.h"
#include <stddef.h>

static bool key_matches(const DaliDispatchKey *key, const DaliInputEvent *event)
{
    if (key->frame_kind != event->frame_kind) return false;

    uint16_t event_information;
    if (event->frame_kind == DALI_EVENT_FRAME_LEGACY_16BIT) {
        if (key->address_kind != event->address_kind) return false;
        if (key->address != event->address) return false;
        event_information = event->legacy_data;
    } else {
        switch (key->address_kind) {
            case DALI_EVENT_ADDRESS_SHORT:
                if (!event->source.has_device_address ||
                    key->address != event->source.device_address) return false;
                break;
            case DALI_EVENT_ADDRESS_GROUP:
                if (event->source.has_device_group &&
                    key->group_kind != DALI_DISPATCH_GROUP_INSTANCE) {
                    if (key->address != event->source.device_group) return false;
                } else if (event->source.has_instance_group &&
                           key->group_kind != DALI_DISPATCH_GROUP_DEVICE) {
                    if (key->address != event->source.instance_group) return false;
                } else {
                    return false;
                }
                break;
            case DALI_EVENT_ADDRESS_INVALID:
                if (event->source.has_device_address ||
                    event->source.has_device_group ||
                    event->source.has_instance_group) return false;
                break;
            case DALI_EVENT_ADDRESS_BROADCAST:
            default:
                return false;
        }

        event_information = event->event_information;
        if (key->instance != DALI_DISPATCH_INSTANCE_ANY &&
            (!event->source.has_instance ||
             key->instance != event->source.instance)) return false;
        if (key->match_instance_type &&
            (!event->source.has_instance_type ||
             key->instance_type != event->source.instance_type)) return false;
    }

    if (key->event_information != DALI_DISPATCH_EVENT_ANY &&
        key->event_information != event_information) return false;
    return true;
}

static bool toggle_get(const DaliDispatchToggleState *state, DaliTarget out)
{
    if (state == NULL) return false;
    if (out.type == DALI_ADDR_BROADCAST)
        return state->broadcast_on;
    if (out.type == DALI_ADDR_GROUP && out.address < DALI_GROUP_COUNT)
        return ((state->group_on >> out.address) & 1u) != 0u;
    if (out.type == DALI_ADDR_SHORT && out.address < DALI_SHORT_ADDRESS_COUNT)
        return ((state->short_on >> out.address) & 1u) != 0u;
    return false;
}

static void toggle_set(DaliDispatchToggleState *state, DaliTarget out, bool on)
{
    if (state == NULL) return;
    if (out.type == DALI_ADDR_BROADCAST) {
        state->broadcast_on = on;
    } else if (out.type == DALI_ADDR_GROUP && out.address < DALI_GROUP_COUNT) {
        if (on) state->group_on |=  (uint16_t)(1u << out.address);
        else    state->group_on &= ~(uint16_t)(1u << out.address);
    } else if (out.type == DALI_ADDR_SHORT && out.address < DALI_SHORT_ADDRESS_COUNT) {
        if (on) state->short_on |=  ((uint64_t)1u << out.address);
        else    state->short_on &= ~((uint64_t)1u << out.address);
    }
}

static bool same_target(DaliTarget a, DaliTarget b)
{
    if (a.type != b.type) return false;
    return a.type == DALI_ADDR_BROADCAST || a.address == b.address;
}

void dali_dispatch_seed_toggle(DaliDispatchToggleState *state,
                               DaliTarget               target,
                               bool                     is_on)
{
    toggle_set(state, target, is_on);
}

/* ── What an action does to the toggle state and the lights ──────────────── */

/*
 * Worked out before anything is sent, and applied either at once (OBSERVE) or
 * when the scheduler reports the frame transmitted (everything else).
 */
typedef struct {
    bool               sets_toggle;
    bool               toggle_on;
    DaliDispatchResult result;
} DispatchOutcome;

static void outcome_state(DispatchOutcome *o, DaliTarget tgt, bool on, uint8_t lvl)
{
    o->result.has_state = true;
    o->result.matched   = true;
    o->result.target    = tgt;
    o->result.is_on     = on;
    o->result.level     = lvl;
}

static void outcome_unknown(DispatchOutcome *o, DaliTarget tgt)
{
    o->result.has_state = false;
    o->result.matched   = true;
    o->result.target    = tgt;
}

static void outcome_toggle(DispatchOutcome *o, bool on)
{
    o->sets_toggle = true;
    o->toggle_on   = on;
}

/*
 * Infer state from a legacy 16-bit command frame. OBSERVE applies it to the
 * frame a direct-control coupler already sent; MIRROR applies it to the copy
 * it sends to its own output.
 *
 * Returns DALI_ERR_INVALID for an opcode this does not model.
 */
static DaliError infer_legacy(const DaliInputEvent *event,
                              DaliTarget            out,
                              DispatchOutcome      *o)
{
    if (!event->address_selector) {
        uint8_t level = event->legacy_data;
        if (level == 0xFFu) {
            outcome_unknown(o, out);
            return DALI_OK;
        }
        outcome_toggle(o, level != 0u);
        outcome_state(o, out, level != 0u, level);
        return DALI_OK;
    }

    uint8_t op = event->legacy_data;
    switch (op) {
        case 0x00u:
            outcome_toggle(o, false);
            outcome_state(o, out, false, 0u);
            return DALI_OK;
        case 0x01u:
        case 0x02u:
        case 0x03u:
        case 0x04u:
            outcome_unknown(o, out);
            return DALI_OK;
        case 0x05u:   /* RECALL MAX LEVEL */
            /* On, at the gear's MAX LEVEL — not necessarily 254. */
            outcome_toggle(o, true);
            outcome_unknown(o, out);
            return DALI_OK;
        case 0x06u:
            outcome_toggle(o, true);
            outcome_unknown(o, out);
            return DALI_OK;
        case 0x07u:   /* STEP DOWN AND OFF */
            /* Switches off only if the gear was already at its minimum;
             * otherwise it steps down and stays on. Neither the level nor the
             * on/off state can be known here, so leave the toggle alone and let
             * the deferred actual-level query settle both. */
            outcome_unknown(o, out);
            return DALI_OK;
        case 0x08u:
            outcome_toggle(o, true);
            outcome_unknown(o, out);
            return DALI_OK;
        case 0x0Au:
            outcome_unknown(o, out);
            return DALI_OK;
        default:
            if (op >= 0x10u && op <= 0x1Fu) {
                outcome_unknown(o, out);
                return DALI_OK;
            }
            return DALI_ERR_INVALID;
    }
}

/*
 * The frame MIRROR re-issues to its output target: the same DAPC level or
 * command the coupler sent.
 *
 * Returns DALI_ERR_INVALID for DAPC MASK or an opcode it does not re-issue;
 * the caller treats that as no match.
 */
static DaliError build_mirror_frame(const DaliInputEvent *event,
                                    DaliTarget            out,
                                    DaliFrame            *frame)
{
    if (!event->address_selector) {
        uint8_t level = event->legacy_data;
        if (level == 0xFFu) return DALI_ERR_INVALID;
        return dali_control_build_dapc(out, level, frame);
    }

    uint8_t op = event->legacy_data;
    switch (op) {
        case 0x00u: return dali_control_build_off(out, frame);
        case 0x01u: return dali_control_build_up(out, frame);
        case 0x02u: return dali_control_build_down(out, frame);
        case 0x03u: return dali_control_build_step_up(out, frame);
        case 0x04u: return dali_control_build_step_down(out, frame);
        case 0x05u: return dali_control_build_recall_max(out, frame);
        case 0x06u: return dali_control_build_recall_min(out, frame);
        case 0x07u: return dali_control_build_step_down_and_off(out, frame);
        case 0x08u: return dali_control_build_on_and_step_up(out, frame);
        case 0x0Au: return dali_control_build_go_to_last_active_level(out, frame);
        default:
            if (op >= 0x10u && op <= 0x1Fu)
                return dali_control_build_go_to_scene(out, op - 0x10u, frame);
            return DALI_ERR_INVALID;
    }
}

static void outcome_apply(DaliDispatchToggleState *state,
                          const DispatchOutcome   *o)
{
    if (o->sets_toggle) {
        toggle_set(state, o->result.target, o->toggle_on);
    }
}

/* ── Actions waiting for their frame ─────────────────────────────────────── */

/*
 * A transmitting action's effect waits for the scheduler to say its frame went
 * out. It used to be applied when the frame was queued, so a frame that never
 * reached the bus still flipped the toggle and still told Home Assistant the
 * lamps were off.
 *
 * One slot per frame in flight. The scheduler completes every admitted
 * transaction exactly once, cancelled ones included, so a slot is always
 * returned. The pool is one larger than the queue, for the frame on the wire.
 *
 * Owned by the scheduler's owner task, like everything dali_dispatch_cb()
 * touches; see its contract in the header.
 */
#define DISPATCH_PENDING_MAX (DALI_CMD_QUEUE_SIZE + 1u)

typedef struct {
    bool                     in_use;
    uint32_t                 seq;   /* admission order, for toggle_effective() */
    DaliDispatchToggleState *toggle_state;
    DispatchOutcome          outcome;
    DaliDispatchDoneCb       done;
    void                    *done_ctx;
} DispatchPending;

static DispatchPending s_pending[DISPATCH_PENDING_MAX];
static uint32_t        s_pending_seq;

void dali_dispatch_init(void)
{
    for (uint8_t i = 0u; i < DISPATCH_PENDING_MAX; i++) {
        s_pending[i].in_use = false;
    }
    s_pending_seq = 0u;
}

/*
 * The toggle state a TOGGLE decides from: what the bus has confirmed, with the
 * latest action still queued for the same target applied on top. Without the
 * second part, a press arriving before the previous press's frame went out
 * would read the old state and send the same command again.
 *
 * Should that queued frame then fail, the state falls back on its own: its
 * slot is released and the confirmed bit was never changed.
 */
static bool toggle_effective(const DaliDispatchToggleState *state, DaliTarget out)
{
    bool on = toggle_get(state, out);
    if (state == NULL) return on;

    bool found = false;
    uint32_t latest = 0u;
    for (uint8_t i = 0u; i < DISPATCH_PENDING_MAX; i++) {
        const DispatchPending *p = &s_pending[i];
        if (!p->in_use || p->toggle_state != state ||
            !p->outcome.sets_toggle ||
            !same_target(p->outcome.result.target, out)) {
            continue;
        }
        if (!found || (int32_t)(p->seq - latest) > 0) {
            found  = true;
            latest = p->seq;
            on     = p->outcome.toggle_on;
        }
    }
    return on;
}

static void on_pending_complete(DaliError result, const DaliFrame *reply, void *ctx)
{
    (void)reply;
    DispatchPending *slot = (DispatchPending *)ctx;

    /* Released before anything is called, so done may dispatch again. */
    DispatchPending p = *slot;
    slot->in_use = false;

    if (result == DALI_OK) {
        outcome_apply(p.toggle_state, &p.outcome);
    }
    if (p.done != NULL) {
        p.done(result, &p.outcome.result, p.done_ctx);
    }
}

/*
 * Queue frame, and hold outcome until the scheduler reports it. On success the
 * caller's result is the outcome, marked pending. Nothing is held when the
 * scheduler refuses the frame.
 */
static DaliError send_pending(const DaliFrame         *frame,
                              DaliDispatchToggleState *state,
                              const DispatchOutcome   *outcome,
                              DaliDispatchResult      *result_out,
                              DaliDispatchDoneCb       done,
                              void                    *done_ctx)
{
    DispatchPending *slot = NULL;
    for (uint8_t i = 0u; i < DISPATCH_PENDING_MAX; i++) {
        if (!s_pending[i].in_use) {
            slot = &s_pending[i];
            break;
        }
    }
    if (slot == NULL) {
        /* More frames in flight than the scheduler can hold. */
        return DALI_ERR_QUEUE_FULL;
    }

    slot->in_use       = true;
    slot->seq          = ++s_pending_seq;
    slot->toggle_state = state;
    slot->outcome      = *outcome;
    slot->outcome.result.pending = false;
    slot->done         = done;
    slot->done_ctx     = done_ctx;

    DaliTransaction txn = {
        .frame       = *frame,
        .on_complete = on_pending_complete,
        .cb_ctx      = slot,
    };
    DaliError err = dali_sched_enqueue(&txn);
    if (err != DALI_OK) {
        slot->in_use = false;
        return err;
    }

    if (result_out != NULL) {
        *result_out = outcome->result;
        result_out->pending = true;
    }
    return DALI_OK;
}

/* The frame a named action sends; TOGGLE decides from toggle_on_now. */
static DaliError build_action_frame(const DaliDispatchEntry *e,
                                    bool                     toggle_on_now,
                                    DaliFrame               *frame)
{
    switch (e->action) {
        case DALI_DISPATCH_ACTION_RECALL_MAX:
            return dali_control_build_recall_max(e->output, frame);
        case DALI_DISPATCH_ACTION_RECALL_MIN:
            return dali_control_build_recall_min(e->output, frame);
        case DALI_DISPATCH_ACTION_OFF:
            return dali_control_build_off(e->output, frame);
        case DALI_DISPATCH_ACTION_GO_TO_LAST:
            return dali_control_build_go_to_last_active_level(e->output, frame);
        case DALI_DISPATCH_ACTION_DIM_UP:
            return dali_control_build_up(e->output, frame);
        case DALI_DISPATCH_ACTION_DIM_DOWN:
            return dali_control_build_down(e->output, frame);
        case DALI_DISPATCH_ACTION_SCENE:
            return dali_control_build_go_to_scene(e->output, e->scene, frame);
        case DALI_DISPATCH_ACTION_TOGGLE:
            return toggle_on_now ? dali_control_build_off(e->output, frame)
                                 : dali_control_build_recall_max(e->output, frame);
        default:
            return DALI_ERR_INVALID;
    }
}

/* What a named action leaves the target in. */
static void infer_action(const DaliDispatchEntry *e,
                         bool                     toggle_on_now,
                         DispatchOutcome         *o)
{
    switch (e->action) {
        case DALI_DISPATCH_ACTION_RECALL_MAX:
            /* The gear lands on its own MAX LEVEL, which is 254 only when MAX
             * LEVEL was never reduced. Assert that it is on and let the
             * deferred actual-level query supply the number, as RECALL MIN
             * already does. */
            outcome_toggle(o, true);
            outcome_unknown(o, e->output);
            break;
        case DALI_DISPATCH_ACTION_RECALL_MIN:
            outcome_toggle(o, true);
            outcome_unknown(o, e->output);
            break;
        case DALI_DISPATCH_ACTION_OFF:
            outcome_toggle(o, false);
            outcome_state(o, e->output, false, 0u);
            break;
        case DALI_DISPATCH_ACTION_TOGGLE:
            outcome_toggle(o, !toggle_on_now);
            if (toggle_on_now) {
                /* OFF is exactly level 0 whatever the gear limits. */
                outcome_state(o, e->output, false, 0u);
            } else {
                /* The on branch is RECALL MAX; see above. */
                outcome_unknown(o, e->output);
            }
            break;
        case DALI_DISPATCH_ACTION_GO_TO_LAST:
        case DALI_DISPATCH_ACTION_DIM_UP:
        case DALI_DISPATCH_ACTION_DIM_DOWN:
        case DALI_DISPATCH_ACTION_SCENE:
        default:
            outcome_unknown(o, e->output);
            break;
    }
}

DaliError dali_dispatch_cb(const DaliDispatchEntry  *table,
                           uint8_t                   count,
                           const DaliInputEvent     *event,
                           DaliDispatchToggleState  *toggle_state,
                           DaliDispatchResult       *result_out,
                           DaliDispatchDoneCb        done,
                           void                     *done_ctx)
{
    if (table == NULL || event == NULL || count == 0u) return DALI_ERR_INVALID;
    if (result_out) {
        result_out->has_state = false;
        result_out->matched   = false;
        result_out->pending   = false;
    }

    for (uint8_t i = 0u; i < count; i++) {
        const DaliDispatchEntry *e = &table[i];
        if (!key_matches(&e->key, event)) continue;

        DispatchOutcome outcome = {0};
        DaliFrame frame;
        DaliError err;

        switch (e->action) {
            case DALI_DISPATCH_ACTION_OBSERVE:
                if (event->frame_kind != DALI_EVENT_FRAME_LEGACY_16BIT)
                    return DALI_ERR_INVALID;
                err = infer_legacy(event, e->output, &outcome);
                if (err != DALI_OK) return err;
                /* The coupler already sent it: nothing to wait for. */
                outcome_apply(toggle_state, &outcome);
                if (result_out) *result_out = outcome.result;
                return DALI_OK;

            case DALI_DISPATCH_ACTION_MIRROR:
                if (event->frame_kind != DALI_EVENT_FRAME_LEGACY_16BIT)
                    return DALI_ERR_INVALID;
                err = build_mirror_frame(event, e->output, &frame);
                if (err != DALI_OK) return err;
                err = infer_legacy(event, e->output, &outcome);
                if (err != DALI_OK) return err;
                return send_pending(&frame, toggle_state, &outcome,
                                    result_out, done, done_ctx);

            case DALI_DISPATCH_ACTION_RECALL_MAX:
            case DALI_DISPATCH_ACTION_RECALL_MIN:
            case DALI_DISPATCH_ACTION_OFF:
            case DALI_DISPATCH_ACTION_GO_TO_LAST:
            case DALI_DISPATCH_ACTION_DIM_UP:
            case DALI_DISPATCH_ACTION_DIM_DOWN:
            case DALI_DISPATCH_ACTION_SCENE:
            case DALI_DISPATCH_ACTION_TOGGLE: {
                const bool on_now = toggle_effective(toggle_state, e->output);
                err = build_action_frame(e, on_now, &frame);
                if (err != DALI_OK) return err;
                infer_action(e, on_now, &outcome);
                return send_pending(&frame, toggle_state, &outcome,
                                    result_out, done, done_ctx);
            }

            default:
                return DALI_ERR_INVALID;
        }
    }

    return DALI_OK; /* no match — silently ignore */
}

DaliError dali_dispatch(const DaliDispatchEntry  *table,
                        uint8_t                   count,
                        const DaliInputEvent     *event,
                        DaliDispatchToggleState  *toggle_state,
                        DaliDispatchResult       *result_out)
{
    return dali_dispatch_cb(table, count, event, toggle_state, result_out,
                            NULL, NULL);
}
