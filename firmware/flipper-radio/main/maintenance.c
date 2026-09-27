/* Application-mode recovery/update over either CDC or MIDI. Never writes the running slot,
 * bootloader or partition table. All commands run serially on the wire reader task. */
#include "maintenance.h"
#include "wire.h"
#include "indicator.h"
#include <string.h>
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { READ = 0x10, HASH, BEGIN, WRITE, COMMIT, ABORT, RESTART };
#define FLASH_SIZE 0x400000U
#define CHUNK 1024U
static esp_ota_handle_t handle;
static const esp_partition_t *target;
static uint32_t expected, written;
static uint8_t expected_hash[32];
static bool updating;

bool maintenance_busy(void) { return updating; }
void maintenance_abort(void) {
    if (updating) esp_ota_abort(handle);
    updating = false; target = NULL; expected = written = 0;
    indicator_operation(LED_IDLE);
}
static uint32_t word(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void answer(uint8_t command, esp_err_t code, const void *data, size_t length) {
    uint8_t head[5] = {command}; memcpy(head + 1, &code, 4);
    wire_send_wait(0x82, head, sizeof(head), data, length, pdMS_TO_TICKS(500));
}
static esp_err_t hash_flash(uint32_t start, uint32_t length, uint8_t out[32]) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) return ESP_FAIL;
    uint8_t block[1024]; size_t size = 0;
    esp_err_t result = ESP_OK;
    for (uint32_t offset = 0; offset < length; offset += sizeof(block)) {
        const size_t count = length - offset < sizeof(block) ? length - offset : sizeof(block);
        result = esp_flash_read(NULL, block, start + offset, count);
        if (result != ESP_OK || psa_hash_update(&op, block, count) != PSA_SUCCESS) { result = ESP_FAIL; break; }
        if (!(offset % 16384)) vTaskDelay(1);
    }
    if (result == ESP_OK && (psa_hash_finish(&op, out, 32, &size) != PSA_SUCCESS || size != 32)) result = ESP_FAIL;
    psa_hash_abort(&op);
    return result;
}
bool maintenance_command(uint8_t command, const uint8_t *p, size_t n, bool idle) {
    if (command < READ || command > RESTART) return false;
    esp_err_t result = ESP_OK;
    uint8_t output[CHUNK]; size_t length = 0;
    if (!idle) { answer(command, ESP_ERR_INVALID_STATE, NULL, 0); return true; }
    switch (command) {
    case READ: {
        if (updating || n != 8) { result = ESP_ERR_INVALID_STATE; break; }
        uint32_t at = word(p), count = word(p + 4);
        if (!count || count > CHUNK || at > FLASH_SIZE || count > FLASH_SIZE - at) { result = ESP_ERR_INVALID_SIZE; break; }
        indicator_operation(LED_BACKUP);
        result = esp_flash_read(NULL, output, at, count); length = result == ESP_OK ? count : 0;
        break;
    }
    case HASH:
        if (updating || n) { result = ESP_ERR_INVALID_STATE; break; }
        indicator_operation(LED_BACKUP);
        result = hash_flash(0, FLASH_SIZE, output); length = result == ESP_OK ? 32 : 0;
        indicator_operation(LED_IDLE);
        break;
    case BEGIN:
        if (updating || n != 36) { result = ESP_ERR_INVALID_STATE; break; }
        target = esp_ota_get_next_update_partition(NULL);
        expected = word(p); written = 0;
        if (!target || !expected || expected > target->size || target == esp_ota_get_running_partition()) { result = ESP_ERR_INVALID_SIZE; break; }
        memcpy(expected_hash, p + 4, 32);
        indicator_operation(LED_UPDATING);
        result = esp_ota_begin(target, expected, &handle);
        updating = result == ESP_OK;
        break;
    case WRITE: {
        if (!updating || n <= 4 || n > CHUNK + 4 || word(p) != written || n - 4 > expected - written) { result = ESP_ERR_INVALID_STATE; break; }
        result = esp_ota_write(handle, p + 4, n - 4);
        if (result == ESP_OK) written += n - 4;
        else maintenance_abort();
        break;
    }
    case COMMIT: {
        if (!updating || n || written != expected) { result = ESP_ERR_INVALID_STATE; break; }
        const esp_partition_t *partition = target;
        result = esp_ota_end(handle); updating = false;
        uint8_t actual[32];
        if (result == ESP_OK) result = hash_flash(partition->address, expected, actual);
        if (result == ESP_OK && memcmp(actual, expected_hash, 32)) result = ESP_ERR_INVALID_CRC;
        if (result == ESP_OK) result = esp_ota_set_boot_partition(partition);
        target = NULL; expected = written = 0;
        indicator_operation(LED_IDLE);
        break;
    }
    case ABORT:
        if (n) result = ESP_ERR_INVALID_ARG;
        else maintenance_abort();
        break;
    case RESTART:
        if (updating || n != 4 || memcmp(p, "BOOT", 4)) { result = ESP_ERR_INVALID_STATE; break; }
        indicator_operation(LED_RESTART);
        if (wire_reset(command, false)) return true;
        result = ESP_ERR_NO_MEM;
        break;
    }
    if (result != ESP_OK) { indicator_error(); if (!updating) indicator_operation(LED_IDLE); }
    answer(command, result, output, length);
    return true;
}
