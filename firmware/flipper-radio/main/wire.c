#include "wire.h"
#include "midi_codec.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#ifdef LS_GPIO
#include "driver/uart.h"
#define GPIO_UART UART_NUM_0
static QueueHandle_t s_uart_events;
#endif

#define MSG_LOG 0x83
#define MSG_CREDIT 0x8B
/* A CREDIT goes out every CREDIT_STEP bytes read, and when the line falls idle. The host keeps
   under half the 16 KB RX ring in flight, so a command waiting on a full Wi-Fi queue stalls the
   host and never overflows the ring. docs/hardware_esp32.md, The serial ceiling. */
#define CREDIT_STEP 1024

typedef struct {
    uint16_t length;
    uint8_t transport;
    uint8_t reset;     /* 1: ROM recovery, 2: restart application, after successful ACK write */
    uint8_t bytes[];   /* type, payload */
} message_t;

/* The queue drops on memory, not on count: 128 entries filled at 96 KB of free heap during a
   Scarlet burst and dropped 337 messages. The floor keeps room for the driver's RX buffers.
   docs/hardware_esp32.md, The serial ceiling. */
#define WIRE_QUEUE_LENGTH 384
#define WIRE_HEAP_FLOOR (64 * 1024)

static QueueHandle_t s_out;
static wire_handler_t s_handler;
static atomic_uint s_dropped, s_rx_bad, s_rx_fifo_ovf, s_rx_buffer_full;
static uint32_t s_consumed, s_credited;   /* the reader task's own; the handler runs on it */
/* Maxima for STATUS: a LOG line is refused at the heap floor, which is when the reader stalls.
   docs/hardware_esp32.md, The serial ceiling. */
static atomic_uint s_refused_heap, s_refused_queue, s_heap_min = UINT32_MAX, s_queue_max;
static atomic_uint s_read_max_us, s_handler_max_us, s_handler_max_type, s_write_max_us;
static atomic_uint s_tx_chunk, s_tx_delay_ticks;
enum { LINK_NONE, LINK_CDC, LINK_MIDI, LINK_GPIO };
static atomic_uint s_transport;

void wire_release_transport(void) { atomic_store(&s_transport, LINK_NONE); }
bool wire_is_gpio(void) { return atomic_load(&s_transport) == LINK_GPIO; }

bool wire_reset(uint8_t command, bool bootloader)
{
    message_t *m = malloc(sizeof(*m) + 6);
    if (!m) return false;
    m->length = 6;
    m->transport = atomic_load(&s_transport);
    m->reset = bootloader ? 1 : 2;
    memset(m->bytes, 0, 6);
    m->bytes[0] = 0x82; /* RESULT and action share one queue entry. */
    m->bytes[1] = command;
    if (xQueueSend(s_out, &m, pdMS_TO_TICKS(100)) == pdTRUE) return true;
    free(m);
    return false;
}

bool wire_set_tx_pacing(uint16_t chunk, uint16_t delay_ms)
{
    if (!chunk && !delay_ms) {
        atomic_store(&s_tx_chunk, 0);
        atomic_store(&s_tx_delay_ticks, 0);
        return true;
    }
    if (chunk < 64 || chunk > 512 || delay_ms < 1 || delay_ms > 10) return false;
    uint32_t ticks = pdMS_TO_TICKS(delay_ms);
    atomic_store(&s_tx_delay_ticks, ticks ? ticks : 1);
    atomic_store(&s_tx_chunk, chunk);
    return true;
}

static void raise_max(atomic_uint *max, uint32_t value)
{
    uint32_t seen = atomic_load(max);
    while (value > seen && !atomic_compare_exchange_weak(max, &seen, value)) {}
}

static void lower_min(atomic_uint *min, uint32_t value)
{
    uint32_t seen = atomic_load(min);
    while (value < seen && !atomic_compare_exchange_weak(min, &seen, value)) {}
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = UINT32_MAX;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}

bool wire_send_wait(uint8_t type, const void *head, size_t head_len, const void *body,
                    size_t body_len, uint32_t ticks)
{
    const size_t length = 1 + head_len + body_len;
    if (length > WIRE_MAX_PAYLOAD + 1) return false;
    const unsigned transport = atomic_load(&s_transport);
    if (!transport) return true; /* No host has sent HELLO yet. */
    const uint32_t heap = esp_get_free_heap_size();
    lower_min(&s_heap_min, heap);
    if (heap < WIRE_HEAP_FLOOR + length) { atomic_fetch_add(&s_refused_heap, 1); return false; }
    message_t *m = malloc(sizeof(*m) + length);
    if (!m) { atomic_fetch_add(&s_refused_heap, 1); return false; }
    m->length = length;
    m->transport = transport;
    m->reset = 0;
    m->bytes[0] = type;
    if (head_len) memcpy(m->bytes + 1, head, head_len);
    if (body_len) memcpy(m->bytes + 1 + head_len, body, body_len);
    if (xQueueSend(s_out, &m, ticks) != pdTRUE) {
        free(m);
        atomic_fetch_add(&s_refused_queue, 1);
        return false;
    }
    raise_max(&s_queue_max, uxQueueMessagesWaiting(s_out));
    return true;
}

void wire_send(uint8_t type, const void *head, size_t head_len, const void *body, size_t body_len)
{
    if (!wire_send_wait(type, head, head_len, body, body_len, 0)) atomic_fetch_add(&s_dropped, 1);
}

void wire_log(const char *format, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
    wire_send(MSG_LOG, line, n, NULL, 0);
}

uint32_t wire_dropped(void) { return atomic_load(&s_dropped); }
uint32_t wire_rx_bad(void) { return atomic_load(&s_rx_bad); }
uint32_t wire_rx_fifo_ovf(void) { return atomic_load(&s_rx_fifo_ovf); }
uint32_t wire_rx_buffer_full(void) { return atomic_load(&s_rx_buffer_full); }

int wire_stats(char *text, size_t size)
{
    return snprintf(text, size,
        "refused_heap=%u refused_queue=%u heap_min=%u queue_max=%u read_max_us=%u "
        "handler_max_us=%u handler_max_type=%#x write_max_us=%u",
        atomic_load(&s_refused_heap), atomic_load(&s_refused_queue), atomic_load(&s_heap_min),
        atomic_load(&s_queue_max), atomic_load(&s_read_max_us), atomic_load(&s_handler_max_us),
        atomic_load(&s_handler_max_type), atomic_load(&s_write_max_us));
}

/* A CREDIT jumps the queue and carries the count current when the writer reaches it: behind a
   console burst's RX_ETH it arrived 0.51 s late and the host resynced over no loss.
   docs/hardware_esp32.md, The serial ceiling. */
static atomic_uint s_credit_value;
static atomic_bool s_credit_queued;
static int64_t s_credit_at;   /* the reader's own: when it last repeated an idle count */

static void send_credit(void)
{
    s_credited = s_consumed;
    atomic_store(&s_credit_value, s_credited);
    if (atomic_exchange(&s_credit_queued, true)) return;
    message_t *m = malloc(sizeof(*m) + 5);
    if (m) {
        m->length = 5;
        m->transport = atomic_load(&s_transport);
        m->reset = 0;
        m->bytes[0] = MSG_CREDIT;
        if (xQueueSendToFront(s_out, &m, 0) == pdTRUE) return;
        free(m);
    }
    atomic_store(&s_credit_queued, false);
    atomic_fetch_add(&s_dropped, 1);
}

/* A CREDIT of 0 at once, so the host's window is shut from the first byte after the HELLO. */
void wire_credit_reset(void)
{
    wire_set_tx_pacing(0, 0);
    s_consumed = 0;
    send_credit();
}

/* A zero-length queue entry carrying the rate: the writer switches when it reaches it, so every
   message queued before it (the BAUD RESULT above all) leaves at the old rate. A flag checked on
   an empty queue raced the RESULT while RX_MGMT kept the writer busy. */
void wire_set_baud(uint32_t baud)
{
    message_t *m = malloc(sizeof(*m) + 4);
    if (!m) return;
    m->length = 0;
    m->transport = atomic_load(&s_transport);
    m->reset = 0;
    memcpy(m->bytes, &baud, 4);
    if (xQueueSend(s_out, &m, portMAX_DELAY) != pdTRUE) free(m);
}

static void writer(void *arg)
{
    static uint8_t frame[WIRE_MAX_PAYLOAD + 8], encoded[WIRE_MAX_PAYLOAD + 128];
    for (;;) {
        message_t *m;
        if (xQueueReceive(s_out, &m, portMAX_DELAY) != pdTRUE) continue;
        const size_t n = m->length;
        const unsigned transport = m->transport;
        const unsigned reset = m->reset;
        if (!n) {
            uint32_t baud;
            memcpy(&baud, m->bytes, 4);
            free(m);
            /* The 16 KB TX ring can hold a second and more at 115200; a 100 ms wait switched
               the rate with the RESULT still in it. */
            /* USB CDC baud is advisory; preserve RESULT-before-switch ordering. */
#ifdef LS_GPIO
            if (transport == LINK_GPIO) {
                if (uart_wait_tx_done(GPIO_UART, pdMS_TO_TICKS(2000)) == ESP_OK)
                    uart_set_baudrate(GPIO_UART, baud);
            }
#endif
            (void)baud;
            continue;
        }
        memcpy(frame, m->bytes, n);
        free(m);
        /* uart_write_bytes spins on a full TX ring (IDF 6.1 uart.c:1662) and this task outranks
           the reader on its core, which it starved in the middle of esp_wifi_internal_tx: 111 ms
           sends under a line-rate flood. Sleep until the frame fits. docs/hardware_esp32.md */

        const int64_t waited = esp_timer_get_time();
        raise_max(&s_write_max_us, esp_timer_get_time() - waited);
        if (frame[0] == MSG_CREDIT && n == 5) {
            atomic_store(&s_credit_queued, false);
            const uint32_t credit = atomic_load(&s_credit_value);
            memcpy(frame + 1, &credit, 4);
        }
        const uint32_t crc = crc32(frame, n);
        memcpy(frame + n, &crc, 4);
        size_t out = 1, code_at = 0;
        uint8_t code = 1;
        for (size_t i = 0; i < n + 4; ++i) {
            if (!frame[i]) { encoded[code_at] = code; code_at = out++; code = 1; continue; }
            encoded[out++] = frame[i];
            if (++code == 255) { encoded[code_at] = code; code_at = out++; code = 1; }
        }
        encoded[code_at] = code;
        encoded[out++] = 0;
        static uint8_t midi[LS_MIDI_ENCODED_MAX];
        const uint8_t *bytes = encoded;
        if (transport == LINK_MIDI) {
            out = ls_midi_encode(encoded, out, midi, sizeof(midi));
            bytes = midi;
        }
        if (!transport) continue;
        size_t sent = 0;
        const int64_t write_started = esp_timer_get_time();
#ifdef LS_GPIO
        if (transport == LINK_GPIO) {
            /* Drain first: the UART API spins if its ring is full, and this
               higher-priority writer must not starve the control reader. */
            if (uart_wait_tx_done(GPIO_UART, pdMS_TO_TICKS(500)) == ESP_OK) {
                int wrote = uart_write_bytes(GPIO_UART, bytes, out);
                if (wrote > 0) sent = (size_t)wrote;
            }
            if (sent != out) atomic_fetch_add(&s_dropped, 1);
        }
#endif
        while (transport != LINK_GPIO && sent < out) {
            if (!tud_mounted() || esp_timer_get_time() - write_started > 2000000) {
                atomic_fetch_add(&s_dropped, 1);
                break;
            }
            /* Browser serial receivers can lose parts of large USB bursts on macOS.
               Negotiated pacing preserves framing while giving the host time to drain.
               Native clients retain the original behavior unless they request pacing. */
            const uint32_t limit = atomic_load(&s_tx_chunk);
            size_t count = out - sent;
            if (limit && count > limit) count = limit;
            if (transport == LINK_MIDI) sent += tud_midi_stream_write(0, bytes + sent, count);
            else {
                sent += tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, bytes + sent, count);
                tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
            }
            if (limit) vTaskDelay(atomic_load(&s_tx_delay_ticks) ?: 1);
            else if (sent < out) vTaskDelay(1);
        }
        if (!atomic_load(&s_tx_chunk)) vTaskDelay(1);
        if (reset && sent == out) {
#ifdef LS_GPIO
            if (transport == LINK_GPIO) uart_wait_tx_done(GPIO_UART, pdMS_TO_TICKS(500));
#endif
            if (transport == LINK_CDC) tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(200));
            vTaskDelay(pdMS_TO_TICKS(250));
            /* ROM CDC has different endpoints. Drop the pull-up so hosts discard the
               composite configuration before the digital-core reset enters ROM. */
            tud_disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
            if (reset == 1) {
                REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
                esp_rom_software_reset_system();
            }
            esp_restart();
        }
    }
}

/* A frame that fails here is a command lost between host and board; wire_rx_bad counts them. */
static void deliver(const uint8_t *encoded, size_t used, unsigned transport)
{
    static uint8_t frame[WIRE_MAX_PAYLOAD + 8];
    size_t read = 0, out = 0;
    while (read < used) {
        const uint8_t code = encoded[read++];
        if (!code || read + code - 1 > used || out + code > sizeof(frame)) {
            atomic_fetch_add(&s_rx_bad, 1);
            return;
        }
        for (int i = 1; i < code; ++i) frame[out++] = encoded[read++];
        if (code != 255 && read < used) frame[out++] = 0;
    }
    uint32_t crc;
    if (out < 5 || (memcpy(&crc, frame + out - 4, 4), crc != crc32(frame, out - 4))) {
        atomic_fetch_add(&s_rx_bad, 1);
        return;
    }
    unsigned owner = atomic_load(&s_transport);
    if (!owner && frame[0] == 1) {
        atomic_store(&s_transport, transport);
        owner = transport;
    }
    if (owner != transport) return;
    const int64_t started = esp_timer_get_time();
    s_handler(frame[0], frame + 1, out - 5);
    const int64_t took = esp_timer_get_time() - started;
    if (took > atomic_load(&s_handler_max_us)) {
        atomic_store(&s_handler_max_us, took);
        atomic_store(&s_handler_max_type, frame[0]);
    }
    if (took > 50000) wire_log("slow command 0x%02x: %u ms", frame[0], (unsigned)(took / 1000));
}

typedef struct { uint8_t bytes[WIRE_MAX_PAYLOAD + 32]; size_t used; bool overflow; } input_t;
static input_t s_inputs[4];

static void consume(const uint8_t *bytes, size_t count, unsigned transport)
{
    input_t *input = &s_inputs[transport];
    for (size_t i = 0; i < count; i++) {
        if (atomic_load(&s_transport) == transport) ++s_consumed;
        if (bytes[i]) {
            if (input->used < sizeof(input->bytes)) input->bytes[input->used++] = bytes[i];
            else input->overflow = true;
        } else {
            if (input->used && input->overflow) atomic_fetch_add(&s_rx_bad, 1);
            if (input->used && !input->overflow) deliver(input->bytes, input->used, transport);
            input->used = 0;
            input->overflow = false;
        }
    }
}

static void consume_midi(const uint8_t *bytes, size_t count)
{
    static uint8_t sysex[LS_MIDI_ENCODED_MAX], raw[LS_MIDI_RAW_MAX];
    static size_t used;
    for (size_t i = 0; i < count; i++) {
        const uint8_t b = bytes[i];
        if (b >= 0xf8) continue;
        if (b == 0xf0) { used = 1; sysex[0] = b; continue; }
        if (!used) continue;
        if (b == 0xf7) {
            sysex[used++] = b;
            size_t n = ls_midi_decode(sysex, used, raw, sizeof(raw));
            if (n != SIZE_MAX) consume(raw, n, LINK_MIDI);
            else atomic_fetch_add(&s_rx_bad, 1);
            used = 0;
        } else if (b >= 0x80 || used >= sizeof(sysex) - 1) {
            used = 0;
            atomic_fetch_add(&s_rx_bad, 1);
        } else sysex[used++] = b;
    }
}

/* Native USB transport for the single-core ESP32-S2. Both wire tasks yield to
   the higher-priority Wi-Fi task; framing and receive credits match the host. */
static void reader(void *arg)
{
    xTaskCreatePinnedToCore(writer, "wire_tx", 4096, NULL, 18, NULL, 0);
    static uint8_t chunk[512];
    for (;;) {
        const int64_t turn = esp_timer_get_time();
        const unsigned owner = atomic_load(&s_transport);
        if ((owner == LINK_CDC || owner == LINK_MIDI) &&
            (!tud_mounted() || (owner == LINK_CDC && !tud_cdc_connected()))) {
            /* A vanished host must not leave a live radio session available to a new owner. */
            s_handler(5, NULL, 0);
            wire_release_transport();
            memset(s_inputs, 0, sizeof(s_inputs));
        }
        size_t received = 0;
        tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, chunk, sizeof(chunk), &received);
        consume(chunk, received, LINK_CDC);
        const size_t midi_count = tud_midi_stream_read(chunk, sizeof(chunk));
        consume_midi(chunk, midi_count);
        int n = (int)(received + midi_count);
#ifdef LS_GPIO
        uart_event_t event;
        while (xQueueReceive(s_uart_events, &event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
                if (event.type == UART_FIFO_OVF) atomic_fetch_add(&s_rx_fifo_ovf, 1);
                else atomic_fetch_add(&s_rx_buffer_full, 1);
                /* Keep discarding until a frame boundary after byte loss. */
                s_inputs[LINK_GPIO].overflow = true;
            }
        }
        int uart_count = uart_read_bytes(GPIO_UART, chunk, sizeof(chunk), 0);
        if (uart_count > 0) { consume(chunk, (size_t)uart_count, LINK_GPIO); n += uart_count; }
#endif
        if (n <= 0) vTaskDelay(1);
        raise_max(&s_read_max_us, esp_timer_get_time() - turn);
        /* Idle, the reader repeats its count every 100 ms: a host whose window stays shut under
           a repeated count knows the rest was lost on the line, and a silent board is busy. */
        if (atomic_load(&s_transport) && n <= 0 && (s_consumed != s_credited || turn - s_credit_at > 100000)) {
            s_credit_at = turn;
            send_credit();
        }
        if (s_consumed - s_credited >= CREDIT_STEP) send_credit();
        const int64_t held = esp_timer_get_time() - turn;
        if (held > 100000) wire_log("reader held %u ms", (unsigned)(held / 1000));
    }
}

void wire_start(wire_handler_t handler)
{
    s_handler = handler;
    s_out = xQueueCreate(WIRE_QUEUE_LENGTH, sizeof(message_t *));
#ifdef LS_GPIO
    /* Official devboard schematic: ESP TXD0 -> Flipper RX (14),
       ESP RXD0 <- Flipper TX (13). ESP32-S2 TXD0/RXD0 are GPIO43/44. */
    const uart_config_t uart = {.baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_param_config(GPIO_UART, &uart));
    ESP_ERROR_CHECK(uart_set_pin(GPIO_UART, 43, 44, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(GPIO_UART, 4096, 4096, 20, &s_uart_events, 0));
    ESP_ERROR_CHECK(uart_set_rx_full_threshold(GPIO_UART, 64));
#endif
    /* CDC uses two interfaces, MIDI uses AudioControl + MIDIStreaming. */
    static const uint8_t descriptor[] = {
        TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN, 0, 250),
        TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
        TUD_MIDI_DESCRIPTOR(2, 5, 0x03, 0x83, 64),
    };
    static const char language[] = {0x09, 0x04};
    static const char *strings[] = {language, "Link Studio", "Link Studio Radio", "ZA-Flipper-S2", "Serial radio", "Link Studio MIDI"};
    tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG();
    usb.descriptor.full_speed_config = descriptor;
    usb.descriptor.string = strings;
    usb.descriptor.string_count = 6;
    ESP_ERROR_CHECK(tinyusb_driver_install(&usb));
    const tinyusb_config_cdcacm_t acm = {.cdc_port = TINYUSB_CDC_ACM_0};
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm));
    xTaskCreatePinnedToCore(reader, "wire_rx", 6144, NULL, 17, NULL, 0);
}
