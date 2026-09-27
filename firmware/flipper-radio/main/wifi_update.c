/* Home Wi-Fi is used only while idle. The running OTA slot is never overwritten. */
#include "wifi_update.h"
#include "update_contract.h"
#include "wifi_profiles.h"
#include "indicator.h"
#include "wire.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "nvs.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef LS_BUILD_ID
#define LS_BUILD_ID "development"
#endif
#define RELEASE_URL "https://api.github.com/repos/" UPDATE_REPOSITORY "/releases/latest"
#define ASSET_URL "https://api.github.com/repos/" UPDATE_REPOSITORY "/releases/assets/"
#define JSON_MAX 49152
static atomic_bool busy;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t status_bytes[120] = {1};
#define SCAN_MAX 16
/* Network list v1: 8-byte header, then 36-byte records. Credentials never leave NVS. */
static uint8_t scan_entries[SCAN_MAX][36];
static uint8_t scan_count, scan_state;
static esp_err_t scan_error;
enum { UpdateIdle, UpdateConnecting, UpdateChecking, UpdateDownloading, UpdateVerifying, UpdateRestart, UpdateError };
typedef struct {
    wifi_config_t wifi;
    esp_err_t (*prepare)(void);
    void (*restore)(void);
    bool remember;
    WifiProfile profile;
} Job;
static void erase(void *data, size_t size) {
    volatile uint8_t *p = data;
    while (size--) *p++ = 0;
}
static esp_err_t profiles_load(WifiProfiles *profiles) {
    memset(profiles, 0, sizeof(*profiles));
    nvs_handle_t handle;
    esp_err_t r = nvs_open("ls_wifi", NVS_READONLY, &handle);
    if (r == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (r != ESP_OK) return r;
    size_t size = sizeof(*profiles);
    r = nvs_get_blob(handle, "profiles_v1", profiles, &size);
    nvs_close(handle);
    if (r == ESP_ERR_NVS_NOT_FOUND) { memset(profiles, 0, sizeof(*profiles)); return ESP_OK; }
    if (r != ESP_OK || size != sizeof(*profiles) || !wifi_profiles_valid(profiles)) {
        erase(profiles, sizeof(*profiles));
        return r != ESP_OK ? r : ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
static esp_err_t profiles_store(const WifiProfiles *profiles) {
    nvs_handle_t handle;
    esp_err_t r = nvs_open("ls_wifi", NVS_READWRITE, &handle);
    if (r != ESP_OK) return r;
    r = nvs_set_blob(handle, "profiles_v1", profiles, sizeof(*profiles));
    if (r == ESP_OK) r = nvs_commit(handle);
    nvs_close(handle);
    return r;
}
static esp_err_t remember_profile(const WifiProfile *profile) {
    WifiProfiles profiles;
    esp_err_t r = profiles_load(&profiles);
    if (r == ESP_OK && wifi_profiles_put(&profiles, profile->ssid, profile->password) < 0)
        r = ESP_ERR_NO_MEM;
    if (r == ESP_OK) r = profiles_store(&profiles);
    erase(&profiles, sizeof(profiles));
    return r;
}
esp_err_t wifi_update_forget(uint8_t slot) {
    if (wifi_update_busy()) return ESP_ERR_INVALID_STATE;
    if (!slot || slot > WIFI_PROFILE_MAX) return ESP_ERR_INVALID_ARG;
    WifiProfiles profiles;
    esp_err_t r = profiles_load(&profiles);
    if (r == ESP_OK) {
        wifi_profiles_forget(&profiles, slot - 1);
        r = profiles_store(&profiles);
    }
    erase(&profiles, sizeof(profiles));
    return r;
}
void wifi_update_networks(void) {
    uint8_t payload[8 + (WIFI_PROFILE_MAX + SCAN_MAX) * 36] = {1};
    WifiProfiles profiles;
    esp_err_t r = profiles_load(&profiles);
    uint8_t count = 0;
    if (r == ESP_OK) for (unsigned i = 0; i < WIFI_PROFILE_MAX; ++i) {
        WifiProfile *p = &profiles.entries[i];
        if (!p->ssid[0]) continue;
        uint8_t *entry = payload + 8 + count++ * 36;
        memcpy(entry, p->ssid, 33);
        entry[33] = (uint8_t)-127; /* saved but not detected */
        entry[34] = p->password[0] ? 1 : 0;
        entry[35] = i + 1;
    }
    erase(&profiles, sizeof(profiles));
    portENTER_CRITICAL(&status_lock);
    payload[1] = scan_state;
    if (r == ESP_OK) r = scan_error;
    for (unsigned i = 0; i < scan_count; ++i) {
        uint8_t *entry = NULL;
        for (unsigned j = 0; j < count; ++j) {
            uint8_t *candidate = payload + 8 + j * 36;
            if (!strcmp((char *)candidate, (char *)scan_entries[i]) && candidate[34] == scan_entries[i][34]) {
                entry = candidate; break;
            }
        }
        if (entry) entry[33] = scan_entries[i][33];
        else memcpy(payload + 8 + count++ * 36, scan_entries[i], 36);
    }
    portEXIT_CRITICAL(&status_lock);
    payload[2] = count;
    if (r != ESP_OK) payload[1] = 2;
    memcpy(payload + 4, &r, 4);
    wire_send(0x8e, payload, 8 + count * 36, NULL, 0);
}
static void scanner(void *arg) {
    Job *job = arg;
    esp_err_t r = job->prepare();
    wifi_ap_record_t *records = calloc(SCAN_MAX, sizeof(*records));
    if (r == ESP_OK && !records) r = ESP_ERR_NO_MEM;
    if (r == ESP_OK) r = esp_wifi_start();
    wifi_scan_config_t config = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    if (r == ESP_OK) r = esp_wifi_scan_start(&config, true);
    uint16_t count = SCAN_MAX;
    if (r == ESP_OK) r = esp_wifi_scan_get_ap_records(&count, records);
    if (r != ESP_OK) esp_wifi_clear_ap_list();
    portENTER_CRITICAL(&status_lock);
    scan_count = 0;
    if (r == ESP_OK) for (unsigned i = 0; i < count; ++i) {
        wifi_ap_record_t *record = records + i;
        if (!record->ssid[0]) continue;
        uint8_t security = record->authmode == WIFI_AUTH_OPEN ? 0 :
            (record->authmode == WIFI_AUTH_WPA2_PSK || record->authmode == WIFI_AUTH_WPA_WPA2_PSK ||
             record->authmode == WIFI_AUTH_WPA2_WPA3_PSK) ? 1 : 2;
        bool duplicate = false;
        for (unsigned j = 0; j < scan_count; ++j)
            if (!strncmp((char *)scan_entries[j], (char *)record->ssid, 32) && scan_entries[j][34] == security)
                duplicate = true;
        if (duplicate) continue;
        uint8_t *entry = scan_entries[scan_count++];
        memset(entry, 0, 36);
        memcpy(entry, record->ssid, 32);
        entry[33] = (uint8_t)record->rssi;
        entry[34] = security;
    }
    portEXIT_CRITICAL(&status_lock);
    free(records);
    esp_wifi_stop();
    job->restore();
    erase(job, sizeof(*job)); free(job);
    portENTER_CRITICAL(&status_lock);
    scan_error = r;
    scan_state = r == ESP_OK ? 0 : 2;
    portEXIT_CRITICAL(&status_lock);
    indicator_operation(LED_IDLE);
    atomic_store(&busy, false);
    wifi_update_networks();
    vTaskDelete(NULL);
}
esp_err_t wifi_update_scan(esp_err_t (*prepare)(void), void (*restore)(void)) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&busy, &expected, true)) return ESP_ERR_INVALID_STATE;
    Job *job = calloc(1, sizeof(*job));
    if (!job) { atomic_store(&busy, false); return ESP_ERR_NO_MEM; }
    job->prepare = prepare; job->restore = restore;
    portENTER_CRITICAL(&status_lock);
    scan_state = 1; scan_error = ESP_OK;
    portEXIT_CRITICAL(&status_lock);
    indicator_operation(LED_SEARCHING);
    if (xTaskCreate(scanner, "wifi-scan", 6144, job, 4, NULL) != pdPASS) {
        free(job); atomic_store(&busy, false);
        portENTER_CRITICAL(&status_lock);
        scan_state = 2; scan_error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&status_lock);
        indicator_operation(LED_IDLE);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static void status(uint8_t state, uint16_t progress, esp_err_t error, const char *message) {
    portENTER_CRITICAL(&status_lock);
    uint8_t previous = status_bytes[1];
    status_bytes[1] = state;
    memcpy(status_bytes + 2, &progress, 2);
    memcpy(status_bytes + 4, &error, 4);
    snprintf((char *)status_bytes + 56, 64, "%s", message);
    portEXIT_CRITICAL(&status_lock);
    if (previous != state)
        wire_log("wifi update stage=%u error=%s internal=%u", state, esp_err_to_name(error),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
bool wifi_update_busy(void) { return atomic_load(&busy); }
void wifi_update_status(void) {
    uint8_t copy[120];
    portENTER_CRITICAL(&status_lock);
    memcpy(copy, status_bytes, sizeof(copy));
    portEXIT_CRITICAL(&status_lock);
    wire_send(0x8d, copy, sizeof(copy), NULL, 0);
}
static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)base; (void)id; (void)data;
    xEventGroupSetBits((EventGroupHandle_t)arg, 1);
}
typedef struct { char location[4096]; bool invalid_location; } HttpHeaders;
static esp_err_t http_event(esp_http_client_event_t *event) {
    HttpHeaders *headers = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key, "Location")) {
        size_t n = strlen(event->header_value);
        if (n >= sizeof(headers->location) || !update_download_url_allowed(event->header_value))
            headers->invalid_location = true;
        else memcpy(headers->location, event->header_value, n + 1);
    }
    return ESP_OK;
}
/* Buffer for JSON, OTA handle for the image. Hash the exact bytes while streaming. */
static esp_err_t download(const char *url, const UpdateAsset *asset, char *json, size_t capacity,
                          esp_ota_handle_t ota, size_t *received) {
    if (!update_download_url_allowed(url)) return ESP_ERR_INVALID_ARG;
    HttpHeaders *headers = calloc(1, sizeof(*headers));
    uint8_t *block = malloc(4096);
    if (!headers || !block) { free(headers); free(block); return ESP_ERR_NO_MEM; }
    esp_http_client_config_t config = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000, .buffer_size = 4096, .buffer_size_tx = 2048,
        .disable_auto_redirect = true, .user_agent = "LinkStudio-ESP32S2",
        .event_handler = http_event, .user_data = headers,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t r = client ? ESP_OK : ESP_ERR_NO_MEM;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (r == ESP_OK && asset && (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS)) r = ESP_FAIL;
    if (r == ESP_OK) r = esp_http_client_set_header(client, "Accept", asset ? "application/octet-stream" : "application/vnd.github+json");
    if (r == ESP_OK) r = esp_http_client_set_header(client, "X-GitHub-Api-Version", "2022-11-28");
    if (r == ESP_OK) r = esp_http_client_set_header(client, "Cache-Control", "no-cache");
    bool ready = false;
    for (int redirect = 0; r == ESP_OK && redirect <= 5; ++redirect) {
        memset(headers, 0, sizeof(*headers));
        r = esp_http_client_open(client, 0);
        if (r != ESP_OK) break;
        int64_t length = esp_http_client_fetch_headers(client);
        int code = esp_http_client_get_status_code(client);
        wire_log("wifi HTTPS response code=%d length=%" PRId64, code, length);
        if (length < 0 || headers->invalid_location) { r = ESP_FAIL; break; }
        if (code == 200) {
            if (length > (int64_t)capacity || (asset && length > 0 && length != (int64_t)asset->size)) r = ESP_ERR_INVALID_SIZE;
            else ready = true;
            break;
        }
        if ((code != 301 && code != 302 && code != 303 && code != 307 && code != 308) || !headers->location[0]) { r = ESP_FAIL; break; }
        esp_http_client_close(client);
        r = esp_http_client_set_url(client, headers->location);
    }
    if (!ready && r == ESP_OK) r = ESP_FAIL;
    size_t total = 0;
    const int64_t deadline = esp_timer_get_time() + 180000000;
    while (r == ESP_OK) {
        if (esp_timer_get_time() > deadline) { r = ESP_ERR_TIMEOUT; break; }
        int count = esp_http_client_read(client, (char *)block, 4096);
        if (count < 0) { r = ESP_FAIL; break; }
        if (!count) {
            if (!esp_http_client_is_complete_data_received(client)) r = ESP_FAIL;
            break;
        }
        if ((size_t)count > capacity - total) { r = ESP_ERR_INVALID_SIZE; break; }
        if (asset && psa_hash_update(&hash, block, count) != PSA_SUCCESS) { r = ESP_FAIL; break; }
        if (json) memcpy(json + total, block, count);
        else {
            r = esp_ota_write(ota, block, count);
            if (r != ESP_OK) break;
        }
        total += count;
        if (!json) status(UpdateDownloading, (uint16_t)(total * 1000 / capacity), ESP_OK, "Downloading firmware");
    }
    if (r == ESP_OK && asset) {
        uint8_t digest[32]; size_t bytes = 0;
        if (total != asset->size || psa_hash_finish(&hash, digest, sizeof(digest), &bytes) != PSA_SUCCESS ||
            bytes != 32 || memcmp(digest, asset->sha256, 32)) r = ESP_ERR_INVALID_CRC;
    }
    psa_hash_abort(&hash);
    if (r != ESP_OK && client) {
        int tls_code = 0, tls_flags = 0;
        esp_err_t tls = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
        wire_log("wifi HTTPS error=%s errno=%d tls=%s code=%d flags=%d",
                 esp_err_to_name(r), esp_http_client_get_errno(client),
                 esp_err_to_name(tls), tls_code, tls_flags);
        wire_log("wifi HTTPS memory internal=%u largest=%u external=%u received=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)total);
    }
    if (client) esp_http_client_cleanup(client);
    free(headers); free(block);
    *received = total;
    return r;
}
static void updater(void *arg) {
    Job *job = arg;
    esp_netif_t *netif = NULL;
    EventGroupHandle_t events = NULL;
    esp_event_handler_instance_t handler = NULL;
    bool clock_started = false, ota_open = false, prepared = false;
    esp_ota_handle_t ota = 0;
    char *json = NULL;
    UpdateRelease release = {0};
    char url[160]; size_t received = 0;
    const char *failure = "Wi-Fi setup failed";
    wire_log("wifi update: restore stock driver");
    esp_err_t r = job->prepare();
    prepared = true;
    if (r != ESP_OK) goto done;
    wire_log("wifi update: create station interface");
    netif = esp_netif_create_default_wifi_sta();
    events = xEventGroupCreate();
    if (!netif || !events) { r = ESP_ERR_NO_MEM; goto done; }
    r = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip, events, &handler);
    if (r != ESP_OK) goto done;
    r = esp_wifi_set_config(WIFI_IF_STA, &job->wifi);
    erase(&job->wifi, sizeof(job->wifi));
    if (r != ESP_OK) goto done;
    wire_log("wifi update: start station");
    r = esp_wifi_start();
    if (r == ESP_OK) r = esp_wifi_set_ps(WIFI_PS_NONE);
    if (r == ESP_OK) r = esp_wifi_connect();
    failure = "Wi-Fi connection failed";
    if (r != ESP_OK) goto done;
    if (!(xEventGroupWaitBits(events, 1, false, true, pdMS_TO_TICKS(30000)) & 1)) { r = ESP_ERR_TIMEOUT; goto done; }
    if (job->remember) {
        failure = "Could not save Wi-Fi network";
        r = remember_profile(&job->profile);
        erase(&job->profile, sizeof(job->profile));
        if (r != ESP_OK) goto done;
    }
    status(UpdateChecking, 0, ESP_OK, "Setting secure clock");
    esp_sntp_config_t clock_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
    r = esp_netif_sntp_init(&clock_config);
    if (r != ESP_OK) goto done;
    clock_started = true;
    r = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000));
    failure = "Clock sync failed; retry";
    if (r != ESP_OK) goto done;
    indicator_operation(LED_UPDATING);
    status(UpdateChecking, 0, ESP_OK, "Checking GitHub release");
    failure = "Release download failed";
    json = malloc(JSON_MAX + 1);
    if (!json) { r = ESP_ERR_NO_MEM; goto done; }
    r = download(RELEASE_URL, NULL, json, JSON_MAX, 0, &received);
    if (r != ESP_OK) goto done;
    if (!update_release_parse(json, received, &release)) { r = ESP_ERR_INVALID_RESPONSE; goto done; }
    portENTER_CRITICAL(&status_lock);
    memcpy(status_bytes + 8, release.tag, sizeof(release.tag));
    portEXIT_CRITICAL(&status_lock);
    if (!strcmp(release.tag, LS_BUILD_ID)) {
        status(UpdateIdle, 1000, ESP_OK, "Latest firmware installed");
        goto done;
    }
    free(json); json = malloc(release.manifest.size + 1);
    if (!json) { r = ESP_ERR_NO_MEM; goto done; }
    snprintf(url, sizeof(url), ASSET_URL "%" PRIu64, release.manifest.id);
    failure = "Invalid release manifest";
    r = download(url, &release.manifest, json, release.manifest.size, 0, &received);
    if (r != ESP_OK) goto done;
    if (!update_manifest_check(json, received, &release)) { r = ESP_ERR_INVALID_RESPONSE; goto done; }
    free(json); json = NULL;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    failure = "No compatible OTA slot";
    if (!target || target == esp_ota_get_running_partition() || release.image.size > target->size) { r = ESP_ERR_INVALID_SIZE; goto done; }
    status(UpdateDownloading, 0, ESP_OK, "Preparing inactive slot");
    r = esp_ota_begin(target, release.image.size, &ota);
    if (r != ESP_OK) goto done;
    ota_open = true;
    snprintf(url, sizeof(url), ASSET_URL "%" PRIu64, release.image.id);
    failure = "Firmware download failed";
    r = download(url, &release.image, NULL, release.image.size, ota, &received);
    if (r != ESP_OK) goto done;
    status(UpdateVerifying, 1000, ESP_OK, "Verifying firmware image");
    failure = "Firmware verification failed";
    r = esp_ota_end(ota); ota_open = false;
    if (r == ESP_OK) r = esp_ota_set_boot_partition(target);
    if (r != ESP_OK) goto done;
    indicator_operation(LED_RESTART);
    status(UpdateRestart, 1000, ESP_OK, "Verified; restarting board");
    wifi_update_status();
    erase(job, sizeof(*job)); free(job);
    vTaskDelay(pdMS_TO_TICKS(1500));
    wire_restart_application();
done:
    wire_log("wifi update finish error=%s stage=%s", esp_err_to_name(r), failure);
    if (ota_open) esp_ota_abort(ota);
    free(json);
    if (clock_started) esp_netif_sntp_deinit();
    esp_wifi_disconnect(); esp_wifi_stop();
    if (handler) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, handler);
    if (netif) esp_netif_destroy_default_wifi(netif);
    if (events) vEventGroupDelete(events);
    wifi_config_t empty = {0};
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    if (prepared) job->restore();
    erase(job, sizeof(*job)); free(job);
    indicator_operation(LED_IDLE);
    if (r != ESP_OK) { status(UpdateError, 0, r, failure); indicator_error(); }
    atomic_store(&busy, false);
    wifi_update_status();
    vTaskDelete(NULL);
}
esp_err_t wifi_update_start(const uint8_t *payload, size_t size,
                            esp_err_t (*prepare)(void), void (*restore)(void), bool remember) {
    if (!update_credentials_valid(payload, size)) return ESP_ERR_INVALID_ARG;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&busy, &expected, true)) return ESP_ERR_INVALID_STATE;
    Job *job = calloc(1, sizeof(*job));
    if (!job) { atomic_store(&busy, false); return ESP_ERR_NO_MEM; }
    job->prepare = prepare; job->restore = restore;
    job->remember = remember;
    memcpy(job->wifi.sta.ssid, payload + 2, payload[0]);
    memcpy(job->wifi.sta.password, payload + 2 + payload[0], payload[1]);
    job->wifi.sta.threshold.authmode = payload[1] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    job->wifi.sta.pmf_cfg.capable = true;
    if (remember) {
        memcpy(job->profile.ssid, payload + 2, payload[0]);
        memcpy(job->profile.password, payload + 2 + payload[0], payload[1]);
        WifiProfiles profiles;
        esp_err_t r = profiles_load(&profiles);
        if (r == ESP_OK && wifi_profiles_put(&profiles, job->profile.ssid, job->profile.password) < 0)
            r = ESP_ERR_NO_MEM;
        erase(&profiles, sizeof(profiles));
        if (r != ESP_OK) {
            erase(job, sizeof(*job)); free(job); atomic_store(&busy, false);
            status(UpdateError, 0, r, "Saved list full; forget a network");
            return r;
        }
    }
    portENTER_CRITICAL(&status_lock);
    memset(status_bytes, 0, sizeof(status_bytes)); status_bytes[0] = 1;
    portEXIT_CRITICAL(&status_lock);
    status(UpdateConnecting, 0, ESP_OK, "Connecting to home Wi-Fi");
    indicator_operation(LED_UPDATING);
    if (xTaskCreate(updater, "wifi-update", 12288, job, 4, NULL) != pdPASS) {
        erase(job, sizeof(*job)); free(job); atomic_store(&busy, false);
        status(UpdateError, 0, ESP_ERR_NO_MEM, "Not enough update memory");
        indicator_operation(LED_IDLE); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t wifi_update_saved(uint8_t slot, esp_err_t (*prepare)(void), void (*restore)(void)) {
    if (!slot || slot > WIFI_PROFILE_MAX) return ESP_ERR_INVALID_ARG;
    if (wifi_update_busy()) return ESP_ERR_INVALID_STATE;
    WifiProfiles profiles;
    esp_err_t r = profiles_load(&profiles);
    uint8_t payload[97] = {0};
    if (r == ESP_OK) {
        const WifiProfile *profile = &profiles.entries[slot - 1];
        payload[0] = strlen(profile->ssid); payload[1] = strlen(profile->password);
        memcpy(payload + 2, profile->ssid, payload[0]);
        memcpy(payload + 2 + payload[0], profile->password, payload[1]);
        r = wifi_update_start(payload, 2 + payload[0] + payload[1], prepare, restore, false);
    }
    erase(payload, sizeof(payload)); erase(&profiles, sizeof(profiles));
    return r;
}
