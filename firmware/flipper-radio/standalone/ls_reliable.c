#include "ls_reliable.h"
#include "ls_alloc.h"
#include <string.h>

typedef struct {
    uint8_t* data;
    size_t size;
    uint32_t last_ms, resends;
    uint8_t flags;
    bool acked;
} Pending;
struct LsReliable {
    Pending pending[LS_RELIABLE_WINDOW];
    uint16_t next, low, recv_next, gap;
    bool received[LS_RELIABLE_WINDOW + 1], gap_pending;
    size_t bytes;
    uint32_t rtt[7];
    uint8_t rtt_size, rtt_index;
};
static bool before(uint16_t a, uint16_t b) {
    uint16_t d = b - a;
    return d && d < 0x8000;
}
LsReliable* ls_reliable_alloc(uint16_t start) {
    LsReliable* link = ls_calloc(1, sizeof(*link));
    if (link)
        link->next = link->low = link->recv_next = start;
    return link;
}
void ls_reliable_free(LsReliable* link) {
    if (!link)
        return;
    for (unsigned i = 0; i < LS_RELIABLE_WINDOW; ++i)
        free(link->pending[i].data);
    free(link);
}
bool ls_reliable_queue(LsReliable* link, const uint8_t* data, size_t size, uint8_t flags,
                       uint32_t now_ms, uint16_t* sequence) {
    if (!link || !sequence || (size && !data) || size > LS_RELIABLE_PAYLOAD_MAX ||
        (uint16_t)(link->next - link->low) >= LS_RELIABLE_WINDOW ||
        size > LS_RELIABLE_BYTES_MAX - link->bytes)
        return false;
    uint8_t* copy = ls_alloc(size ? size : 1);
    if (!copy)
        return false;
    if (size)
        memcpy(copy, data, size);
    *sequence = link->next;
    Pending* entry = &link->pending[link->next % LS_RELIABLE_WINDOW];
    *entry = (Pending){.data = copy, .size = size, .flags = flags, .last_ms = now_ms};
    ++link->next;
    link->bytes += size;
    return true;
}
uint16_t ls_reliable_next(const LsReliable* link) { return link->next; }
uint16_t ls_reliable_low(const LsReliable* link) { return link->low; }
size_t ls_reliable_outstanding(const LsReliable* link) {
    size_t n = 0;
    for (uint16_t seq = link->low; seq != link->next; ++seq)
        if (!link->pending[seq % LS_RELIABLE_WINDOW].acked)
            ++n;
    return n;
}
static uint32_t median(const LsReliable* link) {
    uint32_t sorted[7];
    memcpy(sorted, link->rtt, sizeof(sorted));
    for (unsigned i = 1; i < link->rtt_size; ++i) {
        uint32_t value = sorted[i];
        unsigned j = i;
        while (j && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    return sorted[link->rtt_size / 2];
}
uint32_t ls_reliable_rto(const LsReliable* link) {
    if (!link->rtt_size)
        return 500;
    uint64_t value = 33 + ((uint64_t)median(link) * 14 + 9) / 10;
    return value > 500 ? 500 : (uint32_t)value;
}
void ls_reliable_ack(LsReliable* link, uint16_t next_expected, const uint8_t mask[16],
                     uint32_t now_ms) {
    /* A receiver cannot acknowledge sequence numbers we have not queued. */
    if (before(link->next, next_expected) || (uint16_t)(link->next - next_expected) == 0x8000)
        return;
    bool selective = false;
    if (mask)
        for (unsigned i = 0; i < 16; ++i)
            selective |= mask[i] != 0;
    for (uint16_t seq = link->low; seq != link->next; ++seq) {
        Pending* entry = &link->pending[seq % LS_RELIABLE_WINDOW];
        uint16_t bit = seq - next_expected - 1;
        bool arrived = before(seq, next_expected) ||
                       (mask && bit < 128 && (mask[bit / 8] & (1U << (bit % 8))));
        if (arrived && !entry->acked) {
            if (!entry->resends) {
                link->rtt[link->rtt_index] = now_ms - entry->last_ms;
                link->rtt_index = (link->rtt_index + 1) % 7;
                if (link->rtt_size < 7)
                    ++link->rtt_size;
            }
            entry->acked = true;
        }
    }
    while (link->low != link->next) {
        Pending* entry = &link->pending[link->low % LS_RELIABLE_WINDOW];
        if (!entry->acked)
            break;
        link->bytes -= entry->size;
        free(entry->data);
        memset(entry, 0, sizeof(*entry));
        ++link->low;
    }
    link->gap_pending =
        selective && (uint16_t)(next_expected - link->low) < (uint16_t)(link->next - link->low) &&
        !link->pending[next_expected % LS_RELIABLE_WINDOW].acked;
    link->gap = next_expected;
}
bool ls_reliable_due(LsReliable* link, uint32_t now_ms, uint16_t* sequence, const uint8_t** data,
                     size_t* size, uint8_t* flags) {
    uint32_t rto = ls_reliable_rto(link), fast = link->rtt_size ? median(link) : 25;
    if (fast < 25)
        fast = 25;
    for (uint16_t seq = link->low; seq != link->next; ++seq) {
        Pending* entry = &link->pending[seq % LS_RELIABLE_WINDOW];
        if (entry->acked)
            continue;
        bool is_gap = link->gap_pending && seq == link->gap;
        if ((is_gap && (!entry->resends || now_ms - entry->last_ms >= fast)) ||
            now_ms - entry->last_ms >= rto) {
            if (is_gap)
                link->gap_pending = false;
            entry->last_ms = now_ms;
            if (entry->resends != UINT32_MAX)
                ++entry->resends;
            *sequence = seq;
            *data = entry->data;
            *size = entry->size;
            *flags = entry->flags;
            return true;
        }
    }
    return false;
}
bool ls_reliable_received(LsReliable* link, uint16_t sequence) {
    uint16_t offset = sequence - link->recv_next;
    if (offset > LS_RELIABLE_WINDOW || link->received[offset])
        return false;
    link->received[offset] = true;
    while (link->received[0]) {
        memmove(link->received, link->received + 1, LS_RELIABLE_WINDOW * sizeof(bool));
        link->received[LS_RELIABLE_WINDOW] = false;
        ++link->recv_next;
    }
    return true;
}
void ls_reliable_ack_payload(const LsReliable* link, uint8_t out[20]) {
    memset(out, 0, 20);
    out[1] = 1;
    out[2] = link->recv_next >> 8;
    out[3] = link->recv_next;
    for (unsigned bit = 0; bit < 128; ++bit)
        if (link->received[bit + 1])
            out[4 + bit / 8] |= 1U << (bit % 8);
}
