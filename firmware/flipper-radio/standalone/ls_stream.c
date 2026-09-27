#include "ls_stream.h"
#include "ls_alloc.h"
#include "ls_reliable.h"
#include <string.h>

#define BASE 0xfff0U
#define RX_SLOTS (LS_RELIABLE_WINDOW + 1)
typedef struct {
    uint8_t* data;
    uint16_t size;
    uint8_t flags;
} Slot;
struct LsStream {
    LsReliable* link;
    uint8_t protocol, local, remote;
    bool opened, received_opening, assembling, failed;
    uint16_t next, head;
    size_t queued_bytes, assembled;
    Slot slots[RX_SLOTS];
    uint8_t message[LS_STREAM_MESSAGE_MAX];
};
static uint16_t word(const uint8_t* p) { return (uint16_t)p[0] << 8 | p[1]; }
static void put(uint8_t* p, uint16_t n) {
    p[0] = n >> 8;
    p[1] = n;
}
LsStream* ls_stream_alloc(uint8_t protocol, bool host) {
    if (protocol != 10 && protocol != 11)
        return NULL;
    LsStream* s = ls_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->link = ls_reliable_alloc(BASE);
    if (!s->link) {
        free(s);
        return NULL;
    }
    s->protocol = protocol;
    s->local = host ? 2 : 1;
    s->remote = host ? 1 : 2;
    s->next = BASE;
    return s;
}
void ls_stream_free(LsStream* s) {
    if (!s)
        return;
    ls_reliable_free(s->link);
    for (unsigned i = 0; i < RX_SLOTS; ++i)
        free(s->slots[i].data);
    free(s);
}
static size_t encode(const LsStream* s, uint16_t seq, uint8_t flags, const uint8_t* payload,
                     size_t size, uint8_t* out, size_t capacity) {
    if (!out || size + 8 > capacity || size > LS_RELIABLE_PAYLOAD_MAX)
        return 0;
    out[0] = flags;
    put(out + 1, size - (s->protocol == 11 ? 4 : 0));
    put(out + 3, seq);
    put(out + 5, ls_reliable_low(s->link));
    out[7] = s->protocol == 11 ? 3 : 0;
    memcpy(out + 8, payload, size);
    return size + 8;
}
size_t ls_stream_send(LsStream* s, const uint8_t* data, size_t size, bool opening, uint32_t now,
                      uint8_t* out, size_t capacity) {
    if (!s || s->failed || !out || (size && !data) || opening == s->opened)
        return 0;
    unsigned prefix = s->protocol == 11 ? 4 : 0;
    if (size > LS_RELIABLE_PAYLOAD_MAX - prefix || size + prefix + 8 > capacity)
        return 0;
    uint8_t payload[LS_RELIABLE_PAYLOAD_MAX] = {0};
    if (prefix)
        payload[3] = s->local;
    if (size)
        memcpy(payload + prefix, data, size);
    uint16_t seq;
    uint8_t flags = opening ? 15 : 7;
    if (!ls_reliable_queue(s->link, payload, size + prefix, flags, now, &seq))
        return 0;
    s->opened = true;
    return encode(s, seq, flags, payload, size + prefix, out, capacity);
}
size_t ls_stream_retransmit(LsStream* s, uint32_t now, uint8_t* out, size_t capacity) {
    /* Reserve room before consuming a due retransmission's timer. */
    if (!s || !out || capacity < LS_STREAM_FRAME_MAX)
        return 0;
    const uint8_t* payload;
    size_t size;
    uint16_t seq;
    uint8_t flags;
    if (!ls_reliable_due(s->link, now, &seq, &payload, &size, &flags))
        return 0;
    return encode(s, seq, flags, payload, size, out, capacity);
}
size_t ls_stream_ack(const LsStream* s, uint8_t* out, size_t capacity) {
    if (!s || s->failed)
        return 0;
    uint8_t ack[20], body[78] = {0};
    ls_reliable_ack_payload(s->link, ack);
    if (s->protocol == 10)
        return encode(s, BASE, 0, ack, sizeof(ack), out, capacity);
    body[3] = s->local;
    body[5] = 4;
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t* entry = body + 6 + 18 * i;
        if (i == (s->local == 2 ? 1U : 0U))
            memcpy(entry, ack + 2, 18);
        else
            put(entry, BASE);
    }
    return encode(s, BASE, 0, body, sizeof(body), out, capacity);
}
size_t ls_stream_outstanding(const LsStream* s) { return s ? ls_reliable_outstanding(s->link) : 0; }
static bool application(LsStream* s, const Slot* slot, LsStreamDelivery deliver, void* context) {
    bool first = slot->flags & 2, last = slot->flags & 4;
    if (first == s->assembling)
        return false;
    if (first) {
        s->assembling = true;
        s->assembled = 0;
    }
    if (slot->size > sizeof(s->message) - s->assembled)
        return false;
    memcpy(s->message + s->assembled, slot->data, slot->size);
    s->assembled += slot->size;
    if (last) {
        s->assembling = false;
        deliver(context, s->protocol, s->message, s->assembled);
        s->assembled = 0;
    }
    return true;
}
int ls_stream_receive(LsStream* s, const uint8_t* p, size_t size, uint32_t now,
                      LsStreamDelivery deliver, void* context) {
    if (!s || s->failed || !p || !deliver || size < 8 || size > LS_STREAM_FRAME_MAX)
        return 0;
    unsigned prefix = s->protocol == 11 ? 4 : 0;
    size_t declared = word(p + 1);
    if (size != 8 + prefix + declared || p[7] != (prefix ? 3 : 0))
        return 0;
    const uint8_t* data = p + 8;
    if (prefix && (data[0] || data[1] || data[2] || data[3] != s->remote))
        return 0;
    data += prefix;
    uint8_t flags = p[0];
    if (!flags) {
        unsigned entries = prefix ? 4 : 1;
        if (declared != 2 + 18 * entries || data[0] || data[1] != entries)
            return 0;
        const uint8_t* entry = data + 2 + (prefix && s->local == 1 ? 18 : 0);
        ls_reliable_ack(s->link, word(entry), entry + 2, now);
        return 1;
    }
    if (!(flags & 1) || (flags & 0xf0))
        return 0;
    uint16_t seq = word(p + 3), offset = seq - s->next;
    /* The opening cannot be consumed by a non-INIT frame. Later packets can be
       retained while that opening is in flight. A reset needs a new session. */
    if ((seq == BASE && !s->received_opening && !(flags & 8)) || ((flags & 8) && seq != BASE))
        return 0;
    if (offset >= 0x8000 || offset > LS_RELIABLE_WINDOW)
        return 2;
    Slot* slot = s->slots + (s->head + offset) % RX_SLOTS;
    if (slot->data)
        return 2;
    if (declared > LS_RELIABLE_BYTES_MAX - s->queued_bytes)
        return 0;
    uint8_t* copy = ls_alloc(declared ? declared : 1);
    if (!copy)
        return 0;
    memcpy(copy, data, declared);
    *slot = (Slot){copy, declared, flags};
    if (seq == BASE && (flags & 8))
        s->received_opening = true;
    s->queued_bytes += declared;
    /* Never ACK bytes we could not retain. Receive ACK bookkeeping advances
       independently; delivery drains only contiguous retained slots. */
    if (!ls_reliable_received(s->link, seq)) {
        s->failed = true;
        return -1;
    }
    while (s->slots[s->head].data) {
        slot = s->slots + s->head;
        bool ok = application(s, slot, deliver, context);
        s->queued_bytes -= slot->size;
        free(slot->data);
        memset(slot, 0, sizeof(*slot));
        s->head = (s->head + 1) % RX_SLOTS;
        ++s->next;
        if (!ok) {
            s->failed = true;
            return -1;
        }
    }
    return 2;
}
