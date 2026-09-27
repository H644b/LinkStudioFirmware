#include "ls_pa9.h"
#include <string.h>

/* PokeCrypto Gen-8/9 read order; indices 24..31 repeat 0..7. */
static const uint8_t order[32][4] = {
    {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 3, 1, 2}, {0, 2, 3, 1}, {0, 3, 2, 1},
    {1, 0, 2, 3}, {1, 0, 3, 2}, {2, 0, 1, 3}, {3, 0, 1, 2}, {2, 0, 3, 1}, {3, 0, 2, 1},
    {1, 2, 0, 3}, {1, 3, 0, 2}, {2, 1, 0, 3}, {3, 1, 0, 2}, {2, 3, 0, 1}, {3, 2, 0, 1},
    {1, 2, 3, 0}, {1, 3, 2, 0}, {2, 1, 3, 0}, {3, 1, 2, 0}, {2, 3, 1, 0}, {3, 2, 1, 0},
    {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 3, 1, 2}, {0, 2, 3, 1}, {0, 3, 2, 1},
    {1, 0, 2, 3}, {1, 0, 3, 2},
};
static uint16_t u16(const uint8_t* p) {
    return p[0] | (uint16_t)p[1] << 8;
}
static void crypt(uint8_t* p, unsigned n, uint32_t seed) {
    for (unsigned i = 0; i < n; i += 2) {
        seed = seed * UINT32_C(0x41c64e6d) + 0x6073;
        p[i] ^= seed >> 16;
        p[i + 1] ^= seed >> 24;
    }
}
bool ls_pa9_decode(const uint8_t encrypted[LS_PA9_SIZE], uint8_t plain[LS_PA9_SIZE]) {
    if (!encrypted || !plain || u16(encrypted + 4))
        return false;
    uint8_t shuffled[LS_PA9_SIZE];
    memcpy(shuffled, encrypted, sizeof(shuffled));
    uint32_t ec = u16(shuffled) | (uint32_t)u16(shuffled + 2) << 16;
    crypt(shuffled + 8, 320, ec);
    crypt(shuffled + 328, 16, ec);
    uint16_t sum = 0;
    for (unsigned i = 8; i < 328; i += 2)
        sum += u16(shuffled + i);
    if (sum != u16(shuffled + 6))
        return false;
    memcpy(plain, shuffled, 8);
    for (unsigned i = 0; i < 4; ++i)
        memcpy(plain + 8 + 80 * i, shuffled + 8 + 80 * order[(ec >> 13) & 31][i], 80);
    memcpy(plain + 328, shuffled + 328, 16);
    return u16(plain + 8) != 0;
}
