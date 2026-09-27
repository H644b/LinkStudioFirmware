#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_IP_DATAGRAM_MAX 8192
#define LS_IP_FRAME_MAX 1514
typedef struct LsIp LsIp;
typedef struct {
    uint8_t local_mac[6], peer_mac[6];
    uint32_t local_ip, peer_ip; /* numeric network addresses */
} LsIpConfig;
/* Callbacks run on the owning worker and must not reenter receive. Frame/data
 * pointers are borrowed only for the callback. The peer is fixed by LDN auth;
 * received ARP packets cannot replace the station's MAC or IP. */
typedef bool (*LsIpOutput)(void* context, const uint8_t* frame, size_t size);
typedef void (*LsIpDatagram)(void* context, const uint8_t* data, size_t size);
LsIp* ls_ip_alloc(const LsIpConfig* config, LsIpOutput output, LsIpDatagram datagram,
                  void* context);
void ls_ip_free(LsIp* ip);
/* Sends IPv4 UDP from/to port 12345, fragmenting at the Ethernet MTU. */
bool ls_ip_send(LsIp* ip, const uint8_t* data, size_t size);
/* Handles Ethernet ARP or IPv4 from the one seated peer. IPv4 fragments have
 * four fixed slots, a five-second lifetime, and a strict 8-KiB payload limit.
 * Inconsistent overlaps invalidate a slot until its timeout. */
bool ls_ip_receive(LsIp* ip, const uint8_t* frame, size_t size, uint32_t now_ms);
