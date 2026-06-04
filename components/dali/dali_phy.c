#include "dali_phy.h"
#include "dali_ringbuf.h"

#ifndef DALI_HOST_BUILD
#include "esp_attr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#define IRAM_ATTR
/* Stub out ESP-IDF logging for host builds */
#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#define ESP_LOGD(tag, fmt, ...) ((void)(tag))
#define ESP_LOGV(tag, fmt, ...) ((void)(tag))
#endif

#include <inttypes.h>
#include <string.h>

static const char *TAG = "DALI-PHY";

/* ---------------------------------------------------------------------------
 * Global stats (declared extern in dali_frame.h)
 * --------------------------------------------------------------------------*/
dali_stats_t g_dali_stats;

/* ---------------------------------------------------------------------------
 * RX ring buffer storage
 * Each entry packs: bits[31:1] = timestamp_us (31-bit), bit[0] = edge level
 * --------------------------------------------------------------------------*/
static uint32_t s_rx_edge_buf[DALI_RX_EDGE_BUFFER_SIZE];
static DaliRingBuf s_rx_rb;

/* ---------------------------------------------------------------------------
 * RX frame accumulation state (task context only — never touched from ISR)
 *
 * Intervals between consecutive RX edges, in microseconds.
 * Max: 1 start + 24 data bits Manchester-encoded = up to ~54 edge intervals.
 * --------------------------------------------------------------------------*/
#define RX_INTERVAL_BUF_MAX 56u
static uint32_t s_rx_intervals[RX_INTERVAL_BUF_MAX];
static uint8_t  s_rx_interval_count;
static uint32_t s_rx_last_edge_ts_us;   /* 31-bit wrapped µs timestamp */
static bool     s_rx_in_frame;           /* currently accumulating edges */

/* ---------------------------------------------------------------------------
 * TX half-bit buffer: one byte per half-bit (1 = HIGH, 0 = LOW)
 * Max frame: 1 start + 24 data + 2 stop = 27 bits → 54 half-bits
 * --------------------------------------------------------------------------*/
#define TX_HALF_BIT_BUF_MAX 64u
static uint8_t  s_tx_half_bits[TX_HALF_BIT_BUF_MAX];
static uint8_t  s_tx_total_half_bits;
static uint8_t  s_tx_half_bit_idx;

/* ---------------------------------------------------------------------------
 * TX state machine
 * --------------------------------------------------------------------------*/
static volatile DaliPhyTxState s_tx_state;
static volatile uint8_t        s_tx_tick_count; /* counts 0..3 per half-bit  */

/* Task to notify when TX completes */
#ifndef DALI_HOST_BUILD
static TaskHandle_t s_tx_notify_task;
#endif

/* ---------------------------------------------------------------------------
 * RX callback
 * --------------------------------------------------------------------------*/
static DaliPhyRxCallback s_rx_callback;
static void             *s_rx_callback_ctx;

/* ---------------------------------------------------------------------------
 * GPIO configuration
 * --------------------------------------------------------------------------*/
static uint8_t s_tx_gpio;
static uint8_t s_rx_gpio;

/* ---------------------------------------------------------------------------
 * GPTIMER handle
 * --------------------------------------------------------------------------*/
#ifndef DALI_HOST_BUILD
static gptimer_handle_t s_gptimer;
#endif

/* ---------------------------------------------------------------------------
 * ISR guard: detect re-entry (overrun)
 * --------------------------------------------------------------------------*/
static volatile uint8_t s_isr_active;

/* ===========================================================================
 * Manchester encode / decode — host-portable, no hardware calls
 * =========================================================================*/

uint8_t dali_phy_encode_manchester(const DaliFrame *frame,
                                   uint8_t *out_buf,
                                   uint8_t  out_buf_len)
{
    if (frame == NULL || out_buf == NULL || frame->bit_length == 0u) {
        return 0u;
    }

    /* start + data bits + 2 stop bits, each 2 half-bits */
    uint8_t total = (uint8_t)((1u + frame->bit_length + 2u) * 2u);
    if (total > out_buf_len) {
        return 0u;
    }

    uint8_t idx = 0u;

    /* Start bit: '1' → HIGH then LOW */
    out_buf[idx++] = 1u;
    out_buf[idx++] = 0u;

    /* Data bits, MSB first */
    for (uint8_t bit = frame->bit_length; bit > 0u; bit--) {
        uint8_t b = (uint8_t)((frame->data >> (bit - 1u)) & 0x01u);
        if (b) {
            out_buf[idx++] = 1u; /* HIGH first half */
            out_buf[idx++] = 0u; /* LOW  second half */
        } else {
            out_buf[idx++] = 0u; /* LOW  first half */
            out_buf[idx++] = 1u; /* HIGH second half */
        }
    }

    /* Two stop bits: both halves HIGH */
    out_buf[idx++] = 1u;
    out_buf[idx++] = 1u;
    out_buf[idx++] = 1u;
    out_buf[idx++] = 1u;

    return idx;
}

DaliError dali_phy_decode_manchester(const uint32_t *intervals,
                                     uint8_t         num_intervals,
                                     DaliFrame       *frame_out)
{
    if (intervals == NULL || frame_out == NULL || num_intervals == 0u) {
        return DALI_ERR_INVALID;
    }

    /* Tolerance: ±25% of nominal half-bit period (IEC 62386 Annex A) */
    const uint32_t half_min = (DALI_HALF_BIT_US * 3u) / 4u;  /* 312 µs */
    const uint32_t half_max = (DALI_HALF_BIT_US * 5u) / 4u;  /* 521 µs */
    const uint32_t full_min = (DALI_BIT_US      * 3u) / 4u;  /* 625 µs */
    const uint32_t full_max = (DALI_BIT_US      * 5u) / 4u;  /* 1041 µs*/

    /*
     * Interval-based Manchester decoder.
     *
     * The first edge in intervals[] is always the mid-bit falling edge of the
     * start bit (bus is idle HIGH; start bit = '1' → HIGH first half, LOW
     * second half).  Initial decoder state after this implicit first edge:
     *   level  = LOW  (bus level after the falling start-bit mid-edge)
     *   at_mid = 1    (we have just processed a mid-bit edge)
     *
     * Interval classification:
     *   SHORT (~417 µs): one half-bit gap.
     *     From at_mid=1: a bit-boundary edge (cross into next bit territory).
     *     From at_mid=0: the mid-bit edge of the current bit.
     *   LONG  (~833 µs): two half-bit gap.
     *     Only valid from at_mid=1: no bit-boundary edge occurred (adjacent
     *     bits share the same first-half level); this edge is the mid-bit of
     *     the next bit.
     *
     * Bit value extraction: at every mid-bit edge, the bus level AFTER the
     * edge is the second half of the current bit:
     *   level = LOW  after mid-bit edge  →  bit '1'
     *   level = HIGH after mid-bit edge  →  bit '0'
     *
     * Valid DALI frame bit lengths: 8 (backward), 16 (forward), 24 (DALI-2).
     */
    uint8_t  level    = 0u;   /* 0=LOW, 1=HIGH; starts LOW after start falling edge */
    uint8_t  at_mid   = 1u;   /* 1=just saw mid-bit edge; 0=at bit-boundary         */
    uint32_t data     = 0u;
    uint8_t  num_bits = 0u;

    for (uint8_t i = 0u; i < num_intervals; i++) {
        uint32_t iv       = intervals[i];
        uint8_t  is_short = (iv >= half_min && iv <= half_max) ? 1u : 0u;
        uint8_t  is_long  = (iv >= full_min && iv <= full_max) ? 1u : 0u;

        if (!is_short && !is_long) {
            return DALI_ERR_MALFORMED;
        }

        if (at_mid) {
            if (is_long) {
                /* No bit-boundary edge; this is the mid-bit of the next bit.
                 * Consecutive bits share the same first-half level so the
                 * boundary transition did not occur. */
                level ^= 1u;
                if (num_bits >= 24u) { return DALI_ERR_MALFORMED; }
                data = (data << 1u) | (level == 0u ? 1u : 0u);
                num_bits++;
                /* at_mid remains 1 */
            } else {
                /* Short interval: bit-boundary edge. */
                level ^= 1u;
                at_mid = 0u;
            }
        } else {
            /* At bit-boundary: the only valid next interval is SHORT (mid-bit). */
            if (!is_short) {
                return DALI_ERR_MALFORMED;
            }
            level ^= 1u;
            if (num_bits >= 24u) { return DALI_ERR_MALFORMED; }
            data = (data << 1u) | (level == 0u ? 1u : 0u);
            num_bits++;
            at_mid = 1u;
        }
    }

    if (num_bits != 8u && num_bits != 16u && num_bits != 24u) {
        return DALI_ERR_MALFORMED;
    }

    frame_out->data       = data;
    frame_out->bit_length = num_bits;
    return DALI_OK;
}

/* ===========================================================================
 * TX ISR — runs every 104 µs
 * =========================================================================*/
#ifndef DALI_HOST_BUILD
static bool IRAM_ATTR dali_phy_tx_isr(gptimer_handle_t timer,
                                       const gptimer_alarm_event_data_t *edata,
                                       void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;

    /* Overrun guard */
    if (s_isr_active) {
        g_dali_stats.isr_overruns++;
        return false;
    }
    s_isr_active = 1u;

    if (s_tx_state == DALI_PHY_TX_IDLE || s_tx_state == DALI_PHY_TX_DONE) {
        s_isr_active = 0u;
        return false;
    }

    /* Advance tick; act only every DALI_TICKS_PER_HALF_BIT ticks */
    s_tx_tick_count++;
    if (s_tx_tick_count < DALI_TICKS_PER_HALF_BIT) {
        s_isr_active = 0u;
        return false;
    }
    s_tx_tick_count = 0u;

    if (s_tx_half_bit_idx >= s_tx_total_half_bits) {
        /* All half-bits sent */
        gpio_set_level(s_tx_gpio, 1);   /* release bus (idle HIGH) */
        s_tx_state = DALI_PHY_TX_DONE;
        BaseType_t higher_prio_woken = pdFALSE;
        if (s_tx_notify_task != NULL) {
            vTaskNotifyGiveFromISR(s_tx_notify_task, &higher_prio_woken);
        }
        s_isr_active = 0u;
        return higher_prio_woken == pdTRUE;
    }

    /* Output current half-bit */
    gpio_set_level(s_tx_gpio, s_tx_half_bits[s_tx_half_bit_idx]);
    s_tx_half_bit_idx++;

    s_isr_active = 0u;
    return false;
}

/* ---------------------------------------------------------------------------
 * RX GPIO ISR — captures edge timestamps into ring buffer
 * --------------------------------------------------------------------------*/
static void IRAM_ATTR dali_phy_rx_isr(void *arg)
{
    (void)arg;

    /* Pack timestamp (µs, lower 31 bits) and level into one uint32_t */
    int64_t  ts_raw   = esp_timer_get_time();
    uint32_t ts_us    = (uint32_t)(ts_raw & 0x7FFFFFFFu);
    uint32_t level    = (uint32_t)gpio_get_level(s_rx_gpio);
    uint32_t entry    = (ts_us << 1u) | (level & 0x01u);

    DaliError err = dali_rb_push_from_isr(&s_rx_rb, entry);
    if (err != DALI_OK) {
        g_dali_stats.rx_overflow++;
    }
}
#endif /* !DALI_HOST_BUILD */

/* ===========================================================================
 * Public API
 * =========================================================================*/

DaliError dali_phy_init(uint8_t tx_gpio, uint8_t rx_gpio)
{
    s_tx_gpio = tx_gpio;
    s_rx_gpio = rx_gpio;

    memset(&g_dali_stats, 0, sizeof(g_dali_stats));

    dali_rb_init(&s_rx_rb, s_rx_edge_buf, DALI_RX_EDGE_BUFFER_SIZE);

    s_tx_state          = DALI_PHY_TX_IDLE;
    s_tx_tick_count     = 0u;
    s_tx_half_bit_idx   = 0u;
    s_tx_total_half_bits = 0u;
    s_isr_active        = 0u;

#ifndef DALI_HOST_BUILD
    /* Configure TX GPIO: output, default HIGH (DALI bus idle) */
    gpio_config_t tx_cfg = {
        .pin_bit_mask = (1ULL << tx_gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&tx_cfg);
    gpio_set_level(tx_gpio, 1);

    /* Configure RX GPIO: input, interrupt on both edges */
    gpio_config_t rx_cfg = {
        .pin_bit_mask = (1ULL << rx_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&rx_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(rx_gpio, dali_phy_rx_isr, NULL);

    /* Configure GPTIMER: 1 MHz resolution, alarm every 104 µs */
    gptimer_config_t timer_cfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000u,  /* 1 µs per tick */
    };
    esp_err_t ret = gptimer_new_timer(&timer_cfg, &s_gptimer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_new_timer failed: %d", ret);
        return DALI_ERR_INVALID;
    }

    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count                = DALI_TIMER_TICK_US,
        .reload_count               = 0u,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_set_alarm_action(s_gptimer, &alarm_cfg);

    gptimer_event_callbacks_t cbs = {
        .on_alarm = dali_phy_tx_isr,
    };
    gptimer_register_event_callbacks(s_gptimer, &cbs, NULL);
    gptimer_enable(s_gptimer);

    ESP_LOGI(TAG, "PHY init OK: TX GPIO%d, RX GPIO%d", tx_gpio, rx_gpio);
#endif /* !DALI_HOST_BUILD */

    return DALI_OK;
}

void dali_phy_set_rx_callback(DaliPhyRxCallback cb, void *ctx)
{
    s_rx_callback     = cb;
    s_rx_callback_ctx = ctx;
}

DaliError dali_phy_tx(const DaliFrame *frame)
{
    if (frame == NULL || frame->bit_length == 0u || frame->bit_length > 24u) {
        return DALI_ERR_INVALID;
    }
    if (s_tx_state != DALI_PHY_TX_IDLE) {
        return DALI_ERR_BUSY;
    }

    /* Pre-encode the frame into the half-bit buffer */
    uint8_t len = dali_phy_encode_manchester(frame, s_tx_half_bits, TX_HALF_BIT_BUF_MAX);
    if (len == 0u) {
        return DALI_ERR_INVALID;
    }

    s_tx_total_half_bits = len;
    s_tx_half_bit_idx    = 0u;
    s_tx_tick_count      = 0u;
    s_tx_state           = DALI_PHY_TX_START_H;

    ESP_LOGD(TAG, "TX start: 0x%0*" PRIx32 " (%d-bit)",
             (frame->bit_length + 3) / 4, frame->data, (int)frame->bit_length);

#ifndef DALI_HOST_BUILD
    s_tx_notify_task = xTaskGetCurrentTaskHandle();
    gptimer_start(s_gptimer);

    /* Wait for TX_DONE notification from ISR, with generous timeout */
    uint32_t timeout_ticks = pdMS_TO_TICKS(
        ((uint32_t)(1u + frame->bit_length + 2u) * DALI_BIT_US) / 1000u + 20u);
    if (ulTaskNotifyTake(pdTRUE, timeout_ticks) == 0u) {
        gptimer_stop(s_gptimer);
        s_tx_state = DALI_PHY_TX_IDLE;
        ESP_LOGW(TAG, "TX timeout");
        return DALI_ERR_TIMEOUT;
    }

    gptimer_stop(s_gptimer);
    s_tx_state = DALI_PHY_TX_IDLE;
    ESP_LOGD(TAG, "TX done");
#else
    /* On host: simulate instant completion */
    s_tx_state = DALI_PHY_TX_IDLE;
#endif

    return DALI_OK;
}

void dali_phy_rx_process(void)
{
    /*
     * Drain the RX ring buffer.  Each entry: bits[31:1] = timestamp_us,
     * bit[0] = bus level after the edge.
     *
     * Accumulate edge-to-edge intervals.  When the bus has been silent
     * for at least one bit period (DALI_BIT_US) after the last edge, the
     * frame is complete and we attempt Manchester decode.
     *
     * First edge received starts the frame; its timestamp becomes the
     * reference — no interval is generated for it because we need two
     * edges to compute a gap.
     */
    uint32_t entry;
    while (dali_rb_pop(&s_rx_rb, &entry) == DALI_OK) {
        uint32_t ts_us = entry >> 1u;
        if (!s_rx_in_frame) {
            /* First edge: start-bit mid-bit falling edge. */
            s_rx_in_frame        = true;
            s_rx_interval_count  = 0u;
            s_rx_last_edge_ts_us = ts_us;
        } else {
            /* Compute interval; handle 31-bit counter wrap. */
            uint32_t iv = (ts_us - s_rx_last_edge_ts_us) & 0x7FFFFFFFu;
            s_rx_last_edge_ts_us = ts_us;
            if (s_rx_interval_count < RX_INTERVAL_BUF_MAX) {
                s_rx_intervals[s_rx_interval_count++] = iv;
            } else {
                /* Interval buffer overflow — discard this frame. */
                s_rx_in_frame       = false;
                s_rx_interval_count = 0u;
                g_dali_stats.malformed_frames++;
                ESP_LOGW(TAG, "RX interval buffer overflow; frame discarded");
            }
        }
    }

    /* Check for frame completion: bus silent for >= 1 bit period. */
    if (s_rx_in_frame && s_rx_interval_count > 0u) {
#ifndef DALI_HOST_BUILD
        uint32_t now_us  = (uint32_t)(esp_timer_get_time() & 0x7FFFFFFFu);
        uint32_t silence = (now_us - s_rx_last_edge_ts_us) & 0x7FFFFFFFu;
        if (silence >= DALI_BIT_US) {
            DaliFrame frame;
            DaliError err = dali_phy_decode_manchester(
                s_rx_intervals, s_rx_interval_count, &frame);
            if (err == DALI_OK) {
                ESP_LOGD(TAG, "RX frame: 0x%"PRIx32" (%u bits)",
                         frame.data, (unsigned)frame.bit_length);
                if (s_rx_callback != NULL) {
                    s_rx_callback(&frame, s_rx_callback_ctx);
                }
            } else {
                g_dali_stats.malformed_frames++;
                ESP_LOGD(TAG, "RX decode error %d (%u intervals)",
                         (int)err, (unsigned)s_rx_interval_count);
            }
            s_rx_in_frame       = false;
            s_rx_interval_count = 0u;
        }
#else
        /* Host build: no real time source; just drain. */
        s_rx_in_frame       = false;
        s_rx_interval_count = 0u;
#endif
    }
}

DaliError dali_phy_reset(void)
{
#ifndef DALI_HOST_BUILD
    gptimer_stop(s_gptimer);
#endif
    s_tx_state           = DALI_PHY_TX_IDLE;
    s_tx_half_bit_idx    = 0u;
    s_tx_total_half_bits = 0u;
    s_tx_tick_count      = 0u;
    dali_rb_clear(&s_rx_rb);
    s_rx_in_frame        = false;
    s_rx_interval_count  = 0u;
    ESP_LOGD(TAG, "PHY reset");
    return DALI_OK;
}
