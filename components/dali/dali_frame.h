#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Timing constants (IEC 62386 / 1200 bps Manchester)
 *
 *   Bit period     : 833.3 µs  (1 / 1200 bps)
 *   Half-bit period: 416.7 µs  (Manchester encoding unit)
 *   Timer tick     : 104   µs  (4× oversampling — 4 ticks per half-bit)
 *
 * --------------------------------------------------------------------------*/
#define DALI_TIMER_TICK_US          104u    /* hardware timer alarm period     */
#define DALI_TICKS_PER_HALF_BIT       4u    /* ticks to count before toggling  */
#define DALI_HALF_BIT_US            417u    /* nominal half-bit period (µs)    */
#define DALI_BIT_US                 833u    /* nominal bit period (µs)         */

/* Confirmed from IEC 62386-101 */
#define DALI_SETTLE_MS                2u    /* RX self-echo suppression after TX */
#define DALI_REPLY_TIMEOUT_MS        25u    /* max wait for backward frame (ms) — 22 ms spec + 3 ms margin */
/*
 * Earliest accepted reply activity, measured from the precise local TX end.
 * IEC 62386-101 gives the forward-to-backward settling time as a range; this is
 * its minimum, so gear answering at the fast end of the range is attributed to
 * its own reply window rather than discarded as stray activity. Do not raise it
 * towards the nominal 7 ms: everything between would then time out.
 */
#define DALI_REPLY_WINDOW_OPEN_US   5500u
/*
 * The open edge for an observation that decoded as a complete backward frame.
 *
 * Derived from DALI_SETTLE_MS rather than chosen, because for a decoded frame
 * that is the only floor with a physical meaning. The 5.5 ms edge above exists
 * to keep *undecodable* activity from being read as a reply — the case that
 * matters, because COMPARE maps qualified activity to YES and so invents gear
 * that is not there. A complete 8-bit backward frame, arriving while a query is
 * outstanding, carries none of that ambiguity: our own 16-bit transmission
 * cannot decode as one, ringing cannot, and another master's forward frame is
 * caught by the intervening-frame branch. What remains is the PHY's own RX
 * self-echo suppression, which is DALI_SETTLE_MS.
 *
 * The alternative was a hand-picked margin, and the 1k installation showed why
 * that loses. Four LED-strip drivers settle in 5.24-5.62 ms, straddling the
 * standard's 5.5 ms minimum. One DT6 driver at a0 varies from 4.12 to 5.85 ms
 * on the same device between consecutive queries — 25% faster than the minimum
 * at its worst — which made it look like it answered some opcodes and not
 * others until three captures showed the failures falling wherever the edge
 * happened to be. Any fixed margin gets chased by the next such device.
 *
 * IEC 62386-101 makes this gear non-conformant. Refusing to read it makes the
 * installation unusable, which is the worse answer, and the verb that asked
 * reported a timeout while a correctly decoded byte sat in the buffer.
 */
#define DALI_REPLY_WINDOW_OPEN_DECODED_US ((uint32_t)DALI_SETTLE_MS * 1000u)
/* The scheduler starts its 25 ms wait after the 2 ms TX/RX handoff. Keep that
 * existing effective deadline while attributing timestamped RX observations. */
#define DALI_REPLY_WINDOW_CLOSE_US (((uint32_t)DALI_SETTLE_MS + (uint32_t)DALI_REPLY_TIMEOUT_MS) * 1000u)
/* Reject isolated pulses as commissioning replies. A corrupted backward frame
 * must still span the first edge through the final data-bit region at the
 * decoder's -25% timing tolerance, and contain several real transitions. */
#define DALI_BACKWARD_ACTIVITY_MIN_SPAN_US ((DALI_HALF_BIT_US * 17u * 3u) / 4u)
#define DALI_BACKWARD_ACTIVITY_MIN_EDGES 4u
#define DALI_MAX_RETRIES              3u    /* send attempts before offline    */
#define DALI_SEND_TWICE_WINDOW_MS   100u    /* max gap between repeated sends  */
/* DALI_HALF_BIT_US rounds nominal Te upward, making this 22 Te guard 9174 µs. */
#define DALI_FORWARD_INTERFRAME_US (DALI_HALF_BIT_US * 22u)
/*
 * Hold-off before retransmitting a frame whose reply window expired.
 *
 * The ordinary forward-interframe guard is armed when the frame is sent, so it
 * has long lapsed by the time a 25 ms reply window closes: without this, the
 * retry goes out the instant the window shuts. Gear that answers a shade later
 * than the timeout is then still driving the bus, and the retransmission lands
 * on the tail of that backward frame — losing both, and making a present device
 * look absent to a scan. One backward frame (11 Te) plus the settle period
 * covers the straggler before the wire is used again.
 */
#define DALI_REPLY_TIMEOUT_BACKOFF_US \
    ((DALI_BIT_US * 11u) + (DALI_SETTLE_MS * 1000u))
#define DALI_SEND_TWICE_WINDOW_US  (DALI_SEND_TWICE_WINDOW_MS * 1000u)
#define DALI_BUS_IDLE_GUARD_US   (DALI_BIT_US * 2u) /* idle high before TX      */
#define DALI_BUS_IDLE_TIMEOUT_US (DALI_BIT_US * 40u) /* ~33 ms; outlasts any 24-bit frame from input devices */

/* ---------------------------------------------------------------------------
 * Protocol limits
 * --------------------------------------------------------------------------*/
#define DALI_BACKWARD_FRAME_BITS      8u
#define DALI_FORWARD_FRAME_BITS      16u
#define DALI_EXTENDED_FRAME_BITS     24u
#define DALI_MAX_FRAME_BITS          DALI_EXTENDED_FRAME_BITS

#define DALI_SHORT_ADDRESS_COUNT     64u
#define DALI_MAX_SHORT_ADDRESS      (DALI_SHORT_ADDRESS_COUNT - 1u)
#define DALI_GROUP_COUNT             16u
#define DALI_MAX_GROUP              (DALI_GROUP_COUNT - 1u)
#define DALI_SCENE_COUNT             16u
#define DALI_MAX_SCENE              (DALI_SCENE_COUNT - 1u)
#define DALI_INSTANCE_COUNT          32u
#define DALI_MAX_INSTANCE           (DALI_INSTANCE_COUNT - 1u)

#define DALI_BROADCAST_DAPC_ADDRESS     0xFEu /* 16/24-bit address byte */
#define DALI_BROADCAST_COMMAND_ADDRESS  0xFFu /* 16/24-bit address byte */
#define DALI_DEVICE_INSTANCE            0xFEu /* 24-bit instance byte */
#define DALI_ALL_INSTANCES              0xFFu /* 24-bit instance byte */
#define DALI_YES_RESPONSE               0xFFu /* 8-bit backward frame */
#define DALI_DAPC_MAX_LEVEL             254u
/* Arc power level 255 is MASK: "leave the level unchanged", not a level.
 * The ordinary DAPC builders reject it so a level cannot become MASK by
 * arithmetic; dali_build_dapc_mask() is the only way to emit it. */
#define DALI_DAPC_MASK_LEVEL            255u

/* ---------------------------------------------------------------------------
 * Buffer sizes — power-of-2 required for ring buffer masking
 * --------------------------------------------------------------------------*/
#define DALI_RX_EDGE_BUFFER_SIZE    256u
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
    DALI_ERR_TIMING     = 8,    /* required protocol timing window was missed */
    DALI_ERR_CANCELLED  = 9,    /* queued/active work cancelled by reset      */
    DALI_ERR_INTERVENED = 10,   /* another forward frame invalidated a reply  */
    DALI_ERR_FULL       = 11,   /* fixed-capacity registration table is full  */
    DALI_ERR_RX_ACTIVITY = 12,  /* reply-window activity was not decodable    */
} DaliError;

/*
 * Short operator-facing name for a DaliError, or NULL for a code this build
 * does not know. Callers print the number themselves in that case, so an
 * enumerator added out of tree still reaches an operator as something readable
 * rather than as a wrong name. Defined in dali_error.c.
 */
const char *dali_error_name(DaliError err);

/*
 * The same name, but always printable: an unknown code is rendered into buf
 * as "error <n>". Returns buf only in that case, so a caller may pass a
 * short scratch buffer and use the result with %s unconditionally.
 */
const char *dali_error_text(DaliError err, char *buf, size_t len);

/* Scratch size that fits any dali_error_text() output, including the widest
 * "error <n>" an out-of-range int can produce. */
#define DALI_ERROR_TEXT_MAX 24u

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
    volatile uint32_t rx_ignored_outside_reply; /* scheduler rejected RX frames */
    volatile uint32_t unsolicited_events_routed; /* raw 24-bit RX event frames  */
    volatile uint32_t raw_malformed;    /* diagnostic raw command parse errors*/
    volatile uint32_t isr_overruns;     /* timer fired before previous ISR done*/
    volatile uint32_t bus_idle_failures; /* TX blocked by active/stuck bus     */
    volatile uint32_t rx_self_echo_suppressed; /* RX ignored during TX echo    */
    volatile uint32_t rx_settle_suppressed; /* RX ignored after TX             */
    volatile uint32_t rx_glitch_drops;  /* RX task dropped duplicate/short edge*/
    /* Frames the PHY clocked out in full. This is the recovery signal that
     * pairs with bus_idle_failures: the fault counters only ever grow, so
     * without a positive counter an integration cannot tell a bus that is
     * currently stuck from one that failed once an hour ago and works now. */
    volatile uint32_t tx_frames_ok;
    /* Undecodable PHY observations attributed to an active reply window. */
    volatile uint32_t reply_rx_activity;
} dali_stats_t;

/* Global stats instance — defined in dali_phy.c, read everywhere */
extern dali_stats_t g_dali_stats;
