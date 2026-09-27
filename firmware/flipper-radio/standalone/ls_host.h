#pragma once
#include "ls_ldn.h"
#include "ls_session.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Owns discovery, authentication, IP, encrypted session and semantic trading.
 * It never calls the radio driver or a controller transport. One worker owns
 * all calls; output callbacks consume/copy bytes synchronously. */
typedef struct LsHost LsHost;
typedef bool (*LsHostOutput)(void* context, const uint8_t* frame, size_t size);
typedef struct {
    LsLdn network;
    uint8_t data_key[16], join_random[4];
    uint16_t variable;
    uint64_t first_nonce;
} LsHostConfig;
enum { LS_HOST_MEMORY_ERROR = 0x2001, LS_HOST_PEER_LEFT = 0x2002 };
LsHost* ls_host_alloc(const LsHostConfig* config, const uint8_t* recipe, size_t size,
                      uint32_t now_ms, LsHostOutput raw, LsHostOutput ethernet, void* context);
void ls_host_free(LsHost* host);
void ls_host_tick(LsHost* host, uint32_t now_ms);
bool ls_host_ethernet(LsHost* host, const uint8_t* frame, size_t size, uint32_t now_ms);
bool ls_host_monitor(LsHost* host, const uint8_t* frame, size_t size, uint32_t now_ms);
bool ls_host_preview(LsHost* host, uint32_t now_ms);
bool ls_host_offer(LsHost* host, uint32_t now_ms);
bool ls_host_cancel(LsHost* host, uint32_t now_ms);
void ls_host_stop(LsHost* host);
void ls_host_peer_left(LsHost* host, const uint8_t mac[6]);
/* Only true after a stop request AND acknowledged save replies (if confirmed). */
bool ls_host_finished(const LsHost* host);
/* Compact GPIO status format from flipper/common/ls_control.h. */
void ls_host_status(const LsHost* host, uint8_t out[44]);
/* Worker diagnostics; false until authentication has created the session. */
bool ls_host_session_status(const LsHost* host, LsSessionStatus* status);
