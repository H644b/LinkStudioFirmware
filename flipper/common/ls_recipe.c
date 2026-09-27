#include "ls_recipe.h"
#include "ls_game.h"
#include "ls_pa9.h"
#include "ls_wire.h"
#include <string.h>
static uint16_t u16(const uint8_t* p) {
    return p[0] | (uint16_t)p[1] << 8;
}
static uint32_t u32(const uint8_t* p) {
    return u16(p) | (uint32_t)u16(p + 2) << 16;
}
bool ls_recipe_parse(const uint8_t* data, size_t size, LsRecipe* recipe) {
    if (!data || !recipe || size < 434 || size > LS_RECIPE_MAX)
        return false;
    if (memcmp(data, "LSFZ0001", 8) || u16(data + 8) != 1 || u16(data + 10) != 0)
        return false;
    if (u32(data + 12) != size || u32(data + 16) != ls_crc32(data + 20, size - 20))
        return false;
    if (!memchr(data + 20, 0, 32) || !memchr(data + 52, 0, 32))
        return false;
    uint16_t identity = u16(data + 84), tail = u16(data + 86), ready = u16(data + 88);
    if (identity < 4 || identity > 256 || tail < 4 || tail > 32 || ready < 4 || ready > 1300)
        return false;
    if ((size_t)434 + identity + tail + ready != size)
        return false;
    if (data[434] != 20 || data[435] != 0 || data[434 + identity] != 20 ||
        data[435 + identity] != 3)
        return false;
    if (data[434 + identity + tail] != 1 || data[435 + identity + tail] != 0)
        return false;
    uint8_t plain[LS_PA9_SIZE];
    if (!ls_pa9_decode(data + 90, plain))
        return false;
    uint32_t hash;
    if (!ls_game_identity(data + 434, identity, &hash) ||
        !ls_game_identity_tail(data + 434 + identity, tail, hash) ||
        !ls_game_ready(data + 434 + identity + tail, ready))
        return false;
    *recipe = (LsRecipe){.display_name = (const char*)data + 20,
                         .trainer_name = (const char*)data + 52,
                         .entity = data + 90,
                         .identity = data + 434,
                         .identity_tail = data + 434 + identity,
                         .ready = data + 434 + identity + tail,
                         .identity_size = identity,
                         .tail_size = tail,
                         .ready_size = ready};
    return true;
}
