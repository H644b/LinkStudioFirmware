#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LED_BOOT = 0, LED_IDLE, LED_SEARCHING, LED_CONNECTED, LED_OFFERED,
    LED_SAVING, LED_COMPLETE, LED_BACKUP, LED_UPDATING, LED_ERROR, LED_RESTART,
    LED_STATE_COUNT,
} LedState;
typedef struct { uint8_t red, green, blue; } LedColor;
LedColor indicator_pattern(LedState state, uint32_t elapsed_ms);
bool indicator_init(void);
void indicator_base(LedState state);
/* Application hints expire after 3 seconds; they never alter radio/trade state. */
void indicator_host(LedState state);
/* Maintenance overrides host hints until cleared with LED_IDLE. */
void indicator_operation(LedState state);
void indicator_error(void);
