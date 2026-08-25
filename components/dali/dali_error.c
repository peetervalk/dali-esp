#include "dali_frame.h"

#include <stddef.h>
#include <stdio.h>

/*
 * Operator-facing names for DaliError.
 *
 * Both front ends printed a bare number for everything they did not special-
 * case, which was tolerable while the uncommon codes were internal argument
 * faults. It stopped being tolerable when DALI_ERR_RX_ACTIVITY became a normal
 * result of a commissioning run: the one code an operator most needs to read
 * was the one that arrived as "ERR 12".
 *
 * Keep these short and lowercase. They are printed into a shell line and into a
 * Home Assistant text state, neither of which has room for a sentence, and they
 * are the words the documentation uses for the same conditions.
 */
static const char *const s_error_names[] = {
    [DALI_OK]              = "ok",
    [DALI_ERR_TIMEOUT]     = "timeout",
    [DALI_ERR_BUS_STUCK]   = "bus stuck",
    [DALI_ERR_MALFORMED]   = "malformed",
    [DALI_ERR_QUEUE_FULL]  = "queue full",
    [DALI_ERR_OVERFLOW]    = "overflow",
    [DALI_ERR_BUSY]        = "busy",
    [DALI_ERR_INVALID]     = "invalid",
    [DALI_ERR_TIMING]      = "timing",
    [DALI_ERR_CANCELLED]   = "cancelled",
    [DALI_ERR_INTERVENED]  = "intervened",
    [DALI_ERR_FULL]        = "table full",
    [DALI_ERR_RX_ACTIVITY] = "rx activity",
};

const char *dali_error_name(DaliError err)
{
    if ((unsigned)err >= (sizeof(s_error_names) / sizeof(s_error_names[0]))) {
        return NULL;
    }
    return s_error_names[err];
}

const char *dali_error_text(DaliError err, char *buf, size_t len)
{
    const char *name = dali_error_name(err);

    if (name != NULL) {
        return name;
    }
    if (buf == NULL || len == 0u) {
        return "error";
    }
    snprintf(buf, len, "error %d", (int)err);
    return buf;
}
