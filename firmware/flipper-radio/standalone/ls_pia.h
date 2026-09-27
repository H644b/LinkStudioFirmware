#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_PIA_DATAGRAM_MAX 8192
#define LS_PIA_PLAIN_MAX 16384
#define LS_PIA_MESSAGES_MAX 128
typedef struct LsPia LsPia;
typedef struct {
    uint8_t protocol;
    int16_t flags; /* -1 omits the optional field when sending; received flags are 0..255. */
    size_t size;
    const uint8_t* data;
} LsPiaMessage;
typedef struct {
    uint16_t destination, source, packet_id, recipient;
    uint8_t flags, footer_size;
    size_t count;
    LsPiaMessage messages[LS_PIA_MESSAGES_MAX];
} LsPiaFrame;

LsPia* ls_pia_alloc(const uint8_t ssid[16], uint64_t first_nonce);
void ls_pia_free(LsPia* pia);
uint32_t ls_pia_network_id(const LsPia* pia);
/* IPv4 arguments are numeric network addresses: 169.254.1.1 == 0xa9fe0101.
 * Decode authenticates and validates the whole datagram before returning any
 * messages. The returned pointers remain valid until the next decode. Sending
 * a response while iterating a received frame does not overwrite its messages. */
const LsPiaFrame* ls_pia_decode(LsPia* pia, const uint8_t* data, size_t size, uint32_t source_ip);
size_t ls_pia_encode(LsPia* pia, const LsPiaMessage* messages, size_t count, uint32_t source_ip,
                     uint16_t destination, uint16_t source, uint16_t packet_id, bool establishing,
                     bool footer, uint16_t recipient, uint8_t* out, size_t capacity);
