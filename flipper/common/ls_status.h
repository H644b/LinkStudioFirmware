#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t state, flags;
    bool answers_command;
    uint32_t trades;
    int32_t error;
    char peer[32];
} LsStatus;

/* A valid notification answers only STATUS or DESCRIBE, never a mutation. */
bool ls_status_parse(const uint8_t* data, size_t size, uint8_t awaiting, LsStatus* out);
