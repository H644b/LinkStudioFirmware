#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool ls_crypto_sha256(const uint8_t* data, size_t size, uint8_t out[32]);
/* AES-128 CTR, full 16-byte initial counter; input and output may alias. */
bool ls_crypto_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t size,
                   uint8_t* out);
bool ls_crypto_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
/* Pia uses a 12-byte nonce, no AAD and a detached 8-byte tag. */
bool ls_crypto_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t* in,
                           size_t size, uint8_t* out, uint8_t tag[8]);
bool ls_crypto_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t* in,
                           size_t size, const uint8_t tag[8], uint8_t* out);
