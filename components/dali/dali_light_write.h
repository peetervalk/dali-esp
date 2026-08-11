#ifndef DALI_LIGHT_WRITE_H
#define DALI_LIGHT_WRITE_H

/*
 * dali_light_write.h — desired-state write arbitration for one light entity
 *
 * Header-only and free of ESPHome/FreeRTOS types so the state machine can be
 * exercised on the host. The integration layer owns the actual transmission;
 * this decides what to send, when to suppress a redundant command, and what to
 * believe afterwards.
 *
 * The rule this exists to enforce: a scheduler enqueue returning DALI_OK means
 * queued, not transmitted. Committing the deduplication cache at enqueue time
 * lets a command that later failed on the bus mask the identical retry, so the
 * gear stays at the old level with nothing able to correct it. The cache is
 * therefore committed only from dali_light_write_confirm(), and a failure both
 * invalidates it and re-arms the desired state for one bounded retry.
 *
 * All calls are single-threaded (the integration's own task/core). A completion
 * arriving on another task must be handed over before calling confirm().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Automatic retries after a *transmission* failure. Enqueue rejection is
 * transient back-pressure and retries until admitted; a bus-level failure may
 * be persistent, so it is bounded to avoid a dead bus generating traffic
 * forever. The cache stays invalid after the budget is spent, so an operator
 * or Home Assistant repeating the command still reaches the bus.
 */
#define DALI_LIGHT_WRITE_TX_RETRIES 1u

typedef enum {
    /* Nothing to do: no desired state pending, or one is already in flight. */
    DALI_LIGHT_WRITE_IDLE = 0,
    /* Desired state already matches confirmed state — suppress the command. */
    DALI_LIGHT_WRITE_SUPPRESS,
    /* Send OFF to the target. */
    DALI_LIGHT_WRITE_SEND_OFF,
    /* Send DAPC `level` to the target. */
    DALI_LIGHT_WRITE_SEND_LEVEL,
} DaliLightWriteAction;

typedef struct {
    /* Desired state awaiting transmission. */
    bool    pending;
    bool    pending_is_on;
    uint8_t pending_level;

    /* Desired state handed to the scheduler and awaiting its completion. */
    bool    in_flight;
    bool    in_flight_is_on;
    uint8_t in_flight_level;
    uint8_t tx_retries_left;

    /* Last state confirmed transmitted or observed on the bus. */
    bool    known_valid;
    bool    known_is_on;
    uint8_t known_level;
} DaliLightWrite;

/* ── Desired state ───────────────────────────────────────────────────────── */

/* Record the latest desired state, replacing any earlier pending one. */
static inline void dali_light_write_request(DaliLightWrite *w,
                                            bool is_on,
                                            uint8_t level)
{
    if (w == NULL) return;
    w->pending       = true;
    w->pending_is_on = is_on;
    w->pending_level = is_on ? level : 0u;
    w->tx_retries_left = DALI_LIGHT_WRITE_TX_RETRIES;
}

static inline bool dali_light_write_has_pending(const DaliLightWrite *w)
{
    return w != NULL && w->pending;
}

static inline bool dali_light_write_in_flight(const DaliLightWrite *w)
{
    return w != NULL && w->in_flight;
}

/* ── Transmission ────────────────────────────────────────────────────────── */

/*
 * Decide what to do with the pending desired state. `level_out` is meaningful
 * only for DALI_LIGHT_WRITE_SEND_LEVEL. SUPPRESS clears the pending state; the
 * SEND_* actions leave it set until dali_light_write_sent() reports whether the
 * scheduler admitted the command.
 *
 * Only one command is in flight at a time: a newer desired state stays pending
 * until the outstanding one completes, so its confirmation can never be
 * attributed to the wrong level.
 */
static inline DaliLightWriteAction dali_light_write_next(DaliLightWrite *w,
                                                         uint8_t *level_out)
{
    if (w == NULL || !w->pending || w->in_flight) return DALI_LIGHT_WRITE_IDLE;

    if (w->known_valid &&
        w->known_is_on == w->pending_is_on &&
        (!w->pending_is_on || w->known_level == w->pending_level)) {
        w->pending = false;
        return DALI_LIGHT_WRITE_SUPPRESS;
    }

    if (!w->pending_is_on) return DALI_LIGHT_WRITE_SEND_OFF;
    if (level_out != NULL) *level_out = w->pending_level;
    return DALI_LIGHT_WRITE_SEND_LEVEL;
}

/*
 * Report the scheduler's admission result for the command dali_light_write_next()
 * returned. `admitted` false retains the pending desired state so the next pump
 * retries it — queue pressure is transient and must not silently drop a command.
 */
static inline void dali_light_write_sent(DaliLightWrite *w, bool admitted)
{
    if (w == NULL || !w->pending || w->in_flight) return;
    if (!admitted) return;

    w->in_flight       = true;
    w->in_flight_is_on = w->pending_is_on;
    w->in_flight_level = w->pending_level;
    w->pending         = false;
}

/*
 * Report the scheduler completion for the in-flight command.
 *
 * Success commits the deduplication cache. Failure invalidates it — the gear's
 * real level is now unknown, so no later command may be suppressed against it —
 * and re-arms the same desired state while retry budget remains.
 *
 * Returns true when a retry was re-armed.
 */
static inline bool dali_light_write_confirm(DaliLightWrite *w, bool success)
{
    if (w == NULL || !w->in_flight) return false;
    w->in_flight = false;

    if (success) {
        w->known_valid     = true;
        w->known_is_on     = w->in_flight_is_on;
        w->known_level     = w->in_flight_is_on ? w->in_flight_level : 0u;
        w->tx_retries_left = DALI_LIGHT_WRITE_TX_RETRIES;
        return false;
    }

    w->known_valid = false;
    if (w->tx_retries_left == 0u) return false;
    w->tx_retries_left--;

    /* A newer desired state supersedes the failed one; never resurrect a stale
     * level over an operator's more recent intent. */
    if (!w->pending) {
        w->pending       = true;
        w->pending_is_on = w->in_flight_is_on;
        w->pending_level = w->in_flight_level;
    }
    return true;
}

/* ── Observed state ──────────────────────────────────────────────────────── */

/*
 * Adopt a level observed on the bus (a query reply or a snooped frame) as the
 * confirmed state. Ignored while a command is in flight: that command's own
 * completion is the authority for the state it is establishing, and a readback
 * taken before it executed would otherwise be committed over it.
 */
static inline bool dali_light_write_observe(DaliLightWrite *w,
                                            bool is_on,
                                            uint8_t level)
{
    if (w == NULL || w->in_flight) return false;
    w->known_valid = true;
    w->known_is_on = is_on;
    w->known_level = is_on ? level : 0u;
    return true;
}

/* Drop the cache without touching pending or in-flight work. */
static inline void dali_light_write_invalidate(DaliLightWrite *w)
{
    if (w != NULL) w->known_valid = false;
}

#ifdef __cplusplus
}
#endif

#endif /* DALI_LIGHT_WRITE_H */
