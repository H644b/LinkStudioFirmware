#pragma once
#include <stdint.h>

enum {
    LsCmdHello = 1,
    LsCmdRelease = 15,
    LsCmdDescribe = 0x30,
    LsCmdRecipeBegin,
    LsCmdRecipeChunk,
    LsCmdRecipeCommit,
    LsCmdStart,
    LsCmdPreview,
    LsCmdOffer,
    LsCmdCancel,
    LsCmdStopSafe,
    LsCmdStatus,
    LsCmdPing,
    LsMsgInfo = 0x81,
    LsMsgResult = 0x82,
    LsMsgStatus = 0xb0,
};
enum {
    LsStateIdle,
    LsStateHosting,
    LsStateConnected,
    LsStateOffered,
    LsStateSaving,
    LsStateComplete,
    LsStateStopping,
    LsStateError,
};
enum { LsFlagRecipe = 1, LsFlagPeer = 2, LsFlagSafeToStop = 4, LsFlagStopRequested = 8 };
/* Status v1: version, state, flags, reserved, completed trades LE32,
 * last error signed LE32, peer UTF-8 C-string[32]. No native struct on wire. */
#define LS_STATUS_SIZE 44
static inline uint16_t ls_read16(const uint8_t* p) {
    return p[0] | (uint16_t)p[1] << 8;
}
static inline uint32_t ls_read32(const uint8_t* p) {
    return ls_read16(p) | (uint32_t)ls_read16(p + 2) << 16;
}
static inline void ls_write16(uint8_t* p, uint16_t n) {
    p[0] = n;
    p[1] = n >> 8;
}
static inline void ls_write32(uint8_t* p, uint32_t n) {
    ls_write16(p, n);
    ls_write16(p + 2, n >> 16);
}
