#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * Timing constants (IEC 62386 / 1200 bps Manchester)
 *
 *   Bit period     : 833.3 µs  (1 / 1200 bps)
 *   Half-bit period: 416.7 µs  (Manchester encoding unit)
 *   Timer tick     : 104   µs  (4× oversampling — 4 ticks per half-bit)
 *
 * TX-to-RX settle and reply timeout are marked TBD; update from IEC 62386
 * part 1 section 8 once confirmed.
 * --------------------------------------------------------------------------*/
#define DALI_TIMER_TICK_US          104u    /* hardware timer alarm period     */
#define DALI_TICKS_PER_HALF_BIT       4u    /* ticks to count before toggling  */
#define DALI_HALF_BIT_US            417u    /* nominal half-bit period (µs)    */
#define DALI_BIT_US                 833u    /* nominal bit period (µs)         */

/* TBD: fill from IEC 62386 part 1 when confirmed */
#define DALI_SETTLE_MS                7u    /* min TX-to-RX turnaround (ms)    */
#define DALI_REPLY_TIMEOUT_MS        20u    /* max wait for backward frame (ms)*/
#define DALI_MAX_RETRIES              3u    /* send attempts before offline    */
#define DALI_SEND_TWICE_WINDOW_MS   100u    /* max gap between repeated sends  */

/* ---------------------------------------------------------------------------
 * Buffer sizes — power-of-2 required for ring buffer masking
 * --------------------------------------------------------------------------*/
#define DALI_RX_EDGE_BUFFER_SIZE     64u
#define DALI_TX_HALF_BIT_BUFFER_SIZE 64u    /* pre-encoded half-bits for TX    */
#define DALI_CMD_QUEUE_SIZE          16u
#define DALI_RESPONSE_BUFFER_SIZE     8u

/* ---------------------------------------------------------------------------
 * DaliFrame — core data unit
 *
 * data      : MSB-first; bit (bit_length - 1) is transmitted first
 * bit_length: 16 for standard DALI, 24 for DALI-2 extended frames
 * --------------------------------------------------------------------------*/
typedef struct {
    uint32_t data;
    uint8_t  bit_length;
} DaliFrame;

/* ---------------------------------------------------------------------------
 * DaliError — all functions return one of these; never use bare integers
 * --------------------------------------------------------------------------*/
typedef enum {
    DALI_OK             = 0,
    DALI_ERR_TIMEOUT    = 1,    /* no response within reply window            */
    DALI_ERR_BUS_STUCK  = 2,    /* bus idle timeout — line held low           */
    DALI_ERR_MALFORMED  = 3,    /* Manchester decode error                    */
    DALI_ERR_QUEUE_FULL = 4,    /* scheduler queue full on enqueue            */
    DALI_ERR_OVERFLOW   = 5,    /* ring buffer full — event dropped in ISR    */
    DALI_ERR_BUSY       = 6,    /* PHY TX already in progress                 */
    DALI_ERR_INVALID    = 7,    /* bad argument (e.g. bit_length == 0)        */
} DaliError;

/* ---------------------------------------------------------------------------
 * DaliAddressType
 * --------------------------------------------------------------------------*/
typedef enum {
    DALI_ADDR_SHORT     = 0,
    DALI_ADDR_GROUP     = 1,
    DALI_ADDR_BROADCAST = 2,
} DaliAddressType;

/* ---------------------------------------------------------------------------
 * dali_stats_t — diagnostic counters; updated atomically from ISR and tasks.
 * All fields are 32-bit so that reads are naturally atomic on Xtensa LX6.
 * Never reset from ISR context.
 * --------------------------------------------------------------------------*/
typedef struct {
    volatile uint32_t rx_overflow;      /* RX ring buffer dropped events      */
    volatile uint32_t tx_retries;       /* scheduler TX retry count           */
    volatile uint32_t malformed_frames; /* Manchester decode errors            */
    volatile uint32_t reply_timeouts;   /* scheduler reply timeout count      */
    volatile uint32_t isr_overruns;     /* timer fired before previous ISR done*/
} dali_stats_t;

/* Global stats instance — defined in dali_phy.c, read everywhere */
extern dali_stats_t g_dali_stats;
