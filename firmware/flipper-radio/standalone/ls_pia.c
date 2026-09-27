#include "ls_pia.h"
#include "ls_crypto.h"
#include "ls_wire.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "ls_alloc.h"
#include <string.h>
#include <zstd.h>

struct LsPia {
    uint8_t key[16];
    uint32_t network_id;
    uint64_t nonce;
    bool exhausted;
    ZSTD_DCtx* zstd;
    uint8_t raw[LS_PIA_DATAGRAM_MAX], plain[LS_PIA_PLAIN_MAX];
    LsPiaFrame frame;
};
static uint16_t read16(const uint8_t* p) { return (uint16_t)p[0] << 8 | p[1]; }
static void be16(uint8_t* p, uint16_t n) {
    p[0] = n >> 8;
    p[1] = n;
}
static void be32(uint8_t* p, uint32_t n) {
    be16(p, n >> 16);
    be16(p + 2, n);
}
static void be64(uint8_t* p, uint64_t n) {
    be32(p, n >> 32);
    be32(p + 4, n);
}
LsPia* ls_pia_alloc(const uint8_t ssid[16], uint64_t first_nonce) {
    if (!ssid)
        return NULL;
    LsPia* pia = ls_calloc(1, sizeof(*pia));
    if (!pia)
        return NULL;
    static const uint8_t game_key[16] = {'p', '3', 'b', 'w', 'd', 'a', 'S', 's',
                                         'y', 'w', 'F', 'X', 'U', 'k', 'D', 'u'};
    if (!ls_crypto_ecb(game_key, ssid, pia->key)) {
        free(pia);
        return NULL;
    }
    pia->network_id = ls_crc32(ssid + 1, 15);
    pia->nonce = first_nonce;
    pia->zstd = ZSTD_createDCtx();
    if (!pia->zstd) {
        free(pia);
        return NULL;
    }
    return pia;
}
void ls_pia_free(LsPia* pia) {
    if (pia) {
        ZSTD_freeDCtx(pia->zstd);
        memset(pia, 0, sizeof(*pia));
        free(pia);
    }
}
uint32_t ls_pia_network_id(const LsPia* pia) { return pia->network_id; }
static bool parse_messages(LsPia* pia, const uint8_t* p, size_t size) {
    size_t at = 0, message_size = 0, count = 0;
    uint8_t flags = 0, protocol = 0;
    bool has_size = false, has_protocol = false;
    while (at < size) {
        uint8_t present = p[at++];
        if (present & 0xf0 || count == LS_PIA_MESSAGES_MAX)
            return false;
        if (present & 1) {
            if (at == size)
                return false;
            flags = p[at++];
        }
        if (present & 2) {
            if (size - at < 2)
                return false;
            message_size = read16(p + at);
            at += 2;
            has_size = true;
        }
        if (present & 4) {
            if (at == size)
                return false;
            protocol = p[at++];
            has_protocol = true;
        }
        if (present & 8) {
            if (at == size)
                return false;
            ++at;
        } /* one-byte port at this Pia version */
        if (!has_size || !has_protocol || message_size > size - at)
            return false;
        pia->frame.messages[count++] = (LsPiaMessage){
            .protocol = protocol, .flags = flags, .size = message_size, .data = p + at};
        at += message_size;
    }
    pia->frame.count = count;
    return count != 0;
}
const LsPiaFrame* ls_pia_decode(LsPia* pia, const uint8_t* data, size_t size, uint32_t source_ip) {
    if (!pia || !data || size < 29 || size > LS_PIA_DATAGRAM_MAX ||
        memcmp(data, "\x32\xab\x98\x64", 4) || data[4] != 0x90)
        return NULL;
    pia->frame.count = 0;
    uint8_t nonce[12];
    be32(nonce, pia->network_id ^ source_ip);
    memcpy(nonce + 4, data + 13, 8);
    size_t raw_size = size - 29;
    if (!ls_crypto_gcm_decrypt(pia->key, nonce, data + 29, raw_size, data + 21, pia->raw))
        return NULL;
    size_t padding = data[5] >> 4;
    if (padding > raw_size)
        return NULL;
    for (size_t i = raw_size - padding; i < raw_size; ++i)
        if (pia->raw[i] != 255)
            return NULL;
    raw_size -= padding;
    uint8_t footer = data[12];
    if (footer > raw_size)
        return NULL;
    uint16_t recipient = footer == 2 ? read16(pia->raw + raw_size - 2) : 0;
    raw_size -= footer;
    const uint8_t* messages = pia->raw;
    size_t message_size = raw_size;
    if (data[5] & 1) {
        ZSTD_frameHeader header;
        size_t result = ZSTD_getFrameHeader(&header, pia->raw, raw_size);
        if (result || header.frameType != ZSTD_frame || header.dictID ||
            header.windowSize > 131072 ||
            (header.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN &&
             header.frameContentSize > LS_PIA_PLAIN_MAX))
            return NULL;
        if (ZSTD_findFrameCompressedSize(pia->raw, raw_size) != raw_size)
            return NULL;
        message_size =
            ZSTD_decompressDCtx(pia->zstd, pia->plain, sizeof(pia->plain), pia->raw, raw_size);
        if (ZSTD_isError(message_size))
            return NULL;
        messages = pia->plain;
    }
    if (!parse_messages(pia, messages, message_size)) {
        pia->frame.count = 0;
        return NULL;
    }
    pia->frame.destination = read16(data + 6);
    pia->frame.source = read16(data + 8);
    pia->frame.packet_id = read16(data + 10);
    pia->frame.flags = data[5];
    pia->frame.footer_size = footer;
    pia->frame.recipient = recipient;
    return &pia->frame;
}
size_t ls_pia_encode(LsPia* pia, const LsPiaMessage* messages, size_t count, uint32_t source_ip,
                     uint16_t destination, uint16_t source, uint16_t packet_id, bool establishing,
                     bool footer, uint16_t recipient, uint8_t* out, size_t capacity) {
    if (!pia || !messages || !count || count > LS_PIA_MESSAGES_MAX || !out || capacity < 29 ||
        pia->exhausted)
        return 0;
    uint8_t* body = out + 29;
    size_t body_capacity =
        capacity > LS_PIA_DATAGRAM_MAX ? LS_PIA_DATAGRAM_MAX - 29 : capacity - 29;
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        const LsPiaMessage* m = messages + i;
        size_t header = m->flags >= 0 ? 5 : 4;
        if ((m->size && !m->data) || m->size > UINT16_MAX || m->flags > 255 ||
            at + header > body_capacity || m->size > body_capacity - at - header)
            return 0;
        body[at++] = m->flags >= 0 ? 7 : 6;
        if (m->flags >= 0)
            body[at++] = (uint8_t)m->flags;
        be16(body + at, m->size);
        at += 2;
        body[at++] = m->protocol;
        if (m->size)
            memcpy(body + at, m->data, m->size);
        at += m->size;
    }
    if (footer) {
        if (body_capacity - at < 2)
            return 0;
        be16(body + at, recipient);
        at += 2;
    }
    size_t pad = (16 - at % 16) % 16;
    if (at + pad > body_capacity)
        return 0;
    memset(body + at, 255, pad);
    at += pad;
    memcpy(out, "\x32\xab\x98\x64", 4);
    out[4] = 0x90;
    out[5] = (uint8_t)(pad << 4) | (establishing ? 2 : 0);
    be16(out + 6, destination);
    be16(out + 8, source);
    be16(out + 10, packet_id);
    out[12] = footer ? 2 : 0;
    be64(out + 13, pia->nonce);
    if (pia->nonce == UINT64_MAX)
        pia->exhausted = true;
    else
        ++pia->nonce;
    uint8_t nonce[12];
    be32(nonce, pia->network_id ^ source_ip);
    memcpy(nonce + 4, out + 13, 8);
    if (!ls_crypto_gcm_encrypt(pia->key, nonce, body, at, body, out + 21))
        return 0;
    return 29 + at;
}
