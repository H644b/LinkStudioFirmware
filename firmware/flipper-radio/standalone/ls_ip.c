#include "ls_ip.h"
#include "ls_alloc.h"
#include <string.h>

#define UDP_MAX (LS_IP_DATAGRAM_MAX + 8)
#define FRAGMENT_SLOTS 4
typedef struct {
    bool used, invalid, final;
    uint16_t id, total, high, covered;
    uint32_t started, destination;
    uint8_t data[UDP_MAX], present[(UDP_MAX + 7) / 8];
} Fragments;
struct LsIp {
    LsIpConfig config;
    LsIpOutput output;
    LsIpDatagram datagram;
    void* context;
    uint16_t id;
    Fragments fragments[FRAGMENT_SLOTS];
    uint8_t tx_udp[UDP_MAX], tx_frame[LS_IP_FRAME_MAX];
};
static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
static uint16_t be16(const uint8_t* p) { return (uint16_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }
static void put16(uint8_t* p, uint16_t n) {
    p[0] = n >> 8;
    p[1] = n;
}
static void put32(uint8_t* p, uint32_t n) {
    put16(p, n >> 16);
    put16(p + 2, n);
}
static uint32_t sum(const uint8_t* data, size_t size) {
    uint32_t n = 0;
    while (size >= 2) {
        n += be16(data);
        data += 2;
        size -= 2;
    }
    if (size)
        n += (uint16_t)*data << 8;
    return n;
}
static uint16_t finish(uint32_t n) {
    while (n >> 16)
        n = (n & 65535) + (n >> 16);
    return (uint16_t)~n;
}
static uint32_t pseudo(uint32_t a, uint32_t b, size_t size) {
    return (a >> 16) + (a & 65535) + (b >> 16) + (b & 65535) + 17 + size;
}
static bool valid_mac(const uint8_t mac[6]) {
    static const uint8_t zero[6] = {0};
    return !(mac[0] & 1) && memcmp(mac, zero, 6);
}
LsIp* ls_ip_alloc(const LsIpConfig* c, LsIpOutput output, LsIpDatagram datagram, void* context) {
    if (!c || !output || !datagram || !valid_mac(c->local_mac) || !valid_mac(c->peer_mac) ||
        !memcmp(c->local_mac, c->peer_mac, 6) || !c->local_ip || !c->peer_ip ||
        c->local_ip == c->peer_ip)
        return NULL;
    LsIp* ip = ls_calloc(1, sizeof(*ip));
    if (ip) {
        ip->config = *c;
        ip->output = output;
        ip->datagram = datagram;
        ip->context = context;
    }
    return ip;
}
void ls_ip_free(LsIp* ip) { free(ip); }
static void ethernet(LsIp* ip, uint8_t* out, uint16_t type) {
    memcpy(out, ip->config.peer_mac, 6);
    memcpy(out + 6, ip->config.local_mac, 6);
    put16(out + 12, type);
}
bool ls_ip_send(LsIp* ip, const uint8_t* data, size_t size) {
    if (!ip || size > LS_IP_DATAGRAM_MAX || (size && !data))
        return false;
    uint8_t* udp = ip->tx_udp;
    size_t total = size + 8;
    put16(udp, 12345);
    put16(udp + 2, 12345);
    put16(udp + 4, total);
    put16(udp + 6, 0);
    if (size)
        memcpy(udp + 8, data, size);
    uint16_t check = finish(pseudo(ip->config.local_ip, ip->config.peer_ip, total) + sum(udp, total));
    put16(udp + 6, check ? check : 65535);
    uint16_t id = ++ip->id;
    for (size_t offset = 0; offset < total; offset += 1480) {
        size_t n = total - offset;
        if (n > 1480)
            n = 1480;
        uint8_t* frame = ip->tx_frame;
        ethernet(ip, frame, 0x800);
        uint8_t* h = frame + 14;
        memset(h, 0, 20);
        h[0] = 0x45;
        h[8] = 64;
        h[9] = 17;
        put16(h + 2, 20 + n);
        put16(h + 4, id);
        put16(h + 6, (offset / 8) | (offset + n < total ? 0x2000 : 0));
        put32(h + 12, ip->config.local_ip);
        put32(h + 16, ip->config.peer_ip);
        put16(h + 10, finish(sum(h, 20)));
        memcpy(h + 20, udp + offset, n);
        if (!ip->output(ip->context, frame, n + 34))
            return false;
    }
    return true;
}
static bool arp(LsIp* ip, const uint8_t* p, size_t size) {
    if (size < 28 || be16(p) != 1 || be16(p + 2) != 0x800 || p[4] != 6 || p[5] != 4 ||
        (be16(p + 6) != 1 && be16(p + 6) != 2) || memcmp(p + 8, ip->config.peer_mac, 6) ||
        (be32(p + 14) != ip->config.peer_ip && be32(p + 14) != 0) ||
        be32(p + 24) != ip->config.local_ip)
        return false;
    if (be16(p + 6) == 2)
        return true;
    // A request/probe for our IP gets a reply even when its sender IP is zero.
    uint8_t frame[42];
    ethernet(ip, frame, 0x806);
    uint8_t* a = frame + 14;
    memcpy(a, p, 8);
    put16(a + 6, 2);
    memcpy(a + 8, ip->config.local_mac, 6);
    put32(a + 14, ip->config.local_ip);
    memcpy(a + 18, ip->config.peer_mac, 6);
    memcpy(a + 24, p + 14, 4);
    return ip->output(ip->context, frame, sizeof(frame));
}
static bool udp(LsIp* ip, const uint8_t* p, size_t size, uint32_t destination) {
    if (size < 8 || size > UDP_MAX || be16(p) != 12345 || be16(p + 2) != 12345 ||
        be16(p + 4) != size ||
        (be16(p + 6) && finish(pseudo(ip->config.peer_ip, destination, size) + sum(p, size))))
        return false;
    ip->datagram(ip->context, p + 8, size - 8);
    return true;
}
static bool fragment(LsIp* ip, const uint8_t* h, const uint8_t* data, size_t size, uint16_t flags,
                     uint32_t now) {
    size_t offset = (flags & 0x1fff) * 8, end = offset + size;
    bool more = flags & 0x2000;
    if (!size || end > UDP_MAX || (more && size % 8))
        return false;
    Fragments *f = NULL, *free_slot = NULL;
    for (unsigned i = 0; i < FRAGMENT_SLOTS; ++i) {
        Fragments* entry = ip->fragments + i;
        if (entry->used && (uint32_t)(now - entry->started) >= 5000)
            entry->used = false;
        if (!entry->used)
            free_slot = entry;
        else if (entry->id == be16(h + 4) && entry->destination == be32(h + 16))
            f = entry;
    }
    if (!f) {
        if (!free_slot)
            return false;
        f = free_slot;
        memset(f, 0, sizeof(*f));
        f->used = true;
        f->id = be16(h + 4);
        f->destination = be32(h + 16);
        f->started = now;
    }
    if (f->invalid)
        return false;
    if ((f->final && (end > f->total || (!more && end != f->total) || (more && end >= f->total))) ||
        (!more && end < f->high)) {
        f->invalid = true;
        return false;
    }
    if (!more) {
        f->total = end;
        f->final = true;
    }
    for (size_t i = 0; i < size; ++i) {
        size_t at = offset + i;
        uint8_t mask = 1U << (at % 8);
        if (f->present[at / 8] & mask) {
            if (f->data[at] != data[i]) {
                f->invalid = true;
                return false;
            }
        } else {
            f->data[at] = data[i];
            f->present[at / 8] |= mask;
            ++f->covered;
        }
    }
    if (end > f->high)
        f->high = end;
    if (f->final && f->covered == f->total) {
        bool ok = udp(ip, f->data, f->total, f->destination);
        f->used = false;
        return ok;
    }
    return true;
}
bool ls_ip_receive(LsIp* ip, const uint8_t* frame, size_t size, uint32_t now) {
    if (!ip || !frame || size < 14 || size > LS_IP_FRAME_MAX ||
        memcmp(frame + 6, ip->config.peer_mac, 6) ||
        (memcmp(frame, ip->config.local_mac, 6) && memcmp(frame, broadcast, 6)))
        return false;
    const uint8_t* h = frame + 14;
    size -= 14;
    if (be16(frame + 12) == 0x806)
        return arp(ip, h, size);
    if (be16(frame + 12) != 0x800 || size < 20 || h[0] >> 4 != 4)
        return false;
    size_t length = (h[0] & 15) * 4, total = be16(h + 2);
    uint32_t destination = be32(h + 16);
    /* The seated Switch sends protocol-11 ACKs to the LDN /24 broadcast.
       Keep peer binding and verify the checksum against the actual IP target. */
    bool ours = destination == ip->config.local_ip || destination == UINT32_MAX ||
                destination == (ip->config.local_ip | 255U);
    if (length < 20 || length > size || total < length || total > size || !h[8] || h[9] != 17 ||
        be32(h + 12) != ip->config.peer_ip || !ours ||
        finish(sum(h, length)))
        return false;
    uint16_t flags = be16(h + 6);
    if ((flags & 0x8000) || ((flags & 0x4000) && (flags & 0x3fff)))
        return false;
    if (flags & 0x3fff)
        return fragment(ip, h, h + length, total - length, flags, now);
    return udp(ip, h + length, total - length, destination);
}
