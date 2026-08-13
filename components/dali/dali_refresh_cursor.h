#ifndef DALI_REFRESH_CURSOR_H
#define DALI_REFRESH_CURSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DALI_REFRESH_ADVANCE = 0,
    DALI_REFRESH_RETRY,
} DaliRefreshDisposition;

typedef struct {
    uint8_t next;
    bool    active;
    bool    request_pending;
} DaliRefreshCursor;

static inline void dali_refresh_cursor_prepare(DaliRefreshCursor *cursor,
                                               uint8_t count)
{
    if (cursor->active && cursor->next >= count) {
        cursor->active = false;
        cursor->next = 0u;
    }
    if (!cursor->active && cursor->request_pending) {
        cursor->request_pending = false;
        if (count > 0u) cursor->active = true;
    }
}

/* Multiple requests before a pass starts coalesce into that pass. A request
 * made during a pass guarantees one coalesced follow-up pass. */
static inline void dali_refresh_cursor_request(DaliRefreshCursor *cursor)
{
    if (cursor != NULL) cursor->request_pending = true;
}

/* Returns the current registry index without advancing it. */
static inline bool dali_refresh_cursor_current(DaliRefreshCursor *cursor,
                                               uint8_t count,
                                               uint8_t *index_out)
{
    if (cursor == NULL || index_out == NULL) return false;
    dali_refresh_cursor_prepare(cursor, count);
    if (!cursor->active) return false;
    *index_out = cursor->next;
    return true;
}

/* Advance only after successful scheduler admission or an intentional skip.
 * RETRY retains the current index for the next pump call. */
static inline void dali_refresh_cursor_complete(DaliRefreshCursor *cursor,
                                                uint8_t count,
                                                DaliRefreshDisposition disposition)
{
    if (cursor == NULL || !cursor->active || disposition == DALI_REFRESH_RETRY) return;
    if (cursor->next < count) cursor->next++;
    dali_refresh_cursor_prepare(cursor, count);
}

static inline bool dali_refresh_cursor_is_active(const DaliRefreshCursor *cursor)
{
    return cursor != NULL && cursor->active;
}

static inline bool dali_refresh_cursor_has_pending_request(const DaliRefreshCursor *cursor)
{
    return cursor != NULL && cursor->request_pending;
}

#ifdef __cplusplus
}
#endif

#endif /* DALI_REFRESH_CURSOR_H */
