#include "dali_ringbuf.h"

#ifndef DALI_HOST_BUILD
#include "esp_attr.h"   /* IRAM_ATTR */
#else
#define IRAM_ATTR       /* no-op on host */
#endif

#include <assert.h>

/* Verify capacity is a power of 2 at runtime (assert fires once at init) */
static inline bool is_power_of_two(uint32_t n)
{
    return (n > 0u) && ((n & (n - 1u)) == 0u);
}

void dali_rb_init(DaliRingBuf *rb, uint32_t *buf, uint32_t capacity)
{
    assert(is_power_of_two(capacity));
    rb->buf      = buf;
    rb->capacity = capacity;
    rb->head     = 0u;
    rb->tail     = 0u;
}

/*
 * ISR-safe push.
 *
 * Only the producer writes `head`; only the consumer reads it after the
 * store, so a release store on `head` is sufficient to publish the new
 * element without a full memory barrier.
 *
 * Reading `tail` from the ISR is safe because the consumer only ever
 * increments `tail`, which means the ISR may see a stale (conservative)
 * view — i.e. it may think the buffer is fuller than it is, but will
 * never think it has more room than it actually has.
 */
IRAM_ATTR DaliError dali_rb_push_from_isr(DaliRingBuf *rb, uint32_t value)
{
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);

    if ((head - tail) >= rb->capacity) {
        return DALI_ERR_OVERFLOW;
    }

    rb->buf[head & (rb->capacity - 1u)] = value;

    /* Release store: makes the element visible before the head increment */
    __atomic_store_n(&rb->head, head + 1u, __ATOMIC_RELEASE);

    return DALI_OK;
}

/*
 * Task-context pop.
 *
 * Only the consumer writes `tail`; we acquire-load `head` so that we
 * see the element written by the ISR before its head store.
 */
DaliError dali_rb_pop(DaliRingBuf *rb, uint32_t *value)
{
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);

    if (tail == head) {
        return DALI_ERR_TIMEOUT;  /* buffer empty; re-use TIMEOUT as "no data" */
    }

    *value = rb->buf[tail & (rb->capacity - 1u)];

    __atomic_store_n(&rb->tail, tail + 1u, __ATOMIC_RELEASE);

    return DALI_OK;
}

bool dali_rb_empty(const DaliRingBuf *rb)
{
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    return tail == head;
}

void dali_rb_clear(DaliRingBuf *rb)
{
    /* Task context only.  Reset both indices; no ISR call must be in flight. */
    __atomic_store_n(&rb->tail, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&rb->head, 0u, __ATOMIC_RELEASE);
}
