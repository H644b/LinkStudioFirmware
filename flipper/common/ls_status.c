#include "ls_status.h"
#include "ls_control.h"
#include <string.h>

bool ls_status_parse(const uint8_t* p, size_t n, uint8_t awaiting, LsStatus* out) {
    if (!p || !out || n != LS_STATUS_SIZE || p[0] != 1 || p[1] > LsStateError ||
        (p[2] & 0xf0) || p[3] || !memchr(p + 12, 0, 32))
        return false;
    LsStatus parsed = {.state = p[1], .flags = p[2],
                       .answers_command = awaiting == LsCmdStatus || awaiting == LsCmdDescribe,
                       .trades = ls_read32(p + 4), .error = (int32_t)ls_read32(p + 8)};
    memcpy(parsed.peer, p + 12, sizeof(parsed.peer));
    *out = parsed;
    return true;
}
