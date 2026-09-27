#include "midi_codec.h"
#include <string.h>

static const uint8_t prefix[] = {0xf0, 0x7d, 0x4c, 0x53, 1};

size_t ls_midi_encode(const uint8_t *raw, size_t length, uint8_t *out, size_t capacity)
{
    const size_t needed = sizeof(prefix) + length + (length + 6) / 7 + 1;
    if (length > LS_MIDI_RAW_MAX || capacity < needed) return 0;
    memcpy(out, prefix, sizeof(prefix));
    size_t at = sizeof(prefix);
    for (size_t i = 0; i < length;) {
        const size_t mask_at = at++;
        uint8_t mask = 0;
        for (unsigned bit = 0; bit < 7 && i < length; bit++, i++) {
            mask |= (raw[i] >> 7) << bit;
            out[at++] = raw[i] & 0x7f;
        }
        out[mask_at] = mask;
    }
    out[at++] = 0xf7;
    return at;
}

size_t ls_midi_decode(const uint8_t *data, size_t length, uint8_t *out, size_t capacity)
{
    if (length < 6 || length > LS_MIDI_ENCODED_MAX ||
        memcmp(data, prefix, sizeof(prefix)) || data[length - 1] != 0xf7) return SIZE_MAX;
    size_t read = sizeof(prefix), used = 0;
    while (read < length - 1) {
        const uint8_t mask = data[read++];
        if (mask & 0x80 || read == length - 1) return SIZE_MAX;
        unsigned bit = 0;
        for (; bit < 7 && read < length - 1; bit++, read++) {
            if (data[read] & 0x80 || used >= capacity || used >= LS_MIDI_RAW_MAX) return SIZE_MAX;
            out[used++] = data[read] | (((mask >> bit) & 1) << 7);
        }
        if (mask >> bit) return SIZE_MAX;
    }
    return used;
}
