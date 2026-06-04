#pragma once

/*
 * dali_ringbuf.h — ISR-safe lock-free single-producer / single-consumer
 *                  ring buffer.
 *
 * Contract:
 *   Producer (ISR): calls dali_rb_push_from_isr() only.
 *   Consumer (task): calls dali_rb_pop() and dali_rb_clear() only.
 *   Neither side holds a mutex; safety relies on a single shared index
 *   accessed with compiler barriers / atomics.
 *
 * Capacity must be a power of 2 (enforced by static assert in .c).
 * Elements are uint32_t (used to store packed edge timestamps or TX bits).
 *
 * Host-portable: no ESP-IDF headers included here or in the .c file
 * when DALI_HOST_BUILD is defined.
 */

#include <stdint.h>
#include <stdbool.h>
#include "dali_frame.h"

typedef struct {
    uint32_t         *buf;          /* pointer to statically allocated array  */
    uint32_t          capacity;     /* must be power of 2                     */
    volatile uint32_t head;         /* written by producer (ISR)              */
    volatile uint32_t tail;         /* written by consumer (task)             */
} DaliRingBuf;

/*
 * Initialise a ring buffer.  buf must point to an array of `capacity`
 * uint32_t elements; capacity must be a power of 2.
 */
void dali_rb_init(DaliRingBuf *rb, uint32_t *buf, uint32_t capacity);

/*
 * Push one value from ISR context.
 * Returns DALI_OK or DALI_ERR_OVERFLOW (buffer full — value is dropped).
 */
DaliError dali_rb_push_from_isr(DaliRingBuf *rb, uint32_t value);

/*
 * Pop one value in task context.
 * Returns DALI_OK on success, DALI_ERR_TIMEOUT when the buffer is empty.
 */
DaliError dali_rb_pop(DaliRingBuf *rb, uint32_t *value);

/* Returns true if the buffer contains no elements. Task context only. */
bool dali_rb_empty(const DaliRingBuf *rb);

/* Discard all contents.  Task context only — do not call from ISR. */
void dali_rb_clear(DaliRingBuf *rb);
