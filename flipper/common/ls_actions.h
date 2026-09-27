#pragma once
#include <stdbool.h>
#include <stdint.h>
enum {
    LsCanLoad = 1,
    LsCanSettings = 2,
    LsCanStart = 4,
    LsCanOffer = 8,
    LsCanCancel = 16,
    LsCanStop = 32,
    LsCanWifi = 64,
    LsCanExit = 128
};
unsigned ls_actions(bool connected, bool compatible, bool known, bool recipe, bool busy,
                    bool updating, uint8_t state, uint8_t flags);
