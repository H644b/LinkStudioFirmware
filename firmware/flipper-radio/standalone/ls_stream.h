#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_STREAM_FRAME_MAX 1308
#define LS_STREAM_MESSAGE_MAX 8192
typedef struct LsStream LsStream;
/* Called synchronously, once per complete in-order application message. Data
 * belongs to the stream until this callback returns. No recursive receive calls. */
typedef void (*LsStreamDelivery)(void* context, uint8_t protocol, const uint8_t* data, size_t size);
LsStream* ls_stream_alloc(uint8_t protocol, bool host);
void ls_stream_free(LsStream* stream);
/* Zero means rejected/backpressure: no sequence or opening is consumed. */
size_t ls_stream_send(LsStream* stream, const uint8_t* data, size_t size, bool opening,
                      uint32_t now_ms, uint8_t* out, size_t capacity);
size_t ls_stream_retransmit(LsStream* stream, uint32_t now_ms, uint8_t* out, size_t capacity);
size_t ls_stream_ack(const LsStream* stream, uint8_t* out, size_t capacity);
size_t ls_stream_outstanding(const LsStream* stream);
/* 0: rejected or no receive capacity, 1: control ACK, 2: data (ACK owed),
 * -1: invalid application fragment chain. No bytes are delivered out of order.
 * The header's window base is the peer's send window, never an ACK of ours. */
int ls_stream_receive(LsStream* stream, const uint8_t* frame, size_t size, uint32_t now_ms,
                      LsStreamDelivery deliver, void* context);
