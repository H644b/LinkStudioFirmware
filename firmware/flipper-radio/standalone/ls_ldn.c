#include "ls_ldn.h"
#include "ls_crypto.h"
#include <string.h>

static void be16(uint8_t* p, uint16_t n) {
    p[0] = n >> 8;
    p[1] = n;
}
static void be32(uint8_t* p, uint32_t n) {
    be16(p, n >> 16);
    be16(p + 2, n);
}
static uint16_t read16(const uint8_t* p) {
    return (uint16_t)p[0] << 8 | p[1];
}
static bool valid_mac(const uint8_t* mac) {
    static const uint8_t zero[6] = {0};
    return !(mac[0] & 1) && memcmp(mac, zero, 6);
}
static void network_id(const LsLdn* host, uint8_t* out, bool little) {
    static const uint8_t comm_id[] = {1, 0, 0xf4, 0x30, 8, 0xc4, 0x40, 0};
    memset(out, 0, 32);
    for (unsigned i = 0; i < 8; ++i)
        out[i] = comm_id[little ? 7 - i : i];
    out[little ? 10 : 11] = 1;
    memcpy(out + 16, host->ssid, 16);
}
bool ls_ldn_valid_config(const LsLdn* host) {
    if (!host || !valid_mac(host->mac) || !host->subnet || host->subnet > 127 || !host->app_version)
        return false;
    if (host->channel != 1 && host->channel != 6 && host->channel != 11)
        return false;
    for (unsigned i = 0; i < 8; ++i)
        if (host->code[i] < '0' || host->code[i] > '9')
            return false;
    return true;
}
void ls_ldn_application(const LsLdn* host, uint8_t out[112]) {
    static const uint8_t mask[16] = {0x10, 0x68, 0xa7, 0x42, 0xac, 0x3a, 0x87, 0x87,
                                     0xab, 0x60, 0x66, 0xa1, 0x61, 0xf5, 0xd5, 0xe1};
    memset(out, 0, 112);
    be16(out, 92);
    out[2] = 22;
    be16(out + 3, host->app_version);
    memcpy(out + 5, mask, 16);
    for (unsigned i = 0; i < 8; ++i)
        out[5 + i] ^= (uint8_t)host->code[i];
    out[0x15] = 1;
    out[0x16] = host->peer_connected ? 2 : 1;
    be32(out + 0x17, 1);
    out[0x1b] = 1;
    out[0x1c] = ' ';
    memcpy(out + 92, host->code, 8);
    out[108] = 8; /* game data uses little endian */
}
static void participant(uint8_t* out, uint8_t subnet, uint8_t seat, const uint8_t* mac,
                        const char* name, uint16_t version, uint8_t platform) {
    out[0] = 169;
    out[1] = 254;
    out[2] = subnet;
    out[3] = seat;
    memcpy(out + 4, mac, 6);
    out[10] = 1;
    out[11] = platform;
    memcpy(out + 12, name, 32);
    be16(out + 44, version);
}
size_t ls_ldn_advertisement(const LsLdn* host, uint8_t* out, size_t capacity) {
    if (!ls_ldn_valid_config(host) || !out || capacity < LS_LDN_ADVERTISEMENT_SIZE)
        return 0;
    memset(out, 0, LS_LDN_ADVERTISEMENT_SIZE);
    out[0] = 0xd0;
    memset(out + 4, 255, 6);
    memcpy(out + 10, host->mac, 6);
    memset(out + 16, 255, 6);
    static const uint8_t action[] = {0x7f, 0, 0x22, 0xaa, 4, 0, 1, 1, 0, 0, 0, 0};
    memcpy(out + 24, action, 12);
    uint8_t* header = out + 36;
    network_id(host, header, false);
    header[32] = 4;
    header[33] = 2;
    be16(header + 34, 1280);
    be32(header + 36, host->nonce);
    uint8_t* body = out + 108;
    memcpy(body, host->server_random, 16);
    be16(body + 16, 1);
    be16(body + 20, host->channel);
    body[22] = 2;
    body[23] = host->peer_connected ? 2 : 1;
    participant(body + 24, host->subnet, 1, host->mac, host->name, host->app_version, 0);
    if (host->peer_connected)
        participant(body + 80, host->subnet, 2, host->peer_mac, host->peer_name,
                    host->peer_app_version, host->peer_platform);
    be16(body + 474, 112);
    ls_ldn_application(host, body + 476);
    uint8_t sha[32], iv[16] = {0};
    if (!ls_crypto_sha256(header, 40 + 32 + 1280, sha))
        return 0;
    memcpy(out + 76, sha, 32);
    be32(iv, host->nonce);
    if (!ls_crypto_ctr(host->advertise_key, iv, out + 76, 1312, out + 76))
        return 0;
    return LS_LDN_ADVERTISEMENT_SIZE;
}
size_t ls_ldn_authenticate(LsLdn* host, const uint8_t mac[6], const uint8_t* data, size_t size,
                           uint8_t* out, size_t capacity) {
    static const uint8_t prefix[] = {0, 0x22, 0xaa, 1, 2, 0};
    if (!ls_ldn_valid_config(host) || !mac || !valid_mac(mac) || !memcmp(mac, host->mac, 6) ||
        !data || size < 78 || !out || capacity < LS_LDN_AUTH_RESPONSE_SIZE ||
        memcmp(data, prefix, 6))
        return 0;
    const uint8_t* header = data + 6;
    uint16_t count = header[1] | (uint16_t)header[4] << 8;
    uint8_t status = 0;
    uint8_t expected[32];
    network_id(host, expected, true);
    if (header[0] < 2 || header[0] > 4)
        status = 4;
    else if (header[2] || header[3] || header[5] || count != size - 78 ||
             (header[0] == 2 ? count != 64 : (count != 100 && count != 868)) ||
             memcmp(header + 8, expected, 32) || memcmp(header + 40, host->server_random, 16))
        status = 2;
    else if (host->peer_connected && memcmp(host->peer_mac, mac, 6))
        status = 1;
    if (!status && !host->peer_connected) {
        host->peer_connected = true;
        memcpy(host->peer_mac, mac, 6);
        memcpy(host->peer_name, data + 78, 32);
        host->peer_app_version = read16(data + 110);
        host->peer_platform = data[112];
        ++host->nonce;
    }
    /* Match the reference host's advertised version, even for version-2 requests. */
    memset(out, 0, LS_LDN_AUTH_RESPONSE_SIZE);
    memcpy(out, prefix, 6);
    out[6] = 4;
    out[7] = 132;
    out[8] = status;
    out[9] = 1;
    network_id(host, out + 14, true);
    memcpy(out + 46, host->server_random, 16);
    memcpy(out + 62, header + 56, 16);
    return LS_LDN_AUTH_RESPONSE_SIZE;
}
bool ls_ldn_leave(LsLdn* host, const uint8_t mac[6]) {
    if (!host || !mac || !host->peer_connected || memcmp(host->peer_mac, mac, 6))
        return false;
    host->peer_connected = false;
    memset(host->peer_mac, 0, 6);
    memset(host->peer_name, 0, 32);
    host->peer_app_version = 0;
    host->peer_platform = 0;
    ++host->nonce;
    return true;
}
