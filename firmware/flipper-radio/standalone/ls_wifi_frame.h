#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decode direct BSS data broadcasts the ESP soft AP does not deliver as
 * Ethernet. Inputs exclude the FCS. Only the seated peer and current BSSID are
 * accepted. Do not use driver_plaintext for frames from an untrusted host;
 * it accommodates the ESP receive path retaining CCMP headers after decryption. */
size_t ls_wifi_frame_decode(const uint8_t* frame, size_t size, const uint8_t local[6],
                            const uint8_t peer[6], const uint8_t key[16], bool driver_plaintext,
                            uint8_t* ethernet, size_t capacity);
