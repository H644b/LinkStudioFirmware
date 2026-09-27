#include "ls_session.h"
#include "ls_alloc.h"
#include "ls_game.h"
#include "ls_pa9.h"
#include "ls_pia.h"
#include "ls_recipe.h"
#include "ls_stream.h"
#include <string.h>

#define QUEUE_MAX 32
#define APPLICATION_MAX 1300
typedef struct {
    uint32_t due;
    uint16_t size;
    uint8_t protocol, kind;
    bool opening;
    uint8_t data[APPLICATION_MAX];
} Scheduled;
enum { NORMAL, LOCAL_PICK };
enum { READY, PREVIEW, PICK, CONFIRM, CANCEL, COMMIT, SAVE };
typedef struct {
    uint8_t kind;
    uint16_t revision;
    uint32_t state;
    int32_t reason;
    uint8_t entity[344];
} Game;
struct LsSession {
    LsSessionConfig config;
    LsConnectJoin join;
    LsRecipe recipe;
    uint8_t recipe_bytes[LS_RECIPE_MAX];
    LsPia* pia;
    LsStream* streams[2];
    LsSessionOutput output;
    void* context;
    uint32_t now, start, next_net, next_update, next_property, next_rtt;
    uint32_t error, completed, dropped, peer_hash[2];
    uint16_t packet[2], revision, peer_revision;
    bool joined, net_sent, net_acked, property_sent, property_acked;
    bool update_sent[2], update_acked[2], identity[2];
    bool offered, pick_sent, peer_pick, confirmed, complete, stopping;
    uint8_t barriers, states[32], incoming[344], last_received[344];
    Scheduled queue[QUEUE_MAX];
    unsigned count, head, pending_count;
    Game pending[3];
    uint8_t wire[LS_STREAM_FRAME_MAX], datagram[LS_PIA_DATAGRAM_MAX];
};
static bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }
static void fail(LsSession* s, uint32_t error) {
    if (!s->error)
        s->error = error;
}
static size_t outstanding(const LsSession* s) {
    return ls_stream_outstanding(s->streams[0]) + ls_stream_outstanding(s->streams[1]);
}
bool ls_session_safe_to_stop(const LsSession* s) {
    return s && (!s->confirmed || (s->complete && !s->count && !outstanding(s)));
}
static bool send(LsSession* s, uint8_t protocol, const uint8_t* data, size_t size,
                 uint16_t destination, bool net, int16_t flags) {
    if (!size) {
        fail(s, LS_SESSION_ENCODE_ERROR);
        return false;
    }
    LsPiaMessage message = {protocol, flags, size, data};
    unsigned channel = destination == 1;
    uint16_t packet = net ? 0 : s->packet[channel];
    size_t n =
        ls_pia_encode(s->pia, &message, 1, s->config.host.ip, destination, s->config.host.variable,
                      packet, net, !net, s->join.source_var, s->datagram, sizeof(s->datagram));
    if (!n) {
        fail(s, LS_SESSION_ENCODE_ERROR);
        return false;
    }
    if (!net)
        s->packet[channel] = packet == UINT16_MAX ? 1 : packet + 1;
    if (!s->output(s->context, s->datagram, n, s->config.host.guest_ip))
        ++s->dropped;
    return true;
}
static bool schedule(LsSession* s, uint8_t protocol, const uint8_t* data, size_t size, bool opening,
                     uint32_t delay, uint8_t kind) {
    if (s->count == QUEUE_MAX || size > APPLICATION_MAX) {
        fail(s, LS_SESSION_QUEUE_FULL);
        return false;
    }
    uint32_t when = s->now + delay;
    if (s->count) {
        uint32_t previous = s->queue[(s->head + s->count - 1) % QUEUE_MAX].due + 50;
        if (!due(when, previous))
            when = previous;
    }
    Scheduled* q = s->queue + (s->head + s->count++) % QUEUE_MAX;
    *q = (Scheduled){
        .due = when, .size = size, .protocol = protocol, .kind = kind, .opening = opening};
    memcpy(q->data, data, size);
    return true;
}
static bool schedule_offer(LsSession* s, bool preview, uint32_t delay) {
    uint8_t data[356];
    size_t n = ls_game_offer(s->revision, s->recipe.entity, preview, data, sizeof(data));
    return schedule(s, 10, data, n, false, delay, preview ? NORMAL : LOCAL_PICK);
}
static bool schedule_revision(LsSession* s, uint8_t command, uint32_t delay) {
    uint8_t data[7];
    size_t n = ls_game_revision(command, s->revision, data, sizeof(data));
    return schedule(s, 10, data, n, false, delay, NORMAL);
}
static void net_status(LsSession* s) {
    uint8_t body[512];
    size_t n = ls_connect_net_status(&s->config.host, body, sizeof(body));
    s->net_sent |= send(s, 1, body, n, 0, true, -1);
}
static void property(LsSession* s) {
    uint8_t body[512];
    size_t n = ls_connect_net_property(&s->config.host, s->config.application, body, sizeof(body));
    s->property_sent |= send(s, 1, body, n, 0, true, -1);
}
static void update(LsSession* s, unsigned sequence) {
    uint8_t body[512];
    size_t n = ls_connect_session_update(&s->config.host, &s->join, sequence, body, sizeof(body));
    s->update_sent[sequence] |= send(s, 13, body, n, 1, false, -1);
}
static void connection(LsSession* s, uint16_t source, const uint8_t* data, size_t size) {
    if (!size)
        return;
    if (!data[0]) {
        LsConnectJoin join;
        if (!ls_connect_parse_join(data, size, &join) ||
            !ls_connect_join_matches(&s->config.host, &join, source))
            return;
        bool first = !s->joined;
        /* A retry may repeat a join but cannot replace this seated session. */
        if (!first && (memcmp(&join, &s->join, sizeof(join)) || s->update_acked[0]))
            return;
        s->join = join;
        s->joined = true;
        uint8_t body[512];
        size_t n = ls_connect_join_response(&s->config.host, &s->join, s->config.random, body,
                                            sizeof(body));
        send(s, 13, body, n, source, false, -1);
        update(s, 0);
        s->next_update = s->now + 1000;
        if (first) {
            schedule(s, 10, s->recipe.identity, s->recipe.identity_size, true, 0, NORMAL);
            schedule(s, 11, s->recipe.identity, s->recipe.identity_size, true, 0, NORMAL);
            schedule(s, 11, s->recipe.identity_tail, s->recipe.tail_size, false, 0, NORMAL);
            property(s);
            s->next_property = s->now + 500;
            s->next_rtt = s->now + 190;
        }
        return;
    }
    uint32_t sequence;
    if (!s->joined || !ls_connect_update_ack(data, size, s->join.source_constant, &sequence) ||
        sequence > 1 || !s->update_sent[sequence] || s->update_acked[sequence])
        return;
    s->update_acked[sequence] = true;
    if (!sequence)
        s->next_update = s->now + 1150;
    else {
        schedule(s, 10, s->recipe.ready, s->recipe.ready_size, false, 60, NORMAL);
        schedule(s, 10, s->recipe.ready, s->recipe.ready_size, false, 60, NORMAL);
        schedule_offer(s, true, 2700);
    }
}
static bool signed32(LsRead* r, int32_t* value) {
    if (r->pos >= r->size)
        return false;
    uint8_t tag = r->data[r->pos];
    if (tag >= 0xc0) {
        ++r->pos;
        *value = (int)tag - 256;
        return true;
    }
    if (tag < 0x84) {
        uint64_t n;
        if (!ls_game_uint(r, &n) || n > INT32_MAX)
            return false;
        *value = n;
        return true;
    }
    if (tag > 0x87)
        return false;
    ++r->pos;
    size_t width = (size_t)1 << (tag - 0x84);
    if (width > r->size - r->pos)
        return false;
    uint64_t n = 0;
    for (size_t i = 0; i < width; ++i)
        n |= (uint64_t)r->data[r->pos++] << (8 * i);
    bool negative = n >> (width * 8 - 1);
    if (negative && width < 8)
        n |= UINT64_MAX << (width * 8);
    if ((!negative && n > INT32_MAX) || (negative && n < UINT64_MAX - INT32_MAX))
        return false;
    *value = negative ? -1 - (int32_t)(UINT64_MAX - n) : (int32_t)n;
    return true;
}
static bool parse_game(const uint8_t* data, size_t size, Game* g) {
    if (!data || size < 4)
        return false;
    LsRead r = {data, size, 2};
    uint64_t value, preview;
    const uint8_t* blob;
    size_t n;
    memset(g, 0, sizeof(*g));
    if (data[0] == 2 && !data[1]) {
        if (!ls_game_group(&r, 0xb9, 1) || !ls_game_uint(&r, &value) || value > 255)
            return false;
        g->kind = SAVE;
        g->state = value;
        return r.pos == r.size;
    }
    if (data[0] != 1 || data[1] > 4)
        return false;
    unsigned fields = data[1] == 1 ? 3 : (data[1] == 0 || data[1] == 3) ? 2 : 1;
    if (!ls_game_group(&r, 0xb9, fields) || !ls_game_uint(&r, &value) || value > UINT16_MAX)
        return false;
    g->revision = value;
    switch (data[1]) {
    case 0:
        g->kind = READY;
        if (!ls_game_group(&r, 0xb9, 1) || !ls_game_blob(&r, &blob, &n) || n != 1200)
            return false;
        break;
    case 1: {
        uint8_t plain[344];
        if (!ls_game_blob(&r, &blob, &n) || n != 344 || !ls_pa9_decode(blob, plain) ||
            !ls_game_uint(&r, &preview) || preview > 1)
            return false;
        g->kind = preview ? PREVIEW : PICK;
        memcpy(g->entity, blob, 344);
        break;
    }
    case 2:
        g->kind = CONFIRM;
        break;
    case 3:
        g->kind = CANCEL;
        if (!signed32(&r, &g->reason))
            return false;
        break;
    case 4:
        g->kind = COMMIT;
        break;
    }
    return r.pos == r.size;
}
static void new_round(LsSession* s, uint16_t revision) {
    s->revision = revision;
    s->offered = s->pick_sent = s->peer_pick = s->confirmed = s->complete = false;
    s->barriers = 0;
    memset(s->states, 0, sizeof(s->states));
    s->count = s->head = s->pending_count = 0;
}
static void apply_game(LsSession* s, const Game* g) {
    if (s->error || !s->update_acked[1])
        return;
    bool next = (g->kind == READY || g->kind == PREVIEW || g->kind == PICK) && !g->revision;
    if (s->barriers == 15 && !s->complete && next) {
        unsigned i;
        for (i = 0; i < s->pending_count; ++i)
            if (s->pending[i].kind == g->kind)
                break;
        if (i == s->pending_count)
            ++s->pending_count; /* at most these three kinds */
        s->pending[i] = *g;
        return;
    }
    if (s->complete && next && !s->stopping) {
        s->peer_revision = 0;
        new_round(s, 0);
    }
    if (g->kind == SAVE) {
        if (!s->confirmed || s->complete || (s->states[g->state / 8] & (1U << (g->state % 8))))
            return;
        uint8_t reply[6] = {2, 1, 0xb9, 1, 0x80, g->state};
        if (g->state < 128)
            reply[4] = g->state;
        if (!schedule(s, 11, reply, g->state < 128 ? 5 : 6, false, 30, NORMAL))
            return;
        s->states[g->state / 8] |= 1U << (g->state % 8);
        switch (g->state) {
        case 3:
            s->barriers |= 1;
            break;
        case 6:
            s->barriers |= 2;
            break;
        case 11:
            s->barriers |= 4;
            break;
        case 14:
            s->barriers |= 8;
            break;
        }
        return;
    }
    /* A confirmed transaction cannot be canceled or replaced by an early
       next-round record, even when the controller has asked to stop. */
    if (s->confirmed || s->stopping)
        return;
    if (g->kind == CANCEL) {
        bool own = !g->reason && s->offered, sent = own && s->pick_sent;
        s->peer_revision = g->revision;
        new_round(s, s->revision + 1);
        s->offered = own;
        s->pick_sent = sent;
        /* If our selection was queued but not sent when cancellation crossed
           it, regenerate it with the new local revision. */
        if (own && !sent)
            schedule_offer(s, false, 50);
        return;
    }
    if (g->revision < s->peer_revision)
        return;
    if (g->kind == PICK) {
        s->peer_pick = true;
        memcpy(s->incoming, g->entity, 344);
    } else if (g->kind == CONFIRM && s->pick_sent && s->peer_pick) {
        /* Reserve both messages before setting confirmed. A failed reservation
           leaves the operation unconfirmed, never half-committed. */
        if (s->count > QUEUE_MAX - 2) {
            fail(s, LS_SESSION_QUEUE_FULL);
            return;
        }
        s->confirmed = true;
        schedule_revision(s, 2, 50);
        schedule_revision(s, 4, 1500);
    }
}
static void delivery(void* context, uint8_t protocol, const uint8_t* data, size_t size) {
    LsSession* s = context;
    if (size >= 2 && data[0] == 20 && data[1] == 0) {
        uint32_t hash;
        if (!ls_game_identity(data, size, &hash))
            return;
        unsigned i = protocol - 10;
        if ((s->identity[i] && s->peer_hash[i] != hash) ||
            (s->identity[1 - i] && s->peer_hash[1 - i] != hash))
            return;
        s->identity[i] = true;
        s->peer_hash[i] = hash;
    } else if (protocol == 10 && s->identity[0]) {
        Game g;
        if (parse_game(data, size, &g))
            apply_game(s, &g);
    }
}
LsSession* ls_session_alloc(const LsSessionConfig* config, const uint8_t* recipe, size_t size,
                            uint32_t now, LsSessionOutput output, void* context) {
    LsRecipe parsed;
    uint8_t validation[512];
    if (!config || !output || !ls_recipe_parse(recipe, size, &parsed) ||
        !ls_connect_net_status(&config->host, validation, sizeof(validation)))
        return NULL;
    LsSession* s = ls_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->config = *config;
    memcpy(s->recipe_bytes, recipe, size);
    if (!ls_recipe_parse(s->recipe_bytes, size, &s->recipe)) {
        free(s);
        return NULL;
    }
    s->pia = ls_pia_alloc(config->ssid, config->first_nonce);
    s->streams[0] = ls_stream_alloc(10, true);
    s->streams[1] = ls_stream_alloc(11, true);
    if (!s->pia || !s->streams[0] || !s->streams[1]) {
        ls_session_free(s);
        return NULL;
    }
    s->config.host.network_id = ls_pia_network_id(s->pia);
    s->output = output;
    s->context = context;
    s->now = s->start = s->next_net = now;
    s->packet[0] = s->packet[1] = 1;
    return s;
}
void ls_session_free(LsSession* s) {
    if (!s)
        return;
    ls_stream_free(s->streams[0]);
    ls_stream_free(s->streams[1]);
    ls_pia_free(s->pia);
    memset(s, 0, sizeof(*s));
    free(s);
}
bool ls_session_receive(LsSession* s, const uint8_t* data, size_t size, uint32_t source_ip,
                        uint32_t now) {
    if (!s || source_ip != s->config.host.guest_ip || (s->stopping && ls_session_safe_to_stop(s)))
        return false;
    s->now = now;
    const LsPiaFrame* f = ls_pia_decode(s->pia, data, size, source_ip);
    if (!f ||
        (f->footer_size && (f->footer_size != 2 || f->recipient != s->config.host.variable)) ||
        (f->destination > 1 && f->destination != s->config.host.variable))
        return false;
    if (s->joined && f->source != s->join.source_var && f->source != 0)
        return false;
    for (size_t i = 0; i < f->count; ++i) {
        const LsPiaMessage* m = f->messages + i;
        if (m->protocol == 1 && f->destination == 0) {
            if (s->net_sent && ls_connect_net_ack(m->data, m->size, 0x12, 2))
                s->net_acked = true;
            if (s->property_sent && ls_connect_net_ack(m->data, m->size, 0x51, 1))
                s->property_acked = true;
        } else if (m->protocol == 13 && f->source > 1) {
            connection(s, f->source, m->data, m->size);
        } else if (s->joined && f->source == s->join.source_var) {
            if (m->protocol == 3 && f->destination != 0) {
                uint8_t reply[21];
                size_t n = ls_connect_rtt_response(m->data, m->size,
                                                   (uint64_t)(uint32_t)(now - s->start) * 1000,
                                                   f->source, reply, sizeof(reply));
                if (n)
                    send(s, 3, reply, n, 1, false, -1);
            } else if ((m->protocol == 10 && f->destination == s->config.host.variable) ||
                       (m->protocol == 11 && f->destination == 1)) {
                LsStream* stream = s->streams[m->protocol - 10];
                int result = ls_stream_receive(stream, m->data, m->size, now, delivery, s);
                if (result < 0)
                    fail(s, LS_SESSION_FRAGMENT_ERROR);
                if (result == 2) {
                    size_t n = ls_stream_ack(stream, s->wire, sizeof(s->wire));
                    send(s, m->protocol, s->wire, n, m->protocol == 11 ? 1 : s->join.source_var,
                         false, 0x40);
                }
            }
        }
    }
    return true;
}
void ls_session_tick(LsSession* s, uint32_t now) {
    if (!s || (s->stopping && ls_session_safe_to_stop(s)))
        return;
    s->now = now;
    if (!s->net_acked && due(now, s->next_net)) {
        net_status(s);
        s->next_net = now + 456;
    }
    if (!s->joined)
        return;
    if (!s->update_acked[1] && due(now, s->next_update)) {
        update(s, s->update_acked[0] ? 1 : 0);
        s->next_update = now + 1000;
    }
    if (!s->property_acked && due(now, s->next_property)) {
        property(s);
        s->next_property = now + 500;
    }
    if (due(now, s->next_rtt)) {
        uint8_t body[21];
        uint64_t elapsed = (uint32_t)(now - s->start);
        size_t n = ls_connect_rtt_request(elapsed * 19200, elapsed * 1000, s->config.host.variable,
                                          body, sizeof(body));
        send(s, 3, body, n, 1, false, -1);
        s->next_rtt = now + 310;
    }
    for (unsigned i = 0; i < 2; ++i) {
        for (unsigned retry = 0; retry < 4; ++retry) {
            size_t n = ls_stream_retransmit(s->streams[i], now, s->wire, sizeof(s->wire));
            if (!n)
                break;
            send(s, 10 + i, s->wire, n, i ? 1 : s->join.source_var, false, 0x20);
        }
    }
    while (s->count && due(now, s->queue[s->head].due)) {
        Scheduled* q = s->queue + s->head;
        size_t n = ls_stream_send(s->streams[q->protocol - 10], q->data, q->size, q->opening, now,
                                  s->wire, sizeof(s->wire));
        if (!n)
            break; /* Keep the queued message under reliable backpressure. */
        if (send(s, q->protocol, s->wire, n, q->protocol == 11 ? 1 : s->join.source_var, false,
                 -1) &&
            q->kind == LOCAL_PICK)
            s->pick_sent = true;
        s->head = (s->head + 1) % QUEUE_MAX;
        --s->count;
    }
    if (!s->error && s->barriers == 15 && !s->complete && !s->count && !outstanding(s)) {
        s->complete = true;
        ++s->completed;
        memcpy(s->last_received, s->incoming, sizeof(s->last_received));
        Game pending[3];
        unsigned count = s->pending_count;
        memcpy(pending, s->pending, count * sizeof(Game));
        s->pending_count = 0;
        if (!s->stopping)
            for (unsigned i = 0; i < count; ++i)
                apply_game(s, pending + i);
    }
}
static bool mutable(const LsSession* s) {
    return s && s->update_acked[1] && !s->error && !s->confirmed && !s->stopping;
}
bool ls_session_preview(LsSession* s, uint32_t now) {
    if (!mutable(s) || s->offered)
        return false;
    s->now = now;
    return schedule_offer(s, true, 10);
}
bool ls_session_offer(LsSession* s, uint32_t now) {
    if (!mutable(s) || s->offered)
        return false;
    s->now = now;
    if (!schedule_offer(s, false, 50))
        return false;
    s->offered = true;
    return true;
}
bool ls_session_cancel(LsSession* s, int32_t reason, uint32_t now) {
    if (!mutable(s) || !s->offered)
        return false;
    s->now = now;
    bool peer = !reason && s->peer_pick;
    ++s->peer_revision;
    new_round(s, s->revision + 1);
    s->peer_pick = peer;
    uint8_t data[12];
    size_t n = ls_game_cancel(s->revision, reason, data, sizeof(data));
    return schedule(s, 10, data, n, false, 10, NORMAL);
}
void ls_session_stop(LsSession* s) {
    if (s)
        s->stopping = true;
}
void ls_session_status(const LsSession* s, LsSessionStatus* out) {
    if (!s || !out)
        return;
    *out = (LsSessionStatus){.completed = s->completed,
                             .error = s->error,
                             .scheduled = s->count,
                             .outstanding = outstanding(s),
                             .dropped = s->dropped,
                             .revision = s->revision,
                             .peer_revision = s->peer_revision,
                             .barriers = s->barriers};
    out->flags =
        (s->joined ? LS_SESSION_JOINED : 0) | (s->update_acked[1] ? LS_SESSION_READY : 0) |
        (s->offered ? LS_SESSION_OFFERED : 0) | (s->confirmed ? LS_SESSION_CONFIRMED : 0) |
        (s->complete ? LS_SESSION_COMPLETE : 0) | (s->stopping ? LS_SESSION_STOP_REQUESTED : 0) |
        (ls_session_safe_to_stop(s) ? LS_SESSION_SAFE : 0) |
        (s->peer_pick ? LS_SESSION_PEER_PICK : 0) | (s->identity[0] ? LS_SESSION_PEER_IDENTITY : 0);
}
bool ls_session_received(const LsSession* s, uint8_t entity[344]) {
    if (!s || !entity || !s->completed)
        return false;
    memcpy(entity, s->last_received, 344);
    return true;
}
