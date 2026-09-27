#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_RELIABLE_WINDOW 128
#define LS_RELIABLE_PAYLOAD_MAX 1300
#define LS_RELIABLE_BYTES_MAX 65536
typedef struct LsReliable LsReliable;

/* One link per Pia reliable protocol. All calls belong to the engine worker. */
LsReliable* ls_reliable_alloc(uint16_t start);
void ls_reliable_free(LsReliable* link);
bool ls_reliable_queue(LsReliable* link, const uint8_t* data, size_t size, uint8_t flags,
                       uint32_t now_ms, uint16_t* sequence);
uint16_t ls_reliable_next(const LsReliable* link);
uint16_t ls_reliable_low(const LsReliable* link);
size_t ls_reliable_outstanding(const LsReliable* link);
void ls_reliable_ack(LsReliable* link, uint16_t next_expected, const uint8_t mask[16],
                     uint32_t now_ms);
uint32_t ls_reliable_rto(const LsReliable* link);
bool ls_reliable_due(LsReliable* link, uint32_t now_ms, uint16_t* sequence, const uint8_t** data,
                     size_t* size, uint8_t* flags);
/* Records/deduplicates a complete received frame for selective ACKs. Application
 * delivery/reassembly is separate. Frames >128 ahead are not acknowledged. */
bool ls_reliable_received(LsReliable* link, uint16_t sequence);
void ls_reliable_ack_payload(const LsReliable* link, uint8_t out[20]);
