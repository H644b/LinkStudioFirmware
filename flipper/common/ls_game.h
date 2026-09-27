#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t* data;
    size_t size, pos;
} LsRead;
bool ls_game_uint(LsRead* r, uint64_t* value);
bool ls_game_group(LsRead* r, uint8_t tag, uint32_t count);
bool ls_game_blob(LsRead* r, const uint8_t** data, size_t* size);
bool ls_game_identity(const uint8_t* data, size_t size, uint32_t* sync_hash);
bool ls_game_identity_tail(const uint8_t* data, size_t size, uint32_t sync_hash);
bool ls_game_ready(const uint8_t* data, size_t size);
/* Build typed game messages from fields. A too-small output is never written. */
size_t ls_game_offer(uint16_t revision, const uint8_t entity[344], bool preview, uint8_t* out,
                     size_t capacity);
size_t ls_game_revision(uint8_t command, uint16_t revision, uint8_t* out, size_t capacity);
size_t ls_game_cancel(uint16_t revision, int32_t reason, uint8_t* out, size_t capacity);
