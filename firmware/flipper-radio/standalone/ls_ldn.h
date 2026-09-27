#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Protocol-1 retail Z-A host, one Switch guest. Wire encodings never use this
 * native structure; all endian conversions and size checks are explicit. */
typedef struct {
    uint8_t ssid[16], advertise_key[16], server_random[16], mac[6];
    uint8_t subnet, channel;
    uint16_t app_version;
    char code[8], name[32];
    uint32_t nonce;
    bool peer_connected;
    uint8_t peer_mac[6];
    char peer_name[32];
    uint16_t peer_app_version;
    uint8_t peer_platform;
} LsLdn;

#define LS_LDN_ADVERTISEMENT_SIZE 1388
#define LS_LDN_AUTH_RESPONSE_SIZE 210
bool ls_ldn_valid_config(const LsLdn* host);
void ls_ldn_application(const LsLdn* host, uint8_t out[112]);
size_t ls_ldn_advertisement(const LsLdn* host, uint8_t* out, size_t capacity);
/* Input/output are EtherType 0x88b7 payloads, without the Ethernet header.
 * Repeated authentication from a seated MAC retains its seat/IP/nonce. */
size_t ls_ldn_authenticate(LsLdn* host, const uint8_t mac[6], const uint8_t* data, size_t size,
                           uint8_t* out, size_t capacity);
bool ls_ldn_leave(LsLdn* host, const uint8_t mac[6]);
