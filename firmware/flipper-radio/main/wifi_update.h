#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
/* Start/release callbacks switch the existing driver between LDN and stock WPA. */
esp_err_t wifi_update_start(const uint8_t *payload, size_t size,
                            esp_err_t (*prepare)(void), void (*restore)(void));
bool wifi_update_busy(void);
void wifi_update_status(void);
