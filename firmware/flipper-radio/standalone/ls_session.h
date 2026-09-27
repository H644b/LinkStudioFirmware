#pragma once
#include "ls_connect.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Socketless, single-worker host for one already authenticated LDN guest.
 * Recipe/config bytes are copied. Callbacks must not reenter the session.
 * The output callback consumes/copies a UDP datagram before returning. False
 * means local transport loss; reliable messages remain queued for retry. */
typedef struct LsSession LsSession;
typedef bool (*LsSessionOutput)(void* context, const uint8_t* data, size_t size,
                                uint32_t destination_ip);
typedef struct {
    LsConnectHost host;
    uint8_t ssid[16], random[4], application[112];
    uint64_t first_nonce;
} LsSessionConfig;
enum {
    LS_SESSION_JOINED = 1,
    LS_SESSION_READY = 2,
    LS_SESSION_OFFERED = 4,
    LS_SESSION_CONFIRMED = 8,
    LS_SESSION_COMPLETE = 16,
    LS_SESSION_STOP_REQUESTED = 32,
    LS_SESSION_SAFE = 64,
    LS_SESSION_PEER_PICK = 128,
    LS_SESSION_PEER_IDENTITY = 256,
};
enum {
    LS_SESSION_OK = 0,
    LS_SESSION_QUEUE_FULL = 1,
    LS_SESSION_FRAGMENT_ERROR = 2,
    LS_SESSION_ENCODE_ERROR = 3
};
typedef struct {
    uint32_t flags, completed, error, scheduled, outstanding, dropped;
    uint16_t revision, peer_revision;
    uint8_t barriers;
} LsSessionStatus;
LsSession* ls_session_alloc(const LsSessionConfig* config, const uint8_t* recipe, size_t size,
                            uint32_t now_ms, LsSessionOutput output, void* context);
void ls_session_free(LsSession* session);
void ls_session_tick(LsSession* session, uint32_t now_ms);
/* Source IP, Pia recipient and seated station identity are checked before use. */
bool ls_session_receive(LsSession* session, const uint8_t* datagram, size_t size,
                        uint32_t source_ip, uint32_t now_ms);
bool ls_session_preview(LsSession* session, uint32_t now_ms);
bool ls_session_offer(LsSession* session, uint32_t now_ms);
bool ls_session_cancel(LsSession* session, int32_t reason, uint32_t now_ms);
/* This is a request, never permission to immediately turn the radio off. */
void ls_session_stop(LsSession* session);
bool ls_session_safe_to_stop(const LsSession* session);
void ls_session_status(const LsSession* session, LsSessionStatus* status);
/* The last completed incoming record is retained across the next trade round. */
bool ls_session_received(const LsSession* session, uint8_t entity[344]);
