#include "ls_wire.h"
#include <string.h>

uint32_t ls_crc32(const uint8_t* data, size_t size) {
    uint32_t value = UINT32_MAX;
    while (size--) {
        value ^= *data++;
        for (unsigned i = 0; i < 8; ++i)
            value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1)));
    }
    return ~value;
}

size_t ls_wire_encode(uint8_t type, const uint8_t* payload, size_t size, uint8_t* out,
                      size_t capacity) {
    if (!out || size > LS_WIRE_PAYLOAD || (!payload && size) || capacity < size + 12 + size / 254)
        return 0;
    uint8_t frame[LS_WIRE_PAYLOAD + 5];
    frame[0] = type;
    if (size)
        memcpy(frame + 1, payload, size);
    uint32_t crc = ls_crc32(frame, size + 1);
    for (unsigned i = 0; i < 4; ++i)
        frame[size + 1 + i] = crc >> (8 * i);
    size_t at = 1, code_at = 0;
    uint8_t code = 1;
    for (size_t i = 0; i < size + 5; ++i) {
        if (!frame[i]) {
            out[code_at] = code;
            code_at = at++;
            code = 1;
        } else {
            out[at++] = frame[i];
            if (++code == 255) {
                out[code_at] = code;
                code_at = at++;
                code = 1;
            }
        }
    }
    out[code_at] = code;
    out[at++] = 0;
    return at;
}

static void deliver(LsWire* wire, LsWireReceive receive, void* context) {
    size_t in = 0, out = 0;
    while (in < wire->used) {
        unsigned code = wire->encoded[in++];
        if (!code || in + code - 1 > wire->used || out + code - 1 > sizeof(wire->decoded))
            goto reject;
        for (unsigned i = 1; i < code; ++i)
            wire->decoded[out++] = wire->encoded[in++];
        if (code != 255 && in < wire->used) {
            if (out == sizeof(wire->decoded))
                goto reject;
            wire->decoded[out++] = 0;
        }
    }
    if (out < 5)
        goto reject;
    uint32_t expected = 0;
    for (unsigned i = 0; i < 4; ++i)
        expected |= (uint32_t)wire->decoded[out - 4 + i] << (8 * i);
    if (ls_crc32(wire->decoded, out - 4) != expected)
        goto reject;
    receive(context, wire->decoded[0], wire->decoded + 1, out - 5);
    return;
reject:
    ++wire->rejected;
}

void ls_wire_feed(LsWire* wire, const uint8_t* data, size_t size, LsWireReceive receive,
                  void* context) {
    for (size_t i = 0; i < size; ++i) {
        if (data[i]) {
            if (wire->used < sizeof(wire->encoded))
                wire->encoded[wire->used++] = data[i];
            else
                wire->overflow = true;
        } else {
            if (wire->overflow)
                ++wire->rejected;
            else if (wire->used)
                deliver(wire, receive, context);
            wire->used = 0;
            wire->overflow = false;
        }
    }
}
