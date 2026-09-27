#pragma once
#include <stddef.h>
#include <stdint.h>

/* Experimental/non-commercial SysEx manufacturer 0x7d, product LS, protocol 1.
 * Payload is a bounded chunk of the existing COBS/CRC byte stream. Chunks may split frames. */
#define LS_MIDI_RAW_MAX 1632
#define LS_MIDI_ENCODED_MAX (6 + LS_MIDI_RAW_MAX + (LS_MIDI_RAW_MAX + 6) / 7)
size_t ls_midi_encode(const uint8_t *raw, size_t length, uint8_t *out, size_t capacity);
/* SIZE_MAX means malformed or oversized; zero is a valid empty envelope. */
size_t ls_midi_decode(const uint8_t *data, size_t length, uint8_t *out, size_t capacity);
