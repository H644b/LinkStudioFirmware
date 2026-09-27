#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
/* Start/release callbacks switch the existing driver between LDN and stock WPA. */
esp_err_t wifi_update_start(const uint8_t *payload, size_t size,
                            esp_err_t (*prepare)(void), void (*restore)(void), bool remember);
esp_err_t wifi_update_saved(uint8_t slot, esp_err_t (*prepare)(void), void (*restore)(void));
esp_err_t wifi_update_forget(uint8_t slot);
esp_err_t wifi_update_scan(esp_err_t (*prepare)(void), void (*restore)(void));
void wifi_update_networks(void);
bool wifi_update_busy(void);
void wifi_update_status(void);
