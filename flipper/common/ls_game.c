#include "ls_game.h"
#include "ls_wire.h"
#include <string.h>

bool ls_game_uint(LsRead* r, uint64_t* value) {
    if (!r || !value || r->pos >= r->size)
        return false;
    uint8_t tag = r->data[r->pos++];
    if (tag < 128) {
        *value = tag;
        return true;
    }
    if (tag < 0x80 || tag > 0x83)
        return false;
    size_t n = (size_t)1 << (tag - 0x80);
    if (r->size - r->pos < n)
        return false;
    uint64_t result = 0;
    for (size_t i = 0; i < n; ++i)
        result |= (uint64_t)r->data[r->pos++] << (i * 8);
    *value = result;
    return true;
}
bool ls_game_group(LsRead* r, uint8_t tag, uint32_t count) {
    uint64_t actual;
    return r->pos < r->size && r->data[r->pos++] == tag && ls_game_uint(r, &actual) &&
           actual == count;
}
bool ls_game_blob(LsRead* r, const uint8_t** data, size_t* size) {
    uint64_t length;
    if (r->pos >= r->size || r->data[r->pos++] != 0xbc || !ls_game_uint(r, &length) ||
        length > r->size - r->pos)
        return false;
    *data = r->data + r->pos;
    *size = (size_t)length;
    r->pos += (size_t)length;
    return true;
}
static bool word(LsRead* r, uint32_t max, uint64_t* value) {
    return ls_game_uint(r, value) && *value <= max;
}
bool ls_game_identity(const uint8_t* data, size_t size, uint32_t* sync_hash) {
    if (!data || !sync_hash || size < 4 || data[0] != 20 || data[1])
        return false;
    LsRead r = {data, size, 2};
    uint64_t n;
    const uint8_t *trainer, *name;
    size_t trainer_size, length;
    if (!ls_game_group(&r, 0xb9, 2) || !word(&r, UINT32_MAX, &n) || n != 0x2abe85e2 ||
        !ls_game_group(&r, 0xb9, 1) || !ls_game_blob(&r, &trainer, &trainer_size) ||
        r.pos != r.size)
        return false;
    LsRead t = {trainer, trainer_size, 0};
    if (!ls_game_group(&t, 0xb9, 6) || !word(&t, UINT32_MAX, &n) || !word(&t, 1, &n) ||
        !word(&t, 11, &n) || n == 0 || n == 6 || !ls_game_blob(&t, &name, &length) || length != 26)
        return false;
    bool terminated = false;
    for (size_t i = 0; i < 26; i += 2)
        if (!name[i] && !name[i + 1])
            terminated = true;
    if (!terminated || !ls_game_blob(&t, &name, &length) || length != 32 ||
        !memchr(name, 0, length) || !ls_game_group(&t, 0xba, 3))
        return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!ls_game_group(&t, 0xb9, 2) || !word(&t, UINT32_MAX, &n) || !word(&t, UINT32_MAX, &n))
            return false;
    if (t.pos != t.size)
        return false;
    uint32_t hash = 0x811c9dc5;
    for (size_t i = 0; i < trainer_size; ++i)
        hash = (hash ^ trainer[i]) * UINT32_C(0x01000193);
    uint8_t little[4];
    for (unsigned i = 0; i < 4; ++i)
        little[i] = hash >> (8 * i);
    *sync_hash = ls_crc32(little, 4);
    return true;
}
bool ls_game_identity_tail(const uint8_t* data, size_t size, uint32_t sync_hash) {
    if (!data || size < 4 || data[0] != 20 || data[1] != 3)
        return false;
    LsRead r = {data, size, 2};
    uint64_t value;
    return ls_game_group(&r, 0xb9, 1) && ls_game_uint(&r, &value) && value == sync_hash &&
           r.pos == r.size;
}
bool ls_game_ready(const uint8_t* data, size_t size) {
    if (!data || size < 4 || data[0] != 1 || data[1])
        return false;
    LsRead r = {data, size, 2};
    uint64_t revision;
    const uint8_t* table;
    size_t count;
    return ls_game_group(&r, 0xb9, 2) && ls_game_uint(&r, &revision) && !revision &&
           ls_game_group(&r, 0xb9, 1) && ls_game_blob(&r, &table, &count) && count == 1200 &&
           r.pos == r.size;
}
static size_t write_uint(uint32_t value, uint8_t* out) {
    if (value < 128) {
        out[0] = value;
        return 1;
    }
    size_t n = value <= 255 ? 1 : value <= 65535 ? 2 : 4;
    out[0] = n == 1 ? 0x80 : n == 2 ? 0x81 : 0x82;
    for (size_t i = 0; i < n; ++i)
        out[i + 1] = value >> (i * 8);
    return n + 1;
}
size_t ls_game_offer(uint16_t revision, const uint8_t entity[344], bool preview, uint8_t* out,
                     size_t capacity) {
    if (!out || !entity || capacity < 356)
        return 0;
    memcpy(out, "\x01\x01\xb9\x03", 4);
    size_t at = 4 + write_uint(revision, out + 4);
    memcpy(out + at, "\xbc\x81\x58\x01", 4);
    at += 4;
    memcpy(out + at, entity, 344);
    at += 344;
    out[at++] = preview;
    return at;
}
size_t ls_game_revision(uint8_t command, uint16_t revision, uint8_t* out, size_t capacity) {
    if (!out || capacity < 7 || (command != 2 && command != 4))
        return 0;
    out[0] = 1;
    out[1] = command;
    out[2] = 0xb9;
    out[3] = 1;
    return 4 + write_uint(revision, out + 4);
}
size_t ls_game_cancel(uint16_t revision, int32_t reason, uint8_t* out, size_t capacity) {
    if (!out || capacity < 12)
        return 0;
    memcpy(out, "\x01\x03\xb9\x02", 4);
    size_t at = 4 + write_uint(revision, out + 4);
    if (reason >= -64 && reason <= 127) {
        out[at++] = (uint8_t)reason;
        return at;
    }
    size_t n = reason >= -128 && reason <= 127 ? 1 : reason >= -32768 && reason <= 32767 ? 2 : 4;
    out[at++] = n == 1 ? 0x84 : n == 2 ? 0x85 : 0x86;
    for (size_t i = 0; i < n; ++i)
        out[at++] = (uint32_t)reason >> (i * 8);
    return at;
}
