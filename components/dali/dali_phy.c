#include "dali_phy.h"
#include "dali_ringbuf.h"

#ifndef DALI_HOST_BUILD
#include "esp_attr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
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

#include <string.h>
#include <inttypes.h>

static const char *TAG = "DALI-PHY";

_Static_assert((DALI_RX_EDGE_BUFFER_SIZE & (DALI_RX_EDGE_BUFFER_SIZE - 1u)) == 0u,
               "DALI_RX_EDGE_BUFFER_SIZE must be a power of two");
_Static_assert(DALI_TX_HALF_BIT_BUFFER_SIZE >= ((1u + DALI_MAX_FRAME_BITS + 2u) * 2u),
               "DALI_TX_HALF_BIT_BUFFER_SIZE must hold the largest encoded frame");

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
#define RX_FRAME_GAP_MIN_US ((DALI_BIT_US * 3u) / 2u)
#define RX_FULL_BIT_MAX_US  ((DALI_BIT_US * 5u) / 4u)
#define RX_GLITCH_MIN_US    150u
_Static_assert(RX_FRAME_GAP_MIN_US > RX_FULL_BIT_MAX_US,
               "RX frame gap must be wider than a legal full-bit interval");

static uint32_t s_rx_intervals[DALI_PHY_RXDEBUG_MAX_INTERVALS];
static uint8_t  s_rx_edge_levels[DALI_PHY_RXDEBUG_MAX_EDGES];
static uint8_t  s_rx_interval_count;
static uint8_t  s_rx_edge_count;
static uint8_t  s_rx_last_edge_level;
static uint32_t s_rx_last_edge_ts_us;   /* 31-bit wrapped µs timestamp */
static bool     s_rx_in_frame;           /* currently accumulating edges */

static DaliPhyRxDebugSnapshot s_rx_debug_snapshot;

/* ---------------------------------------------------------------------------
 * TX half-bit buffer: one byte per half-bit (1 = HIGH, 0 = LOW)
 * Max frame: 1 start + 24 data + 2 stop = 27 bits → 54 half-bits
 * --------------------------------------------------------------------------*/
static uint8_t  s_tx_half_bits[DALI_TX_HALF_BIT_BUFFER_SIZE];
static uint8_t  s_tx_total_half_bits;
static uint8_t  s_tx_half_bit_idx;

/* ---------------------------------------------------------------------------
 * TX state machine
 * --------------------------------------------------------------------------*/
static volatile DaliPhyTxState s_tx_state;
static volatile uint8_t        s_tx_tick_count; /* counts 0..3 per half-bit  */

#define RX_TS_MASK       0x7FFFFFFFu
#define RX_TS_HALF_RANGE 0x40000000u

static volatile uint8_t  s_rx_settle_suppression_active;
static volatile uint32_t s_rx_suppress_until_us;

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
static gpio_num_t s_tx_gpio;
static gpio_num_t s_rx_gpio;

/* ---------------------------------------------------------------------------
 * GPTIMER handle
 * --------------------------------------------------------------------------*/
#ifndef DALI_HOST_BUILD
static gptimer_handle_t s_gptimer;

/*
 * MikroE DALI 2 Click optocoupler polarity:
 *   - TX GPIO high turns the transmit optocoupler on and drives the DALI bus low.
 *   - RX GPIO is pulled low when the receive optocoupler reports DALI bus high.
 *
 * The PHY internals use logical DALI bus level: 1 = idle/high, 0 = active/low.
 */
static uint8_t IRAM_ATTR tx_pin_level_for_bus_level(uint8_t bus_level)
{
    return bus_level == 0u ? 1u : 0u;
}

static uint8_t IRAM_ATTR rx_bus_level_from_pin(void)
{
    return gpio_get_level(s_rx_gpio) == 0 ? 1u : 0u;
}
#endif

/* ---------------------------------------------------------------------------
 * ISR guard: detect re-entry (overrun)
 * --------------------------------------------------------------------------*/
static volatile uint8_t s_isr_active;

#ifndef DALI_HOST_BUILD
static portMUX_TYPE s_rx_debug_mux = portMUX_INITIALIZER_UNLOCKED;
#define RX_DEBUG_ENTER() taskENTER_CRITICAL(&s_rx_debug_mux)
#define RX_DEBUG_EXIT()  taskEXIT_CRITICAL(&s_rx_debug_mux)
#else
#define RX_DEBUG_ENTER() ((void)0)
#define RX_DEBUG_EXIT()  ((void)0)
#endif

#ifndef DALI_HOST_BUILD
static bool IRAM_ATTR rx_ts_not_after(uint32_t now_us, uint32_t deadline_us)
{
    return ((deadline_us - now_us) & RX_TS_MASK) < RX_TS_HALF_RANGE;
}

static bool IRAM_ATTR rx_settle_suppression_active(uint32_t now_us)
{
    if (!s_rx_settle_suppression_active) {
        return false;
    }

    if (rx_ts_not_after(now_us, s_rx_suppress_until_us)) {
        return true;
    }

    s_rx_settle_suppression_active = 0u;
    return false;
}

static bool IRAM_ATTR tx_rx_suppression_active(void)
{
    return s_tx_state != DALI_PHY_TX_IDLE;
}

static uint32_t tx_rx_timestamp_us(void)
{
    return (uint32_t)(esp_timer_get_time() & RX_TS_MASK);
}

static DaliError wait_for_bus_idle_before_tx(void)
{
    int64_t start_us      = esp_timer_get_time();
    int64_t deadline_us   = start_us + (int64_t)DALI_BUS_IDLE_TIMEOUT_US;
    int64_t idle_since_us = -1;

    while (esp_timer_get_time() < deadline_us) {
        uint8_t level = rx_bus_level_from_pin();
        int64_t now_us = esp_timer_get_time();

        if (level != 0u) {
            if (idle_since_us < 0) {
                idle_since_us = now_us;
            }
            if ((now_us - idle_since_us) >= (int64_t)DALI_BUS_IDLE_GUARD_US) {
                return DALI_OK;
            }
        } else {
            idle_since_us = -1;
        }

        esp_rom_delay_us(50u);
    }

    g_dali_stats.bus_idle_failures++;
    return DALI_ERR_BUS_STUCK;
}
#endif

static void rx_debug_clear(void)
{
    DaliPhyRxDebugSnapshot empty = {0};

    RX_DEBUG_ENTER();
    s_rx_debug_snapshot = empty;
    RX_DEBUG_EXIT();
}

static void rx_debug_store(DaliError error)
{
    DaliPhyRxDebugSnapshot snapshot = {
        .valid          = true,
        .error          = error,
        .interval_count = s_rx_interval_count,
        .edge_count     = s_rx_edge_count,
    };

    if (snapshot.interval_count > DALI_PHY_RXDEBUG_MAX_INTERVALS) {
        snapshot.interval_count = DALI_PHY_RXDEBUG_MAX_INTERVALS;
    }
    if (snapshot.edge_count > DALI_PHY_RXDEBUG_MAX_EDGES) {
        snapshot.edge_count = DALI_PHY_RXDEBUG_MAX_EDGES;
    }

    memcpy(snapshot.intervals_us,
           s_rx_intervals,
           (size_t)snapshot.interval_count * sizeof(snapshot.intervals_us[0]));
    memcpy(snapshot.edge_levels,
           s_rx_edge_levels,
           (size_t)snapshot.edge_count * sizeof(snapshot.edge_levels[0]));

    RX_DEBUG_ENTER();
    s_rx_debug_snapshot = snapshot;
    RX_DEBUG_EXIT();
}

static void complete_rx_frame(void)
{
    if (!s_rx_in_frame || s_rx_interval_count == 0u) {
        s_rx_in_frame       = false;
        s_rx_interval_count = 0u;
        s_rx_edge_count     = 0u;
        s_rx_last_edge_level = 0u;
        return;
    }

    DaliFrame frame;
    DaliError err = dali_phy_decode_manchester_edges(
        s_rx_intervals, s_rx_interval_count,
        s_rx_edge_levels, s_rx_edge_count, &frame);
    if (err == DALI_OK) {
        ESP_LOGD(TAG, "RX frame: 0x%"PRIx32" (%u bits)",
                 frame.data, (unsigned)frame.bit_length);
        if (s_rx_callback != NULL) {
            s_rx_callback(&frame, s_rx_callback_ctx);
        }
    } else {
        rx_debug_store(err);
        g_dali_stats.malformed_frames++;
        ESP_LOGD(TAG, "RX decode error %d (%u intervals)",
                 (int)err, (unsigned)s_rx_interval_count);
    }

    s_rx_in_frame       = false;
    s_rx_interval_count = 0u;
    s_rx_edge_count     = 0u;
    s_rx_last_edge_level = 0u;
}

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

    /* Start bit: LOW then HIGH */
    out_buf[idx++] = 0u;
    out_buf[idx++] = 1u;

    /* Data bits, MSB first */
    for (uint8_t bit = frame->bit_length; bit > 0u; bit--) {
        uint8_t b = (uint8_t)((frame->data >> (bit - 1u)) & 0x01u);
        if (b) {
            out_buf[idx++] = 0u; /* LOW  first half */
            out_buf[idx++] = 1u; /* HIGH second half */
        } else {
            out_buf[idx++] = 1u; /* HIGH first half */
            out_buf[idx++] = 0u; /* LOW  second half */
        }
    }

    /* Two stop bits: both halves HIGH */
    out_buf[idx++] = 1u;
    out_buf[idx++] = 1u;
    out_buf[idx++] = 1u;
    out_buf[idx++] = 1u;

    return idx;
}

static DaliError decode_half_bits(const uint8_t *half_bits,
                                  uint8_t half_bit_count,
                                  DaliFrame *frame_out)
{
    static const uint8_t frame_lengths[] = {
        DALI_BACKWARD_FRAME_BITS,
        DALI_FORWARD_FRAME_BITS,
        DALI_EXTENDED_FRAME_BITS,
    };

    if (half_bits == NULL || frame_out == NULL) {
        return DALI_ERR_INVALID;
    }

    if (half_bit_count < (uint8_t)((1u + DALI_BACKWARD_FRAME_BITS) * 2u) ||
        half_bits[0] != 0u || half_bits[1] != 1u) {
        return DALI_ERR_MALFORMED;
    }

    for (uint8_t len_i = 0u;
         len_i < (uint8_t)(sizeof(frame_lengths) / sizeof(frame_lengths[0]));
         len_i++) {
        uint8_t bit_length = frame_lengths[len_i];
        uint8_t needed = (uint8_t)((1u + bit_length) * 2u);
        uint8_t max_with_stops = (uint8_t)(needed + 4u);

        if (half_bit_count < needed || half_bit_count > max_with_stops) {
            continue;
        }

        uint32_t data = 0u;
        uint8_t hb = 2u; /* first data bit after the start-bit pair */
        for (uint8_t bit = 0u; bit < bit_length; bit++, hb = (uint8_t)(hb + 2u)) {
            uint8_t first  = half_bits[hb];
            uint8_t second = half_bits[hb + 1u];
            uint8_t value;

            if (first == 0u && second == 1u) {
                value = 1u;
            } else if (first == 1u && second == 0u) {
                value = 0u;
            } else {
                return DALI_ERR_MALFORMED;
            }

            data = (data << 1u) | value;
        }

        for (uint8_t stop = needed; stop < half_bit_count; stop++) {
            if (half_bits[stop] != 1u) {
                return DALI_ERR_MALFORMED;
            }
        }

        frame_out->data       = data;
        frame_out->bit_length = bit_length;
        return DALI_OK;
    }

    return DALI_ERR_MALFORMED;
}

DaliError dali_phy_decode_manchester_edges(const uint32_t *intervals,
                                           uint8_t         num_intervals,
                                           const uint8_t  *edge_levels,
                                           uint8_t         edge_count,
                                           DaliFrame      *frame_out)
{
    if (intervals == NULL || edge_levels == NULL || frame_out == NULL ||
        num_intervals == 0u || edge_count != (uint8_t)(num_intervals + 1u)) {
        return DALI_ERR_INVALID;
    }

    if (num_intervals > DALI_PHY_RXDEBUG_MAX_INTERVALS) {
        return DALI_ERR_MALFORMED;
    }

    /* Tolerance: ±25% of nominal half-bit period (IEC 62386 Annex A) */
    const uint32_t half_min = (DALI_HALF_BIT_US * 3u) / 4u;  /* 312 µs */
    const uint32_t half_max = (DALI_HALF_BIT_US * 5u) / 4u;  /* 521 µs */
    const uint32_t full_min = (DALI_BIT_US      * 3u) / 4u;  /* 625 µs */
    const uint32_t full_max = (DALI_BIT_US      * 5u) / 4u;  /* 1041 µs*/

    uint8_t half_bits[(1u + DALI_MAX_FRAME_BITS + 2u) * 2u];
    uint8_t half_count = 0u;

    if (edge_levels[0] > 1u) {
        return DALI_ERR_MALFORMED;
    }
    half_bits[half_count++] = edge_levels[0];

    for (uint8_t i = 0u; i < num_intervals; i++) {
        uint32_t iv       = intervals[i];
        uint8_t  is_short = (iv >= half_min && iv <= half_max) ? 1u : 0u;
        uint8_t  is_long  = (iv >= full_min && iv <= full_max) ? 1u : 0u;
        uint8_t  next_level = edge_levels[i + 1u];

        if ((!is_short && !is_long) || next_level > 1u ||
            next_level == half_bits[half_count - 1u]) {
            return DALI_ERR_MALFORMED;
        }

        if (is_long) {
            if (half_count >= (uint8_t)sizeof(half_bits)) {
                return DALI_ERR_MALFORMED;
            }
            half_bits[half_count] = half_bits[half_count - 1u];
            half_count++;
        }

        if (half_count >= (uint8_t)sizeof(half_bits)) {
            return DALI_ERR_MALFORMED;
        }
        half_bits[half_count++] = next_level;
    }

    return decode_half_bits(half_bits, half_count, frame_out);
}

DaliError dali_phy_decode_manchester(const uint32_t *intervals,
                                     uint8_t         num_intervals,
                                     DaliFrame       *frame_out)
{
    if (intervals == NULL || frame_out == NULL || num_intervals == 0u) {
        return DALI_ERR_INVALID;
    }
    if (num_intervals > DALI_PHY_RXDEBUG_MAX_INTERVALS) {
        return DALI_ERR_MALFORMED;
    }

    uint8_t edge_levels[DALI_PHY_RXDEBUG_MAX_EDGES];
    uint8_t level = 0u;
    edge_levels[0] = level;
    for (uint8_t i = 0u; i < num_intervals; i++) {
        level ^= 1u;
        edge_levels[i + 1u] = level;
    }

    return dali_phy_decode_manchester_edges(intervals,
                                            num_intervals,
                                            edge_levels,
                                            (uint8_t)(num_intervals + 1u),
                                            frame_out);
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
        /* All half-bits sent — bus released to idle. */
        gpio_set_level(s_tx_gpio, tx_pin_level_for_bus_level(1u));

        /* Arm the settle-suppression window from the ISR so the deadline is
         * precise (no FreeRTOS scheduling jitter).  The window must expire
         * well before the 7 ms DALI minimum answer time. */
        uint32_t ts_us = (uint32_t)(esp_timer_get_time() & RX_TS_MASK);
        s_rx_suppress_until_us = (ts_us + (uint32_t)DALI_SETTLE_MS * 1000u) & RX_TS_MASK;
        __asm__ __volatile__("" ::: "memory");
        s_rx_settle_suppression_active = 1u;

        s_tx_state = DALI_PHY_TX_DONE;
        BaseType_t higher_prio_woken = pdFALSE;
        if (s_tx_notify_task != NULL) {
            vTaskNotifyGiveFromISR(s_tx_notify_task, &higher_prio_woken);
        }
        s_isr_active = 0u;
        return higher_prio_woken == pdTRUE;
    }

    /* Output current half-bit */
    gpio_set_level(s_tx_gpio,
                   tx_pin_level_for_bus_level(s_tx_half_bits[s_tx_half_bit_idx]));
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
    uint32_t ts_us    = (uint32_t)(ts_raw & RX_TS_MASK);

    if (tx_rx_suppression_active()) {
        g_dali_stats.rx_self_echo_suppressed++;
        return;
    }

    if (rx_settle_suppression_active(ts_us)) {
        g_dali_stats.rx_settle_suppressed++;
        return;
    }

    uint32_t level    = (uint32_t)rx_bus_level_from_pin();
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
    s_tx_gpio = (gpio_num_t)tx_gpio;
    s_rx_gpio = (gpio_num_t)rx_gpio;

    memset(&g_dali_stats, 0, sizeof(g_dali_stats));

    dali_rb_init(&s_rx_rb, s_rx_edge_buf, DALI_RX_EDGE_BUFFER_SIZE);

    s_tx_state          = DALI_PHY_TX_IDLE;
    s_tx_tick_count     = 0u;
    s_tx_half_bit_idx   = 0u;
    s_tx_total_half_bits = 0u;
    s_isr_active        = 0u;
    s_rx_settle_suppression_active = 0u;
    s_rx_suppress_until_us         = 0u;
    s_rx_in_frame       = false;
    s_rx_interval_count = 0u;
    s_rx_edge_count     = 0u;
    s_rx_last_edge_level = 0u;
    rx_debug_clear();

#ifndef DALI_HOST_BUILD
    /* Configure TX GPIO: output, default to logical DALI bus idle/high. */
    gpio_config_t tx_cfg = {
        .pin_bit_mask = (1ULL << tx_gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&tx_cfg);
    gpio_set_level(tx_gpio, tx_pin_level_for_bus_level(1u));

    /* Configure RX GPIO: input, interrupt on both edges */
    gpio_config_t rx_cfg = {
        .pin_bit_mask = (1ULL << rx_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&rx_cfg);
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %d", isr_ret);
        return DALI_ERR_INVALID;
    }
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
    if (frame == NULL || frame->bit_length == 0u ||
        frame->bit_length > DALI_MAX_FRAME_BITS) {
        return DALI_ERR_INVALID;
    }
    if (s_tx_state != DALI_PHY_TX_IDLE) {
        return DALI_ERR_BUSY;
    }
#ifndef DALI_HOST_BUILD
    DaliError idle_err = wait_for_bus_idle_before_tx();
    if (idle_err != DALI_OK) {
        return idle_err;
    }
#endif

    /* Pre-encode the frame into the half-bit buffer */
    uint8_t len = dali_phy_encode_manchester(frame, s_tx_half_bits,
                                             DALI_TX_HALF_BIT_BUFFER_SIZE);
    if (len == 0u) {
        return DALI_ERR_INVALID;
    }

    s_tx_total_half_bits = len;
    s_tx_half_bit_idx    = 0u;
    s_tx_tick_count      = 0u;
    s_tx_state           = DALI_PHY_TX_BUSY;

    ESP_LOGD(TAG, "TX start: 0x%0*" PRIx32 " (%d-bit)",
             (frame->bit_length + 3) / 4, frame->data, (int)frame->bit_length);

#ifndef DALI_HOST_BUILD
    s_tx_notify_task = xTaskGetCurrentTaskHandle();
    gptimer_set_raw_count(s_gptimer, 0);
    gptimer_start(s_gptimer);

    /* Wait for TX_DONE notification from ISR, with generous timeout */
    uint32_t timeout_ticks = pdMS_TO_TICKS(
        ((uint32_t)(1u + frame->bit_length + 2u) * DALI_BIT_US) / 1000u + 20u);
    if (ulTaskNotifyTake(pdTRUE, timeout_ticks) == 0u) {
        gptimer_stop(s_gptimer);
        gpio_set_level(s_tx_gpio, tx_pin_level_for_bus_level(1u));
        s_tx_state = DALI_PHY_TX_IDLE;
        ESP_LOGW(TAG, "TX timeout");
        return DALI_ERR_TIMEOUT;
    }

    gptimer_stop(s_gptimer);
    s_tx_state = DALI_PHY_TX_IDLE;
    /* Settle suppression was armed in the TX ISR at the precise TX-done moment. */
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
     * past a legal full-bit interval after the last edge, the frame is
     * complete and we attempt Manchester decode.
     *
     * First edge received starts the frame; its timestamp becomes the
     * reference — no interval is generated for it because we need two
     * edges to compute a gap.
     */
    uint32_t entry;
    while (dali_rb_pop(&s_rx_rb, &entry) == DALI_OK) {
        uint32_t ts_us = entry >> 1u;
        uint8_t  level = (uint8_t)(entry & 0x01u);
        if (!s_rx_in_frame) {
            if (level != 0u) {
                g_dali_stats.rx_glitch_drops++;
                continue;
            }
            /* First edge: leading start edge into logical LOW. */
            s_rx_in_frame        = true;
            s_rx_interval_count  = 0u;
            s_rx_edge_count      = 1u;
            s_rx_edge_levels[0]  = level;
            s_rx_last_edge_level = level;
            s_rx_last_edge_ts_us = ts_us;
        } else {
            /* Compute interval; handle 31-bit counter wrap. */
            uint32_t iv = (ts_us - s_rx_last_edge_ts_us) & 0x7FFFFFFFu;
            if (iv > RX_FRAME_GAP_MIN_US) {
                complete_rx_frame();
                if (level != 0u) {
                    g_dali_stats.rx_glitch_drops++;
                    continue;
                }
                s_rx_in_frame        = true;
                s_rx_interval_count  = 0u;
                s_rx_edge_count      = 1u;
                s_rx_edge_levels[0]  = level;
                s_rx_last_edge_level = level;
                s_rx_last_edge_ts_us = ts_us;
                continue;
            }
            if (level == s_rx_last_edge_level || iv < RX_GLITCH_MIN_US) {
                g_dali_stats.rx_glitch_drops++;
                continue;
            }
            s_rx_last_edge_ts_us = ts_us;
            s_rx_last_edge_level = level;
            if (s_rx_interval_count < DALI_PHY_RXDEBUG_MAX_INTERVALS &&
                s_rx_edge_count < DALI_PHY_RXDEBUG_MAX_EDGES) {
                s_rx_intervals[s_rx_interval_count++] = iv;
                s_rx_edge_levels[s_rx_edge_count++] = level;
            } else {
                /* Interval buffer overflow — discard this frame. */
                rx_debug_store(DALI_ERR_OVERFLOW);
                s_rx_in_frame       = false;
                s_rx_interval_count = 0u;
                s_rx_edge_count     = 0u;
                s_rx_last_edge_level = 0u;
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
        if (silence >= RX_FRAME_GAP_MIN_US) {
            complete_rx_frame();
        }
#else
        /* Host build: no real time source; just drain. */
        complete_rx_frame();
#endif
    }
}

DaliError dali_phy_reset(void)
{
#ifndef DALI_HOST_BUILD
    gptimer_stop(s_gptimer);
    gpio_set_level(s_tx_gpio, tx_pin_level_for_bus_level(1u));
#endif
    s_tx_state           = DALI_PHY_TX_IDLE;
    s_tx_half_bit_idx    = 0u;
    s_tx_total_half_bits = 0u;
    s_tx_tick_count      = 0u;
    s_isr_active         = 0u;
    s_rx_settle_suppression_active = 0u;
    s_rx_suppress_until_us         = 0u;
#ifndef DALI_HOST_BUILD
    s_tx_notify_task     = NULL;
    gpio_intr_disable(s_rx_gpio);
#endif
    dali_rb_clear(&s_rx_rb);
    s_rx_in_frame        = false;
    s_rx_interval_count  = 0u;
    s_rx_edge_count      = 0u;
    s_rx_last_edge_level = 0u;
#ifndef DALI_HOST_BUILD
    gpio_intr_enable(s_rx_gpio);
#endif
    rx_debug_clear();
    ESP_LOGD(TAG, "PHY reset");
    return DALI_OK;
}

DaliError dali_phy_read_rx_level(uint8_t *level_out)
{
    if (level_out == NULL) {
        return DALI_ERR_INVALID;
    }

#ifndef DALI_HOST_BUILD
    *level_out = rx_bus_level_from_pin();
    return DALI_OK;
#else
    *level_out = 0u;
    return DALI_ERR_INVALID;
#endif
}

DaliError dali_phy_get_rx_debug(DaliPhyRxDebugSnapshot *snapshot_out)
{
    if (snapshot_out == NULL) {
        return DALI_ERR_INVALID;
    }

    RX_DEBUG_ENTER();
    *snapshot_out = s_rx_debug_snapshot;
    RX_DEBUG_EXIT();
    return DALI_OK;
}
