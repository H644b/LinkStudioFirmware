#include "ls_connect.h"
#include <string.h>

typedef struct {
    const uint8_t* data;
    size_t size, offset;
    bool valid;
} Reader;
typedef struct {
    uint8_t* data;
    size_t capacity, offset;
    bool valid;
} Writer;
static void read_bytes(Reader* r, uint8_t* out, size_t size) {
    if (!r->valid || size > r->size - r->offset) {
        r->valid = false;
        return;
    }
    memcpy(out, r->data + r->offset, size);
    r->offset += size;
}
static uint32_t read_number(Reader* r, unsigned width) {
    uint8_t bytes[4] = {0};
    read_bytes(r, bytes, width);
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value = (value << 8) | bytes[i];
    return value;
}
static void write_bytes(Writer* w, const uint8_t* data, size_t size) {
    if (!w->valid || size > w->capacity - w->offset) {
        w->valid = false;
        return;
    }
    memcpy(w->data + w->offset, data, size);
    w->offset += size;
}
static void number(Writer* w, uint64_t value, unsigned width) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < width; ++i)
        bytes[width - 1 - i] = (uint8_t)(value >> (8 * i));
    write_bytes(w, bytes, width);
}
static bool valid_host(const LsConnectHost* h) {
    static const uint8_t zero[6] = {0};
    return h && h->variable > 1 && h->ip && h->guest_ip && h->ip != h->guest_ip &&
           !(h->mac[0] & 1) && memcmp(h->mac, zero, 6) && !(h->guest_mac[0] & 1) &&
           memcmp(h->guest_mac, zero, 6) && memcmp(h->mac, h->guest_mac, 6);
}
static bool valid_join(const LsConnectJoin* j) {
    if (!j || j->protocol_count > LS_CONNECT_PROTOCOLS_MAX ||
        j->player_count > LS_CONNECT_PLAYERS_MAX)
        return false;
    for (unsigned i = 0; i < j->player_count; ++i)
        if (j->players[i].name_size > LS_CONNECT_NAME_MAX)
            return false;
    return true;
}
void ls_connect_constant_id(const uint8_t mac[6], uint8_t out[8]) {
    uint8_t result[] = {mac[2], mac[4], mac[5], mac[3], mac[1], mac[0], 0, 0};
    memcpy(out, result, sizeof(result));
}
bool ls_connect_parse_join(const uint8_t* data, size_t size, LsConnectJoin* out) {
    if (!data || !out || size > LS_CONNECT_MESSAGE_MAX)
        return false;
    Reader r = {data, size, 0, true};
    LsConnectJoin j = {0};
    if (read_number(&r, 1) != 0)
        return false;
    j.protocol_count = read_number(&r, 1);
    if (j.protocol_count > LS_CONNECT_PROTOCOLS_MAX)
        return false;
    read_bytes(&r, &j.protocols[0][0], j.protocol_count * 2);
    read_bytes(&r, j.app_version, 2);
    read_bytes(&r, j.random, 4);
    read_bytes(&r, j.source_constant, 8);
    j.source_var = read_number(&r, 2);
    j.nat_mapping = read_number(&r, 1);
    j.private_ipv6 = read_number(&r, 1);
    read_bytes(&r, j.token, 32);
    read_bytes(&r, j.destination_constant, 8);
    j.destination_var = read_number(&r, 2);
    j.player_count = read_number(&r, 1);
    j.participant_count = read_number(&r, 1);
    if (j.player_count > LS_CONNECT_PLAYERS_MAX || read_number(&r, 1) != 0)
        return false;
    j.ip = read_number(&r, 4);
    j.port = read_number(&r, 2);
    for (unsigned i = 0; i < j.player_count; ++i) {
        LsConnectPlayer* p = &j.players[i];
        read_bytes(&r, p->id, 16);
        uint32_t count = read_number(&r, 4);
        if (count > LS_CONNECT_NAME_MAX)
            return false;
        p->name_size = count;
        p->encoding = read_number(&r, 1);
        read_bytes(&r, p->name, count);
    }
    if (!r.valid || r.offset != size)
        return false;
    *out = j;
    return true;
}
bool ls_connect_join_matches(const LsConnectHost* h, const LsConnectJoin* j, uint16_t source) {
    if (!valid_host(h) || !valid_join(j) || !j->player_count || j->participant_count != 1 ||
        j->nat_mapping || j->private_ipv6 || j->source_var <= 1 || j->source_var == h->variable ||
        j->source_var != source || j->destination_var != h->variable || j->ip != h->guest_ip ||
        j->port != 12345)
        return false;
    uint8_t local[8], guest[8];
    ls_connect_constant_id(h->mac, local);
    ls_connect_constant_id(h->guest_mac, guest);
    return !memcmp(local, j->destination_constant, 8) && !memcmp(guest, j->source_constant, 8);
}
static void station_address(Writer* w, uint32_t ip, uint8_t rank) {
    number(w, 0, 1);
    number(w, rank, 1);
    number(w, 0, 2);
    number(w, ip, 4);
    number(w, 0, 8);
    number(w, 0, 4);
    number(w, ip ? 12345 : 0, 2);
}
size_t ls_connect_net_status(const LsConnectHost* h, uint8_t* out, size_t capacity) {
    if (!valid_host(h) || !out)
        return 0;
    Writer w = {out, capacity, 0, true};
    uint8_t constant[8];
    ls_connect_constant_id(h->mac, constant);
    number(&w, 0x0111, 2);
    number(&w, 4 * 22, 2);
    number(&w, 2, 4);
    number(&w, h->variable, 2);
    write_bytes(&w, constant, 8);
    number(&w, h->network_id, 8);
    number(&w, 1, 1);
    number(&w, 4, 2);
    number(&w, 0, 1);
    station_address(&w, h->ip, 0);
    station_address(&w, h->guest_ip, 1);
    station_address(&w, 0, 255);
    station_address(&w, 0, 255);
    return w.valid ? w.offset : 0;
}
size_t ls_connect_net_property(const LsConnectHost* h, const uint8_t app[112], uint8_t* out,
                               size_t capacity) {
    if (!valid_host(h) || !app || !out)
        return 0;
    Writer w = {out, capacity, 0, true};
    number(&w, 0x0150, 2);
    number(&w, 112, 2);
    number(&w, 1, 4);
    number(&w, h->network_id, 8);
    number(&w, 2, 2);
    number(&w, 4, 2);
    number(&w, 0, 6);
    number(&w, 1, 2);
    number(&w, 0x0201, 2);
    number(&w, 92, 4);
    number(&w, 20, 4);
    write_bytes(&w, app, 112);
    return w.valid ? w.offset : 0;
}
bool ls_connect_net_ack(const uint8_t* data, size_t size, uint8_t kind, uint32_t sequence) {
    if (!data || size != 8 || (kind != 0x12 && kind != 0x51) || data[0] != 1 || data[1] != kind ||
        data[2] || data[3])
        return false;
    Reader r = {data, size, 4, true};
    return read_number(&r, 4) == sequence;
}
size_t ls_connect_join_response(const LsConnectHost* h, const LsConnectJoin* j,
                                const uint8_t random[4], uint8_t* out, size_t capacity) {
    if (!valid_host(h) || !valid_join(j) || !random || !out)
        return 0;
    uint8_t constant[8], version = 7;
    ls_connect_constant_id(h->mac, constant);
    for (unsigned i = 0; i < j->protocol_count; ++i)
        if (j->protocols[i][0] == 13)
            version = j->protocols[i][1];
    Writer w = {out, capacity, 0, true};
    number(&w, 0x020d, 2);
    number(&w, version, 1);
    number(&w, 1, 1);
    number(&w, 0, 4);
    write_bytes(&w, random, 4);
    write_bytes(&w, constant, 8);
    number(&w, h->variable, 2);
    write_bytes(&w, j->source_constant, 8);
    number(&w, j->source_var, 2);
    number(&w, 1, 1);
    number(&w, 1, 2);
    number(&w, 0, 2);
    return w.valid ? w.offset : 0;
}
static void player(Writer* w, const LsConnectPlayer* p) {
    write_bytes(w, p->id, 16);
    number(w, p->name_size, 4);
    number(w, p->encoding, 1);
    write_bytes(w, p->name, p->name_size);
}
static void session_station(Writer* w, const uint8_t constant[8], uint16_t variable, uint32_t ip,
                            uint16_t port, uint8_t index, const uint8_t token[32],
                            uint8_t participants, const LsConnectPlayer* players, uint8_t count) {
    write_bytes(w, constant, 8);
    number(w, variable, 2);
    number(w, ip, 4);
    number(w, port, 2);
    number(w, index, 1);
    number(w, index, 2);
    number(w, 0, 2);
    write_bytes(w, token, 32);
    number(w, count, 1);
    number(w, participants, 1);
    number(w, 0, 2);
    for (unsigned i = 0; i < count; ++i)
        player(w, players + i);
}
size_t ls_connect_session_update(const LsConnectHost* h, const LsConnectJoin* j, uint16_t update,
                                 uint8_t* out, size_t capacity) {
    if (!valid_host(h) || !valid_join(j) || !j->player_count || !out)
        return 0;
    uint8_t constant[8];
    ls_connect_constant_id(h->mac, constant);
    static const uint8_t token[32] = {6};
    static const LsConnectPlayer local = {
        .id = {0, 0, 0, 0, 0, 0, 0, 1}, .encoding = 1, .name_size = 1, .name = {' '}};
    Writer w = {out, capacity, 0, true};
    number(&w, 5, 1);
    number(&w, update, 2);
    number(&w, 1, 1);
    number(&w, 0, 1);
    number(&w, 3, 2);
    write_bytes(&w, constant, 8);
    number(&w, h->variable, 2);
    number(&w, 0x0200, 2);
    number(&w, 1, 2);
    number(&w, update, 2);
    number(&w, 0, 4);
    session_station(&w, constant, h->variable, h->ip, 12345, 0, token, 1, &local, 1);
    session_station(&w, j->source_constant, j->source_var, j->ip, j->port, 1, j->token,
                    j->participant_count, j->players, j->player_count);
    return w.valid ? w.offset : 0;
}
bool ls_connect_update_ack(const uint8_t* data, size_t size, const uint8_t constant[8],
                           uint32_t* sequence) {
    if (!data || !constant || !sequence || size != 15 || data[0] != 6 ||
        memcmp(data + 1, constant, 8) || data[13] || data[14] != 1)
        return false;
    Reader r = {data, size, 9, true};
    *sequence = read_number(&r, 4);
    return true;
}
size_t ls_connect_rtt_request(uint64_t tick, uint64_t micros, uint16_t variable, uint8_t* out,
                              size_t capacity) {
    if (!out)
        return 0;
    Writer w = {out, capacity, 0, true};
    number(&w, 0, 1);
    number(&w, tick, 8);
    number(&w, micros, 8);
    number(&w, 0, 2);
    number(&w, variable, 2);
    return w.valid ? w.offset : 0;
}
size_t ls_connect_rtt_response(const uint8_t* request, size_t size, uint64_t micros,
                               uint16_t requester, uint8_t* out, size_t capacity) {
    if (!request || size != 21 || request[0] || !out || capacity < 21)
        return 0;
    memmove(out, request, 21);
    out[0] = 1;
    Writer w = {out, capacity, 9, true};
    number(&w, micros, 8);
    number(&w, requester, 2);
    return 21;
}
