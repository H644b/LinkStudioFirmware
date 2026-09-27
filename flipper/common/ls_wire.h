#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_WIRE_PAYLOAD 1600
#define LS_WIRE_ENCODED 1632
typedef void (*LsWireReceive)(void* context, uint8_t type, const uint8_t* payload, size_t size);
typedef struct {
    uint8_t encoded[LS_WIRE_ENCODED];
    uint8_t decoded[LS_WIRE_PAYLOAD + 5];
    size_t used;
    bool overflow;
    uint32_t rejected;
} LsWire;
uint32_t ls_crc32(const uint8_t* data, size_t size);
size_t ls_wire_encode(uint8_t type, const uint8_t* payload, size_t size, uint8_t* out,
                      size_t capacity);
void ls_wire_feed(LsWire* wire, const uint8_t* data, size_t size, LsWireReceive receive,
                  void* context);
