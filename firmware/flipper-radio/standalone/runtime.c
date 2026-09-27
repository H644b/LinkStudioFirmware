#include "runtime.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "indicator.h"
#include "ls_alloc.h"
#include "ls_control.h"
#include "ls_host.h"
#include "ls_host_profiles.h"
#include "ls_recipe.h"
#include "ls_wire.h"
#include "wire.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { RX_ETHERNET = 1, RX_MONITOR = 2, RX_LEFT = 3 };
typedef struct {
    uint16_t size;
    uint8_t type;
    uint8_t data[1600];
} Message;
static LsRadio radio;
static QueueHandle_t commands, frames;
static StaticQueue_t commands_control, frames_control;
static atomic_bool active, ready;
static atomic_uint dropped;
static atomic_uint stack_free, session_outstanding, session_error, trade_count;
/* All state below belongs only to worker(). */
static LsHost* host;
static uint8_t *recipe, *upload;
static size_t recipe_size, upload_size, upload_used;
static uint32_t upload_crc, heartbeat, last_status, completed;
static bool upload_open, controller_lost;
static int32_t last_error;
static uint8_t last_state;
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void result(uint8_t command, int32_t error) {
    uint8_t reply[5] = {command};
    ls_write32(reply + 1, (uint32_t)error);
    wire_send(LsMsgResult, reply, sizeof(reply), NULL, 0);
    if (error)
        indicator_error();
}
static void status(void) {
    uint8_t out[44] = {1, LsStateIdle, LsFlagSafeToStop};
    if (recipe_size)
        out[2] |= LsFlagRecipe;
    ls_write32(out + 4, completed);
    ls_write32(out + 8, last_error);
    if (host)
        ls_host_status(host, out);
    LsSessionStatus diagnostics = {0};
    if (host)
        ls_host_session_status(host, &diagnostics);
    atomic_store(&session_outstanding, diagnostics.outstanding);
    atomic_store(&session_error, ls_read32(out + 8));
    atomic_store(&trade_count, ls_read32(out + 4));
    atomic_store(&stack_free, uxTaskGetStackHighWaterMark(NULL));
    if (out[1] != last_state) {
        last_state = out[1];
        LedState led = LED_IDLE;
        switch (out[1]) {
        case LsStateHosting:
            led = LED_SEARCHING;
            break;
        case LsStateConnected:
            led = LED_CONNECTED;
            break;
        case LsStateOffered:
            led = LED_OFFERED;
            break;
        case LsStateSaving:
        case LsStateStopping:
            led = LED_SAVING;
            break;
        case LsStateComplete:
            led = LED_COMPLETE;
            break;
        case LsStateError:
            led = LED_ERROR;
            break;
        }
        indicator_base(led);
    }
    wire_send(LsMsgStatus, out, sizeof(out), NULL, 0);
    last_status = now_ms();
}
static bool ethernet(void* context, const uint8_t* data, size_t size) {
    (void)context;
    return radio.ethernet(data, size);
}
static bool raw(void* context, const uint8_t* data, size_t size) {
    (void)context;
    return radio.raw(data, size);
}
static esp_err_t start(const uint8_t* p, size_t size) {
    if (host || !recipe_size || size != 11)
        return ESP_ERR_INVALID_STATE;
    LsHostConfig c = {0};
    const LsHostProfile* profile =
        ls_host_profiles + esp_random() % (sizeof(ls_host_profiles) / sizeof(ls_host_profiles[0]));
    memcpy(c.network.ssid, profile->ssid, 16);
    memcpy(c.network.server_random, profile->server_random, 16);
    memcpy(c.network.advertise_key, profile->advertise_key, 16);
    memcpy(c.data_key, profile->data_key, 16);
    memcpy(c.network.code, p, 8);
    c.network.channel = p[8];
    c.network.app_version = ls_read16(p + 9);
    if (!c.network.app_version)
        c.network.app_version = LS_PROFILE_APP_VERSION;
    c.network.subnet = 1 + esp_random() % 127;
    esp_fill_random(c.network.mac, 6);
    c.network.mac[0] = (c.network.mac[0] & 0xfc) | 2;
    c.network.nonce = esp_random();
    c.variable = 2 + esp_random() % 0xffee;
    esp_fill_random(c.join_random, sizeof(c.join_random));
    esp_fill_random(&c.first_nonce, sizeof(c.first_nonce));
    c.first_nonce &= UINT64_MAX >> 1;
    if (!ls_ldn_valid_config(&c.network))
        return ESP_ERR_INVALID_ARG;
    host = ls_host_alloc(&c, recipe, recipe_size, now_ms(), raw, ethernet, NULL);
    if (!host)
        return ESP_ERR_NO_MEM;
    uint8_t ap[57] = {0};
    ap[0] = c.network.channel;
    memcpy(ap + 1, c.network.mac, 6);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        ap[7 + 2 * i] = hex[c.network.ssid[i] >> 4];
        ap[8 + 2 * i] = hex[c.network.ssid[i] & 15];
    }
    memcpy(ap + 39, c.data_key, 16);
    ap[55] = 1;
    ap[56] = 4; /* no serial data trace */
    esp_err_t error = radio.start(ap, sizeof(ap));
    if (error) {
        ls_host_free(host);
        host = NULL;
        radio.stop();
        return error;
    }
    completed = 0;
    last_error = 0;
    controller_lost = false;
    heartbeat = now_ms();
    return ESP_OK;
}
static void command(const Message* m) {
    esp_err_t error = ESP_OK;
    const uint8_t* p = m->data;
    size_t n = m->size;
    heartbeat = now_ms();
    switch (m->type) {
    case LsCmdDescribe:
    case LsCmdStatus:
        if (n)
            result(m->type, ESP_ERR_INVALID_SIZE);
        else
            status();
        return;
    case LsCmdPing:
        result(m->type, n ? ESP_ERR_INVALID_SIZE : ESP_OK);
        return;
    case LsCmdRecipeBegin:
        if (host) {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        // Any attempted replacement invalidates the old selection immediately.
        recipe_size = 0;
        upload_open = false;
        upload_used = 0;
        if (n != 6 || !ls_read16(p) || ls_read16(p) > LS_RECIPE_MAX) {
            error = ESP_ERR_INVALID_SIZE;
            break;
        }
        upload_size = ls_read16(p);
        upload_crc = ls_read32(p + 2);
        upload_open = true;
        break;
    case LsCmdRecipeChunk:
        if (host || !upload_open) {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        if (n < 3 || n > 514 || ls_read16(p) != upload_used || n - 2 > upload_size - upload_used) {
            upload_open = false;
            error = ESP_ERR_INVALID_SIZE;
            break;
        }
        memcpy(upload + upload_used, p + 2, n - 2);
        upload_used += n - 2;
        break;
    case LsCmdRecipeCommit: {
        LsRecipe parsed;
        if (n || host || !upload_open || upload_used != upload_size ||
            ls_crc32(upload, upload_size) != upload_crc ||
            !ls_recipe_parse(upload, upload_size, &parsed)) {
            upload_open = false;
            error = ESP_ERR_INVALID_ARG;
            break;
        }
        memcpy(recipe, upload, upload_size);
        recipe_size = upload_size;
        upload_open = false;
        break;
    }
    case LsCmdStart:
        error = start(p, n);
        if (error) {
            atomic_store(&active, false);
            last_error = error;
        }
        break;
    case LsCmdPreview:
        if (n || !ls_host_preview(host, heartbeat))
            error = ESP_ERR_INVALID_STATE;
        break;
    case LsCmdOffer:
        if (n || !ls_host_offer(host, heartbeat))
            error = ESP_ERR_INVALID_STATE;
        break;
    case LsCmdCancel:
        if (n || !ls_host_cancel(host, heartbeat))
            error = ESP_ERR_INVALID_STATE;
        break;
    case LsCmdStopSafe:
        if (n)
            error = ESP_ERR_INVALID_SIZE;
        else if (host)
            ls_host_stop(host);
        break;
    default:
        error = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    result(m->type, error);
    status();
}
static void worker(void* context) {
    Message* m = context;
    for (;;) {
        for (unsigned i = 0; i < 4 && xQueueReceive(commands, m, 0) == pdTRUE; ++i)
            command(m);
        for (unsigned i = 0; i < 16 && xQueueReceive(frames, m, 0) == pdTRUE; ++i) {
            if (!host)
                continue;
            if (m->type == RX_ETHERNET)
                ls_host_ethernet(host, m->data, m->size, now_ms());
            else if (m->type == RX_MONITOR)
                ls_host_monitor(host, m->data, m->size, now_ms());
            else if (m->type == RX_LEFT)
                ls_host_peer_left(host, m->data);
        }
        if (host) {
            uint32_t now = now_ms();
            if (!controller_lost && (uint32_t)(now - heartbeat) > 5000) {
                controller_lost = true;
                ls_host_stop(host);
            }
            ls_host_tick(host, now);
            if (ls_host_finished(host)) {
                uint8_t final[44];
                ls_host_status(host, final);
                completed = ls_read32(final + 4);
                last_error = (int32_t)ls_read32(final + 8);
                radio.stop();
                ls_host_free(host);
                host = NULL;
                xQueueReset(frames);
                atomic_store(&active, false);
                status();
                if (controller_lost)
                    wire_release_transport();
            } else if ((uint32_t)(now - last_status) >= 200)
                status();
        }
        vTaskDelay(1);
    }
}
bool ls_runtime_init(const LsRadio* ops) {
    if (!ops || !ops->start || !ops->stop || !ops->ethernet || !ops->raw)
        return false;
    uint8_t *command_bytes = ls_alloc(8 * sizeof(Message)),
            *frame_bytes = ls_alloc(32 * sizeof(Message));
    recipe = ls_alloc(LS_RECIPE_MAX);
    upload = ls_alloc(LS_RECIPE_MAX);
    Message* message = ls_alloc(sizeof(Message));
    if (!command_bytes || !frame_bytes || !recipe || !upload || !message) {
        free(command_bytes);
        free(frame_bytes);
        free(recipe);
        free(upload);
        free(message);
        return false;
    }
    commands = xQueueCreateStatic(8, sizeof(Message), command_bytes, &commands_control);
    frames = xQueueCreateStatic(32, sizeof(Message), frame_bytes, &frames_control);
    radio = *ops;
    atomic_store(&ready, true);
    if (xTaskCreatePinnedToCore(worker, "ldn_host", 12288, message, 5, NULL, 0) != pdPASS) {
        atomic_store(&ready, false);
        vQueueDelete(commands);
        vQueueDelete(frames);
        free(command_bytes);
        free(frame_bytes);
        free(recipe);
        free(upload);
        free(message);
        return false;
    }
    return true;
}
bool ls_runtime_ready(void) { return atomic_load(&ready); }
bool ls_runtime_active(void) { return atomic_load(&active); }
int ls_runtime_stats(char* text, size_t size) {
    return snprintf(text, size,
                    "standalone=%d host_active=%d host_drop=%u host_pending=%u host_error=%u "
                    "host_trades=%u host_stack_free=%u",
                    ls_runtime_ready(), ls_runtime_active(), atomic_load(&dropped),
                    atomic_load(&session_outstanding), atomic_load(&session_error),
                    atomic_load(&trade_count), atomic_load(&stack_free));
}
bool ls_runtime_command(uint8_t type, const uint8_t* p, size_t n, bool idle) {
    if (type < LsCmdDescribe || type > LsCmdPing)
        return false;
    if (!ls_runtime_ready() || !wire_is_gpio() || (!idle && !ls_runtime_active())) {
        result(type, ESP_ERR_INVALID_STATE);
        return true;
    }
    if (n > 1600) {
        result(type, ESP_ERR_INVALID_SIZE);
        return true;
    }
    bool reserved = false;
    if (type == LsCmdStart) {
        bool expected = false;
        if (!atomic_compare_exchange_strong(&active, &expected, true)) {
            result(type, ESP_ERR_INVALID_STATE);
            return true;
        }
        reserved = true;
    }
    Message m = {.type = type, .size = n};
    if (n)
        memcpy(m.data, p, n);
    if (xQueueSend(commands, &m, 0) != pdTRUE) {
        if (reserved)
            atomic_store(&active, false);
        result(type, ESP_ERR_NO_MEM);
    }
    return true;
}
static void receive(uint8_t type, const uint8_t* p, size_t n) {
    if (!ls_runtime_active() || !p || n > 1600)
        return;
    Message m = {.type = type, .size = n};
    memcpy(m.data, p, n);
    if (xQueueSend(frames, &m, 0) != pdTRUE)
        atomic_fetch_add(&dropped, 1);
}
void ls_runtime_ethernet(const uint8_t* p, size_t n) { receive(RX_ETHERNET, p, n); }
void ls_runtime_monitor(const uint8_t* p, size_t n) { receive(RX_MONITOR, p, n); }
void ls_runtime_peer_left(const uint8_t mac[6]) { receive(RX_LEFT, mac, 6); }
