#include "ls_host.h"
#include "ls_alloc.h"
#include "ls_control.h"
#include "ls_ip.h"
#include "ls_recipe.h"
#include "ls_wifi_frame.h"
#include <string.h>

struct LsHost {
    LsHostConfig config;
    uint8_t recipe[LS_RECIPE_MAX], advertisement[LS_LDN_ADVERTISEMENT_SIZE];
    size_t recipe_size;
    uint32_t now, next_advertisement, error;
    bool stopping, advertisement_valid;
    LsSession* session;
    LsIp* ip;
    LsHostOutput raw, ethernet;
    void* context;
};
static bool safe(const LsHost* h) { return !h->session || ls_session_safe_to_stop(h->session); }
static bool ether_out(void* context, const uint8_t* frame, size_t size) {
    LsHost* h = context;
    return h->ethernet(h->context, frame, size);
}
static bool session_out(void* context, const uint8_t* data, size_t size, uint32_t destination) {
    LsHost* h = context;
    (void)destination; /* The session's only destination is the seated IP. */
    return ls_ip_send(h->ip, data, size);
}
static void datagram(void* context, const uint8_t* data, size_t size) {
    LsHost* h = context;
    uint32_t guest = 0xa9fe0002 | (uint32_t)h->config.network.subnet << 8;
    ls_session_receive(h->session, data, size, guest, h->now);
}
LsHost* ls_host_alloc(const LsHostConfig* c, const uint8_t* recipe, size_t size, uint32_t now,
                      LsHostOutput raw, LsHostOutput ethernet, void* context) {
    LsRecipe parsed;
    if (!c || !raw || !ethernet || !ls_ldn_valid_config(&c->network) || c->network.peer_connected ||
        c->variable <= 1 || !ls_recipe_parse(recipe, size, &parsed))
        return NULL;
    LsHost* h = ls_calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->config = *c;
    memcpy(h->recipe, recipe, size);
    h->recipe_size = size;
    memcpy(h->config.network.name, parsed.trainer_name, sizeof(h->config.network.name));
    h->now = h->next_advertisement = now;
    h->raw = raw;
    h->ethernet = ethernet;
    h->context = context;
    if (!ls_ldn_advertisement(&h->config.network, h->advertisement, sizeof(h->advertisement))) {
        ls_host_free(h);
        return NULL;
    }
    h->advertisement_valid = true;
    return h;
}
void ls_host_free(LsHost* h) {
    if (!h)
        return;
    ls_session_free(h->session);
    ls_ip_free(h->ip);
    memset(h, 0, sizeof(*h));
    free(h);
}
static bool seat(LsHost* h) {
    const LsLdn* ldn = &h->config.network;
    LsIpConfig ip = {.local_ip = 0xa9fe0001 | (uint32_t)ldn->subnet << 8,
                     .peer_ip = 0xa9fe0002 | (uint32_t)ldn->subnet << 8};
    memcpy(ip.local_mac, ldn->mac, 6);
    memcpy(ip.peer_mac, ldn->peer_mac, 6);
    h->ip = ls_ip_alloc(&ip, ether_out, datagram, h);
    if (!h->ip)
        return false;
    LsSessionConfig c = {
        .host = {.variable = h->config.variable, .ip = ip.local_ip, .guest_ip = ip.peer_ip},
        .first_nonce = h->config.first_nonce};
    memcpy(c.host.mac, ldn->mac, 6);
    memcpy(c.host.guest_mac, ldn->peer_mac, 6);
    memcpy(c.ssid, ldn->ssid, 16);
    memcpy(c.random, h->config.join_random, 4);
    ls_ldn_application(ldn, c.application);
    h->session = ls_session_alloc(&c, h->recipe, h->recipe_size, h->now, session_out, h);
    if (!h->session) {
        ls_ip_free(h->ip);
        h->ip = NULL;
        return false;
    }
    return true;
}
bool ls_host_ethernet(LsHost* h, const uint8_t* frame, size_t size, uint32_t now) {
    static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    if (!h || !frame || size < 14 || size > 1600 || ls_host_finished(h) ||
        (memcmp(frame, h->config.network.mac, 6) && memcmp(frame, broadcast, 6)))
        return false;
    h->now = now;
    if (frame[12] == 0x88 && frame[13] == 0xb7) {
        if (h->stopping || h->error)
            return false;
        uint8_t answer[14 + LS_LDN_AUTH_RESPONSE_SIZE];
        bool first = !h->config.network.peer_connected;
        size_t n = ls_ldn_authenticate(&h->config.network, frame + 6, frame + 14, size - 14,
                                       answer + 14, sizeof(answer) - 14);
        if (!n)
            return false;
        if (first && h->config.network.peer_connected) {
            if (!seat(h)) {
                h->error = LS_HOST_MEMORY_ERROR;
                h->stopping = true;
                ls_ldn_leave(&h->config.network, frame + 6);
                return false;
            }
            h->advertisement_valid = false;
            h->next_advertisement = now;
        }
        memcpy(answer, frame + 6, 6);
        memcpy(answer + 6, h->config.network.mac, 6);
        answer[12] = 0x88;
        answer[13] = 0xb7;
        return h->ethernet(h->context, answer, n + 14);
    }
    return h->ip && ls_ip_receive(h->ip, frame, size, now);
}
bool ls_host_monitor(LsHost* h, const uint8_t* frame, size_t size, uint32_t now) {
    if (!h || !h->ip || ls_host_finished(h))
        return false;
    uint8_t ethernet[LS_IP_FRAME_MAX];
    size_t n = ls_wifi_frame_decode(frame, size, h->config.network.mac, h->config.network.peer_mac,
                                    h->config.data_key, true, ethernet, sizeof(ethernet));
    return n && ls_host_ethernet(h, ethernet, n, now);
}
void ls_host_tick(LsHost* h, uint32_t now) {
    if (!h || ls_host_finished(h))
        return;
    h->now = now;
    if ((int32_t)(now - h->next_advertisement) >= 0) {
        if (!h->advertisement_valid)
            h->advertisement_valid = ls_ldn_advertisement(&h->config.network, h->advertisement,
                                                          sizeof(h->advertisement)) != 0;
        if (h->advertisement_valid)
            h->raw(h->context, h->advertisement, sizeof(h->advertisement));
        h->next_advertisement = now + 100;
    }
    if (h->session)
        ls_session_tick(h->session, now);
}
bool ls_host_preview(LsHost* h, uint32_t now) {
    return h && !h->stopping && !h->error && ls_session_preview(h->session, now);
}
bool ls_host_offer(LsHost* h, uint32_t now) {
    return h && !h->stopping && !h->error && ls_session_offer(h->session, now);
}
bool ls_host_cancel(LsHost* h, uint32_t now) {
    return h && !h->stopping && !h->error && ls_session_cancel(h->session, 0, now);
}
void ls_host_stop(LsHost* h) {
    if (!h)
        return;
    h->stopping = true;
    ls_session_stop(h->session);
}
void ls_host_peer_left(LsHost* h, const uint8_t mac[6]) {
    if (!h || !mac || !h->config.network.peer_connected ||
        memcmp(mac, h->config.network.peer_mac, 6))
        return;
    if (!safe(h))
        h->error = LS_HOST_PEER_LEFT;
    ls_host_stop(h);
}
bool ls_host_finished(const LsHost* h) { return h && h->stopping && safe(h); }
void ls_host_status(const LsHost* h, uint8_t out[44]) {
    if (!h || !out)
        return;
    LsSessionStatus s = {0};
    if (h->session)
        ls_session_status(h->session, &s);
    uint32_t error = h->error ? h->error : s.error;
    uint8_t state = LsStateHosting;
    if (s.flags & LS_SESSION_READY)
        state = LsStateConnected;
    if (s.flags & LS_SESSION_OFFERED)
        state = LsStateOffered;
    if (s.flags & LS_SESSION_CONFIRMED)
        state = LsStateSaving;
    if (s.flags & LS_SESSION_COMPLETE)
        state = LsStateComplete;
    if (h->stopping)
        state = LsStateStopping;
    if (error)
        state = LsStateError;
    memset(out, 0, 44);
    out[0] = 1;
    out[1] = state;
    out[2] = LsFlagRecipe | (h->config.network.peer_connected ? LsFlagPeer : 0) |
             (safe(h) ? LsFlagSafeToStop : 0) | (h->stopping ? LsFlagStopRequested : 0);
    ls_write32(out + 4, s.completed);
    ls_write32(out + 8, error);
    if (h->config.network.peer_connected) {
        memcpy(out + 12, h->config.network.peer_name, 31);
        out[43] = 0;
    }
}
bool ls_host_session_status(const LsHost* h, LsSessionStatus* status) {
    if (!h || !h->session || !status)
        return false;
    ls_session_status(h->session, status);
    return true;
}
