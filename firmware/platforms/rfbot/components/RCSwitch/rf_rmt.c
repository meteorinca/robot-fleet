#include "RCSwitch.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

#define RX_SYMBOLS 192
#define RX_TAIL_PULSES 68

static rmt_channel_handle_t s_tx, s_rx;
static rmt_encoder_handle_t s_encoder;
static SemaphoreHandle_t s_transport_mutex;
static QueueHandle_t s_rx_queue;
static rmt_symbol_word_t s_rx_symbols[RX_SYMBOLS];
static rf_pulse_t s_tail[RX_TAIL_PULSES];
static size_t s_tail_count;
static rf_pulse_t s_pending[RX_TAIL_PULSES + RX_SYMBOLS * 2];
static size_t s_pending_count, s_pending_offset;
static unsigned s_pending_generation;
static volatile unsigned s_generation;
static unsigned s_capture_sequence;

typedef struct {
    rmt_symbol_word_t symbols[RX_SYMBOLS];
    size_t count;
    unsigned generation;
    unsigned sequence;
    bool last;
} rx_capture_t;
static rx_capture_t s_isr_capture, s_isr_discard;

// C3's hardware filter is clocked at 80 MHz and has an 8-bit threshold:
// 3 us is supported; a 35 us filter would fail driver validation.
static const rmt_receive_config_t s_rx_config = {
    .signal_range_min_ns = 3000,
    .signal_range_max_ns = 30000000,
    .flags.en_partial_rx = true,
};

extern volatile uint32_t rf_isr_edge_count;

static bool rx_done(rmt_channel_handle_t channel,
                    const rmt_rx_done_event_data_t *event, void *ctx) {
    // Copy before the driver reuses its partial receive buffer. Never decode
    // or log here. The bounded queue drops captures if the consumer is busy.
    rx_capture_t *capture = &s_isr_capture;
    capture->count = event->num_symbols;
    capture->generation = s_generation;
    capture->sequence = ++s_capture_sequence;
    capture->last = event->flags.is_last;
    if (capture->count > RX_SYMBOLS) capture->count = RX_SYMBOLS;
    memcpy(capture->symbols, event->received_symbols,
           capture->count * sizeof(rmt_symbol_word_t));
    rf_isr_edge_count += capture->count * 2;
    BaseType_t wake = pdFALSE;
    if (xQueueSendFromISR(s_rx_queue, capture, &wake) != pdTRUE) {
        // A final event must reach the task to restart reception. Remove the
        // oldest queued capture, then retain this one (including its gap).
        xQueueReceiveFromISR(s_rx_queue, &s_isr_discard, &wake);
        xQueueSendFromISR(s_rx_queue, capture, &wake);
    }
    return wake == pdTRUE;
}

void rf_rmt_tx_init(int pin) {
    s_transport_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_transport_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    rmt_tx_channel_config_t config = {
        .gpio_num = pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&config, &s_tx));
    rmt_copy_encoder_config_t encoder = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder, &s_encoder));
    ESP_ERROR_CHECK(rmt_enable(s_tx));
}

esp_err_t enableReceive(RCSWITCH_t *rc, int pin) {
    xSemaphoreTake(s_transport_mutex, portMAX_DELAY);
    s_rx_queue = xQueueCreate(4, sizeof(rx_capture_t));
    ESP_ERROR_CHECK(s_rx_queue ? ESP_OK : ESP_ERR_NO_MEM);
    // Use both RX memory blocks to halve refill interrupt frequency on C3.
    rmt_rx_channel_config_t config = {
        .gpio_num = pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 96,
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&config, &s_rx));
    gpio_pulldown_en(pin);
    gpio_pullup_dis(pin);
    rmt_rx_event_callbacks_t callbacks = {.on_recv_done = rx_done};
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(s_rx, &callbacks, NULL));
    ESP_ERROR_CHECK(rmt_enable(s_rx));
    rc->nReceiverInterrupt = pin;
    esp_err_t err = rmt_receive(s_rx, s_rx_symbols, sizeof(s_rx_symbols), &s_rx_config);
    xSemaphoreGive(s_transport_mutex);
    return err;
}

static bool decode_pending(RCSWITCH_t *rc) {
    if (s_pending_generation != s_generation) s_pending_count = 0;
    if (s_pending_offset >= s_pending_count) return false;
    rf_decoded_t decoded;
    if (!rf_decode(s_pending + s_pending_offset, s_pending_count - s_pending_offset, &decoded)) {
        s_pending_count = 0;
        return false;
    }
    // Keep the trailing sync as the leading sync of the next repeated frame.
    s_pending_offset += decoded.consumed - 2;
    rc->nReceivedValue = decoded.code;
    rc->nReceivedBitlength = decoded.bits;
    rc->nReceivedDelay = decoded.pulse;
    rc->nReceivedProtocol = decoded.protocol;
    return true;
}

bool receiveCode(RCSWITCH_t *rc, TickType_t timeout) {
    static unsigned last_sequence, last_generation;
    if (decode_pending(rc)) return true;
    rx_capture_t capture;
    if (xQueueReceive(s_rx_queue, &capture, timeout) != pdTRUE) return false;
    xSemaphoreTake(s_transport_mutex, portMAX_DELAY);
    if (capture.generation != s_generation) {
        xSemaphoreGive(s_transport_mutex);
        return false;
    }
    if (capture.last) {
        ESP_ERROR_CHECK(rmt_receive(s_rx, s_rx_symbols, sizeof(s_rx_symbols), &s_rx_config));
    }
    xSemaphoreGive(s_transport_mutex);

    if (capture.sequence != last_sequence + 1 || capture.generation != last_generation) s_tail_count = 0;
    last_sequence = capture.sequence;
    last_generation = capture.generation;

    rf_pulse_t *pulses = s_pending;
    size_t count = s_tail_count;
    memcpy(pulses, s_tail, count * sizeof(*pulses));
    for (size_t i = 0; i < capture.count; ++i) {
        rmt_symbol_word_t symbol = capture.symbols[i];
        if (symbol.duration0) pulses[count++] = (rf_pulse_t){symbol.duration0, symbol.level0};
        if (symbol.duration1) pulses[count++] = (rf_pulse_t){symbol.duration1, symbol.level1};
    }
    s_tail_count = capture.last ? 0 : (count < RX_TAIL_PULSES ? count : RX_TAIL_PULSES);
    memcpy(s_tail, pulses + count - s_tail_count, s_tail_count * sizeof(*pulses));
    s_pending_count = count;
    s_pending_offset = 0;
    s_pending_generation = capture.generation;
    return decode_pending(rc);
}

esp_err_t sendCode(RCSWITCH_t *rc, unsigned long code, unsigned int length) {
    if (length < 1 || length > RF_MAX_BITS || rc->nRepeatTransmit < 1) return ESP_ERR_INVALID_ARG;
    rmt_symbol_word_t symbols[RF_MAX_BITS + 1];
    for (unsigned i = 0; i <= length; ++i) {
        HighLow factors = i == length ? rc->protocol.syncFactor :
            ((code & (UINT32_C(1) << (length-1-i))) ? rc->protocol.one : rc->protocol.zero);
        unsigned high = rc->protocol.pulseLength * factors.high;
        unsigned low = rc->protocol.pulseLength * factors.low;
        if (!high || !low || high > 32767 || low > 32767) return ESP_ERR_INVALID_ARG;
        symbols[i] = (rmt_symbol_word_t){
            .duration0 = high, .level0 = !rc->protocol.invertedSignal,
            .duration1 = low, .level1 = rc->protocol.invertedSignal,
        };
    }

    xSemaphoreTake(s_transport_mutex, portMAX_DELAY);
    // Pause the actual RX channel, independent of the TX protocol structure.
    // Increment the generation so queued captures cannot become TX echoes.
    if (s_rx) {
        ESP_ERROR_CHECK(rmt_disable(s_rx));
        ++s_generation;
        xQueueReset(s_rx_queue);
    }
    rmt_transmit_config_t config = {
        .loop_count = rc->nRepeatTransmit,
        .flags.eot_level = 0,
    };
    // C3 hardware loops fit a complete 32-bit frame in its 48-symbol block.
    // The calling task sleeps while RMT emits all repeated pulses.
    esp_err_t err = rmt_transmit(s_tx, s_encoder, symbols, (length+1) * sizeof(*symbols), &config);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(s_tx, -1);
    if (s_rx) {
        ESP_ERROR_CHECK(rmt_enable(s_rx));
        ESP_ERROR_CHECK(rmt_receive(s_rx, s_rx_symbols, sizeof(s_rx_symbols), &s_rx_config));
    }
    xSemaphoreGive(s_transport_mutex);
    return err;
}
