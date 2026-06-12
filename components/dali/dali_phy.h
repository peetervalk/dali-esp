#pragma once

/*
 * dali_phy.h — DALI physical layer
 *
 * Responsibilities:
 *   - Manchester encoding and decoding
 *   - TX state machine driven by GPTIMER ISR (104 µs ticks, 4× oversampling)
 *   - RX edge capture via GPIO ISR → ring buffer → task-context decode
 *   - Bus idle detection
 *   - Half-duplex TX/RX mode switching
 *
 * This layer has no knowledge of DALI commands, device addresses, or
 * ESPHome entities.  It only speaks bits and frames.
 */

#include <stdbool.h>
#include <stdint.h>
#include "dali_frame.h"

/* ---------------------------------------------------------------------------
 * PHY TX state machine states
 * --------------------------------------------------------------------------*/
typedef enum {
    DALI_PHY_TX_IDLE       = 0,
    DALI_PHY_TX_START_H    = 1,   /* legacy name: start bit first half (LOW)  */
    DALI_PHY_TX_START_L    = 2,   /* legacy name: start bit second half (HIGH)*/
    DALI_PHY_TX_BIT_FIRST  = 3,   /* data bit — first half                    */
    DALI_PHY_TX_BIT_SECOND = 4,   /* data bit — second half                   */
    DALI_PHY_TX_STOP1      = 5,   /* stop bit 1 (HIGH, full bit period)        */
    DALI_PHY_TX_STOP2      = 6,   /* stop bit 2 (HIGH, full bit period)        */
    DALI_PHY_TX_DONE       = 7,   /* frame complete; timer will be stopped     */
} DaliPhyTxState;

/* ---------------------------------------------------------------------------
 * PHY RX state machine states
 * --------------------------------------------------------------------------*/
typedef enum {
    DALI_PHY_RX_IDLE       = 0,
    DALI_PHY_RX_RECEIVING  = 1,
    DALI_PHY_RX_DONE       = 2,
    DALI_PHY_RX_ERROR      = 3,
} DaliPhyRxState;

#define DALI_PHY_RXDEBUG_MAX_INTERVALS 56u
#define DALI_PHY_RXDEBUG_MAX_EDGES     (DALI_PHY_RXDEBUG_MAX_INTERVALS + 1u)

typedef struct {
    bool      valid;
    DaliError error;
    uint8_t   interval_count;
    uint8_t   edge_count;
    uint32_t  intervals_us[DALI_PHY_RXDEBUG_MAX_INTERVALS];
    uint8_t   edge_levels[DALI_PHY_RXDEBUG_MAX_EDGES];
} DaliPhyRxDebugSnapshot;

/* ---------------------------------------------------------------------------
 * Callback invoked from task context when a complete frame is received.
 * frame is valid only for the duration of the call.
 * --------------------------------------------------------------------------*/
typedef void (*DaliPhyRxCallback)(const DaliFrame *frame, void *ctx);

/* ---------------------------------------------------------------------------
 * Public API — all functions run in task context unless noted
 * --------------------------------------------------------------------------*/

/*
 * Initialise the PHY layer.
 * tx_gpio: GPIO number connected to DALI-2 Click RST / DALI_TX pin.
 *          The Click transmit optocoupler is active-high; the PHY maps
 *          logical DALI idle/high to the inactive physical GPIO level.
 * rx_gpio: GPIO number connected to DALI-2 Click INT / DALI_RX pin.
 *          The Click receive optocoupler output is inverted by the PHY.
 *
 * Must be called once before any other dali_phy_* function.
 */
DaliError dali_phy_init(uint8_t tx_gpio, uint8_t rx_gpio);

/*
 * Register a callback invoked (from DALI task context) for each received
 * frame.  Pass NULL to disable.  ctx is forwarded to the callback.
 */
void dali_phy_set_rx_callback(DaliPhyRxCallback cb, void *ctx);

/*
 * Transmit a frame.  Blocks until transmission completes or timeout.
 * Must not be called from ISR context.
 * Returns DALI_ERR_BUSY if a TX is already in progress.
 */
DaliError dali_phy_tx(const DaliFrame *frame);

/*
 * Process pending RX events.  Must be called regularly from the DALI task
 * (or any task — but only one task at a time).  Drains the RX ring buffer
 * and runs the Manchester decoder.  Invokes the RX callback on frame completion.
 */
void dali_phy_rx_process(void);

/*
 * Reset the PHY state machines and clear ring buffers.
 * Stops any in-progress TX.  Task context only.
 */
DaliError dali_phy_reset(void);

/*
 * Read the current RX-side logic level, if available.
 * Returns DALI_OK with *level_out set to 0/1 on device builds.
 */
DaliError dali_phy_read_rx_level(uint8_t *level_out);

/*
 * Copy the last malformed RX frame snapshot.  edge_levels[] contains logical
 * DALI bus levels after each captured edge: 1 = idle/high, 0 = active/low.
 * edge_count is normally interval_count + 1.
 */
DaliError dali_phy_get_rx_debug(DaliPhyRxDebugSnapshot *snapshot_out);

/* ---------------------------------------------------------------------------
 * Host-testable Manchester encode / decode helpers
 * These are separated from hardware so they can be unit-tested without ESP-IDF.
 * --------------------------------------------------------------------------*/

/*
 * Encode a DaliFrame into a half-bit buffer.
 *
 * DALI Manchester rule:
 *   Bit '1': LOW first half, HIGH second half
 *   Bit '0': HIGH first half, LOW second half
 *   Start bit is LOW then HIGH.
 *   Two stop bits: both halves HIGH (4 half-bits total).
 *
 * out_buf      : caller-supplied buffer; must be >= (1 + bit_length + 2) * 2 bytes
 * out_buf_len  : size of out_buf in bytes
 * Returns number of half-bits written, or 0 on error (frame invalid or buf too small).
 *
 * Each byte in out_buf is 1 (HIGH) or 0 (LOW) for one half-bit period.
 */
uint8_t dali_phy_encode_manchester(const DaliFrame *frame,
                                   uint8_t *out_buf,
                                   uint8_t  out_buf_len);

/*
 * Decode a sequence of edge intervals and logical edge levels into a DaliFrame.
 *
 * intervals    : array of time intervals between consecutive edges (µs)
 * num_intervals: number of entries in intervals[]
 * edge_levels  : logical DALI bus level after each captured edge
 * edge_count   : must be num_intervals + 1
 * frame_out    : output frame on success
 * Returns DALI_OK or DALI_ERR_MALFORMED.
 *
 * Tolerance: ±25% of the nominal half-bit period (IEC 62386 Annex A).
 */
DaliError dali_phy_decode_manchester_edges(const uint32_t *intervals,
                                           uint8_t         num_intervals,
                                           const uint8_t  *edge_levels,
                                           uint8_t         edge_count,
                                           DaliFrame      *frame_out);

/*
 * Decode a sequence of edge intervals (in µs) into a DaliFrame.  This helper
 * assumes the first edge is the leading start edge into logical LOW and
 * reconstructs alternating edge levels.  Hardware RX should prefer
 * dali_phy_decode_manchester_edges() so captured levels are validated.
 *
 * intervals    : array of time intervals between consecutive edges (µs)
 * num_intervals: number of entries in intervals[]
 * frame_out    : output frame on success
 * Returns DALI_OK or DALI_ERR_MALFORMED.
 *
 * Tolerance: ±25% of the nominal half-bit period (IEC 62386 Annex A).
 */
DaliError dali_phy_decode_manchester(const uint32_t *intervals,
                                     uint8_t         num_intervals,
                                     DaliFrame       *frame_out);
