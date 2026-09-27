#include "ls_wifi_frame.h"
#include "ls_crypto.h"
#include <string.h>

size_t ls_wifi_frame_decode(const uint8_t* f, size_t size, const uint8_t local[6],
                            const uint8_t peer[6], const uint8_t key[16], bool driver_plaintext,
                            uint8_t* out, size_t capacity) {
    static const uint8_t snap[6] = {0xaa, 0xaa, 3, 0, 0, 0};
    static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    if (!f || !local || !peer || !key || !out || size < 32 || size > 1600 ||
        (f[0] != 0x08 && f[0] != 0x88) || (f[1] & 0x87) || (f[22] & 15) ||
        memcmp(f + 10, peer, 6) || memcmp(f + 16, local, 6) ||
        (memcmp(f + 4, local, 6) && memcmp(f + 4, broadcast, 6)))
        return 0;
    bool qos = f[0] == 0x88;
    if (qos && (size < 34 || (f[24] & 0x80)))
        return 0;
    uint8_t tid = qos ? f[24] & 15 : 0;
    size_t at = qos ? 26 : 24;
    const uint8_t* payload = f + at;
    size_t length = size - at;
    uint8_t plain[1508];
    if (f[1] & 0x40) {
        if (driver_plaintext && length >= 8 && !memcmp(payload, snap, 6)) {
            /* Some driver paths strip both the CCMP header and MIC. */
        } else {
            if (length < 24 || payload[2] || (payload[3] & 0x3f) != 0x20)
                return 0;
            const uint8_t* ccmp = payload;
            payload += 8;
            length -= 16; /* header and detached MIC */
            if (length > sizeof(plain))
                return 0;
            if (!(driver_plaintext && !memcmp(payload, snap, 6))) {
                uint8_t nonce[13] = {tid}, aad[24] = {0};
                memcpy(nonce + 1, peer, 6);
                nonce[7] = ccmp[7];
                nonce[8] = ccmp[6];
                nonce[9] = ccmp[5];
                nonce[10] = ccmp[4];
                nonce[11] = ccmp[1];
                nonce[12] = ccmp[0];
                aad[0] = f[0];
                aad[1] = 0x40;
                memcpy(aad + 2, f + 4, 18);
                if (qos)
                    aad[22] = tid;
                if (!ls_crypto_ccm_decrypt(key, nonce, aad, qos ? 24 : 22, payload, length,
                                           payload + length, plain))
                    return 0;
                payload = plain;
            }
        }
    }
    if (length < 8 || length > 1508 || length + 6 > capacity || memcmp(payload, snap, 6))
        return 0;
    memcpy(out, f + 4, 12);
    memcpy(out + 12, payload + 6, length - 6);
    return length + 6;
}
