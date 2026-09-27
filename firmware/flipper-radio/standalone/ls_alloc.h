#pragma once
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

/* Retained protocol state must not consume Wi-Fi/USB's internal heap. The
 * standalone engine requires PSRAM; allocation failure is reported to its
 * caller. Ordinary USB/MIDI firmware remains usable when PSRAM is absent. */
static inline void* ls_alloc(size_t size) {
#ifdef ESP_PLATFORM
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}
static inline void* ls_calloc(size_t count, size_t size) {
#ifdef ESP_PLATFORM
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(count, size);
#endif
}
