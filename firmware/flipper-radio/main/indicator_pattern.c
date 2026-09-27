#include "indicator.h"

static uint8_t breath(uint32_t t, uint32_t period) {
    uint32_t p = t % period;
    uint32_t ramp = p < period / 2 ? p : period - p;
    return 4 + ramp * 44 / (period / 2);
}
LedColor indicator_pattern(LedState state, uint32_t t) {
    const LedColor off = {0};
    switch (state) {
    case LED_BOOT: return (LedColor){12,12,12};
    case LED_IDLE: return (LedColor){0,0,breath(t,3000)};
    case LED_SEARCHING: return t % 1000 < 180 ? (LedColor){0,0,48} : off;
    case LED_CONNECTED: return (LedColor){0,20,28};
    case LED_OFFERED: return t % 1600 < 100 || (t % 1600 >= 220 && t % 1600 < 320)
        ? (LedColor){36,0,48} : off;
    case LED_SAVING: return t % 300 < 150 ? (LedColor){48,30,0} : off;
    case LED_COMPLETE: return t % 1800 < 600 && t % 200 < 100 ? (LedColor){0,48,0} : off;
    case LED_BACKUP: return t % 600 < 300 ? (LedColor){20,20,20} : off;
    case LED_UPDATING: { uint8_t b=breath(t,700); return (LedColor){b,0,b}; }
    case LED_ERROR: return t % 1400 < 600 && t % 200 < 100 ? (LedColor){64,0,0} : off;
    case LED_RESTART: return (LedColor){32,32,32};
    default: return off;
    }
}
