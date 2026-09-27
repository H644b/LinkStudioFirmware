#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    esp_err_t (*start)(const uint8_t* data, size_t size);
    void (*stop)(void);
    bool (*ethernet)(const uint8_t* data, size_t size);
    bool (*raw)(const uint8_t* data, size_t size);
} LsRadio;
bool ls_runtime_init(const LsRadio* radio);
bool ls_runtime_active(void);
bool ls_runtime_ready(void);
int ls_runtime_stats(char* text, size_t size);
/* Commands are queued; the worker emits RESULT/STATUS. Only GPIO may call. */
bool ls_runtime_command(uint8_t command, const uint8_t* data, size_t size, bool idle);
/* Wi-Fi callbacks only copy into a bounded queue, never execute the engine. */
void ls_runtime_ethernet(const uint8_t* frame, size_t size);
void ls_runtime_monitor(const uint8_t* frame, size_t size);
void ls_runtime_peer_left(const uint8_t mac[6]);
