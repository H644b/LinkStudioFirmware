#include "ls_crypto.h"
#include "psa/crypto.h"
#include <stdlib.h>
#include <string.h>

bool ls_crypto_sha256(const uint8_t* data, size_t size, uint8_t out[32]) {
    size_t count = 0;
    return psa_crypto_init() == PSA_SUCCESS &&
           psa_hash_compute(PSA_ALG_SHA_256, data, size, out, 32, &count) == PSA_SUCCESS &&
           count == 32;
}
bool ls_crypto_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t size,
                   uint8_t* out) {
    if (psa_crypto_init() != PSA_SUCCESS)
        return false;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CTR);
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_import_key(&attributes, key, 16, &id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS)
        return false;
    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    size_t count = 0, tail = 0;
    status = psa_cipher_encrypt_setup(&operation, id, PSA_ALG_CTR);
    if (status == PSA_SUCCESS)
        status = psa_cipher_set_iv(&operation, iv, 16);
    if (status == PSA_SUCCESS)
        status = psa_cipher_update(&operation, in, size, out, size, &count);
    if (status == PSA_SUCCESS)
        status = psa_cipher_finish(&operation, out + count, size - count, &tail);
    psa_cipher_abort(&operation);
    psa_destroy_key(id);
    return status == PSA_SUCCESS && count + tail == size;
}

static psa_status_t import_aes(const uint8_t key[16], psa_algorithm_t algorithm,
                               psa_key_usage_t usage, mbedtls_svc_key_id_t* id) {
    if (psa_crypto_init() != PSA_SUCCESS)
        return PSA_ERROR_BAD_STATE;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, algorithm);
    psa_status_t result = psa_import_key(&attributes, key, 16, id);
    psa_reset_key_attributes(&attributes);
    return result;
}
bool ls_crypto_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    if (import_aes(key, PSA_ALG_ECB_NO_PADDING, PSA_KEY_USAGE_ENCRYPT, &id) != PSA_SUCCESS)
        return false;
    size_t count = 0;
    psa_status_t result = psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, in, 16, out, 16, &count);
    psa_destroy_key(id);
    return result == PSA_SUCCESS && count == 16;
}
bool ls_crypto_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t* in,
                           size_t size, uint8_t* out, uint8_t tag[8]) {
    if (size > 8192)
        return false;
    uint8_t* combined = malloc(size + 8);
    if (!combined)
        return false;
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_algorithm_t algorithm = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, 8);
    psa_status_t result = import_aes(key, algorithm, PSA_KEY_USAGE_ENCRYPT, &id);
    size_t count = 0;
    if (result == PSA_SUCCESS)
        result = psa_aead_encrypt(id, algorithm, nonce, 12, NULL, 0, in, size, combined, size + 8,
                                  &count);
    bool ok = result == PSA_SUCCESS && count == size + 8;
    if (ok) {
        memcpy(out, combined, size);
        memcpy(tag, combined + size, 8);
    }
    psa_destroy_key(id);
    free(combined);
    return ok;
}
bool ls_crypto_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t* in,
                           size_t size, const uint8_t tag[8], uint8_t* out) {
    if (size > 8192)
        return false;
    uint8_t* combined = malloc(size + 8);
    if (!combined)
        return false;
    memcpy(combined, in, size);
    memcpy(combined + size, tag, 8);
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_algorithm_t algorithm = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, 8);
    psa_status_t result = import_aes(key, algorithm, PSA_KEY_USAGE_DECRYPT, &id);
    size_t count = 0;
    if (result == PSA_SUCCESS)
        result = psa_aead_decrypt(id, algorithm, nonce, 12, NULL, 0, combined, size + 8, out, size,
                                  &count);
    bool ok = result == PSA_SUCCESS && count == size;
    if (!ok)
        memset(out, 0, size);
    psa_destroy_key(id);
    free(combined);
    return ok;
}
