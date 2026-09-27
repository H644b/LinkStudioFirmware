#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pia 6.39 Net/Session/RTT layouts used by Z-A. These structures are never
 * copied to the wire. IPv4 values are numeric network addresses (0xa9fe2f01).
 * Parsing is bounded to four local players and 32 protocol declarations. */
#define LS_CONNECT_PROTOCOLS_MAX 32
#define LS_CONNECT_PLAYERS_MAX 4
#define LS_CONNECT_NAME_MAX 40
#define LS_CONNECT_MESSAGE_MAX 512
typedef struct {
    uint8_t id[16], encoding, name_size, name[LS_CONNECT_NAME_MAX];
} LsConnectPlayer;
typedef struct {
    uint8_t protocols[LS_CONNECT_PROTOCOLS_MAX][2], protocol_count;
    uint8_t app_version[2], random[4], source_constant[8];
    uint16_t source_var;
    uint8_t nat_mapping, private_ipv6, token[32], destination_constant[8];
    uint16_t destination_var;
    uint8_t player_count, participant_count;
    uint32_t ip;
    uint16_t port;
    LsConnectPlayer players[LS_CONNECT_PLAYERS_MAX];
} LsConnectJoin;
typedef struct {
    uint8_t mac[6], guest_mac[6];
    uint16_t variable;
    uint32_t ip, guest_ip, network_id;
} LsConnectHost;

void ls_connect_constant_id(const uint8_t mac[6], uint8_t out[8]);
bool ls_connect_parse_join(const uint8_t* data, size_t size, LsConnectJoin* join);
/* Bind a parsed join to the already seated LDN guest, including the outer Pia
 * source variable ID. This does not certify application-version compatibility. */
bool ls_connect_join_matches(const LsConnectHost* host, const LsConnectJoin* join,
                             uint16_t source_var);
size_t ls_connect_net_status(const LsConnectHost* host, uint8_t* out, size_t capacity);
size_t ls_connect_net_property(const LsConnectHost* host, const uint8_t app[112], uint8_t* out,
                               size_t capacity);
/* Net ACKs must echo the corresponding sequence, not merely the command byte. */
bool ls_connect_net_ack(const uint8_t* data, size_t size, uint8_t kind, uint32_t sequence);
size_t ls_connect_join_response(const LsConnectHost* host, const LsConnectJoin* join,
                                const uint8_t random[4], uint8_t* out, size_t capacity);
size_t ls_connect_session_update(const LsConnectHost* host, const LsConnectJoin* join,
                                 uint16_t update_sequence, uint8_t* out, size_t capacity);
/* Z-A ACKs are 15 bytes: constant ID, update u32 at +9, session sequence u16
 * at +13 (1 for this two-station session). A controller must separately check
 * that the acknowledged update was actually sent. */
bool ls_connect_update_ack(const uint8_t* data, size_t size, const uint8_t guest_constant[8],
                           uint32_t* sequence);
size_t ls_connect_rtt_request(uint64_t tick, uint64_t micros, uint16_t variable, uint8_t* out,
                              size_t capacity);
size_t ls_connect_rtt_response(const uint8_t* request, size_t size, uint64_t micros,
                               uint16_t requester, uint8_t* out, size_t capacity);
