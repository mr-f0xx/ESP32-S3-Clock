#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>

// Authenticated, versioned vault container. The entire header is AAD; ciphertext
// includes a 16-byte GCM tag. No plaintext, PIN, or derived key is persisted.
namespace VaultCrypto {
constexpr size_t SALT_BYTES = 16, NONCE_BYTES = 12, KEY_BYTES = 32,
                 TAG_BYTES = 16, HEADER_BYTES = 4 + 1 + 4 + SALT_BYTES + NONCE_BYTES,
                 MAX_PLAINTEXT = 49152, MAX_FILE = HEADER_BYTES + MAX_PLAINTEXT + TAG_BYTES;
constexpr uint32_t KDF_ITERATIONS = 600000;
inline void wipe(void *p, size_t n) { mbedtls_platform_zeroize(p, n); }
inline void put32(uint8_t *p, uint32_t v) { for (uint8_t i = 0; i < 4; ++i) p[i] = v >> (8 * i); }
inline uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline bool deriveKey(const char *pin, size_t pinLength, const uint8_t salt[SALT_BYTES],
                      uint32_t iterations, uint8_t key[KEY_BYTES]) {
  if (!pin || pinLength < 10 || pinLength > 64 || iterations < 100000 || iterations > 2000000)
    return false;
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;
  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  int result = mbedtls_md_setup(&context, md, 1);
  if (result == 0)
    result = mbedtls_pkcs5_pbkdf2_hmac(&context, reinterpret_cast<const uint8_t *>(pin),
                                      pinLength, salt, SALT_BYTES, iterations, KEY_BYTES, key);
  mbedtls_md_free(&context);
  return result == 0;
}
inline bool seal(const uint8_t key[KEY_BYTES], const uint8_t salt[SALT_BYTES],
                 const uint8_t nonce[NONCE_BYTES], const uint8_t *plain, size_t plainLength,
                 uint8_t *output, size_t capacity, size_t &outputLength) {
  outputLength = 0;
  if (!key || !salt || !nonce || !plain || !output || plainLength > MAX_PLAINTEXT ||
      capacity < HEADER_BYTES + plainLength + TAG_BYTES) return false;
  memcpy(output, "CKV1", 4); output[4] = 1; put32(output + 5, KDF_ITERATIONS);
  memcpy(output + 9, salt, SALT_BYTES); memcpy(output + 25, nonce, NONCE_BYTES);
  mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
  int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (result == 0)
    result = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plainLength,
      nonce, NONCE_BYTES, output, HEADER_BYTES, plain, output + HEADER_BYTES,
      TAG_BYTES, output + HEADER_BYTES + plainLength);
  mbedtls_gcm_free(&gcm);
  if (result != 0) { wipe(output, HEADER_BYTES + plainLength + TAG_BYTES); return false; }
  outputLength = HEADER_BYTES + plainLength + TAG_BYTES;
  return true;
}
inline bool open(const char *pin, size_t pinLength, const uint8_t *file, size_t fileLength,
                 uint8_t *plain, size_t capacity, size_t &plainLength,
                 uint8_t key[KEY_BYTES], uint8_t salt[SALT_BYTES]) {
  plainLength = 0;
  if (!file || fileLength < HEADER_BYTES + TAG_BYTES || fileLength > MAX_FILE ||
      memcmp(file, "CKV1", 4) || file[4] != 1 || !plain) return false;
  const uint32_t iterations = get32(file + 5);
  const size_t encrypted = fileLength - HEADER_BYTES;
  if (encrypted < TAG_BYTES || encrypted - TAG_BYTES > capacity) return false;
  memcpy(salt, file + 9, SALT_BYTES);
  if (!deriveKey(pin, pinLength, salt, iterations, key)) return false;
  mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
  int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (result == 0)
    result = mbedtls_gcm_auth_decrypt(&gcm, encrypted - TAG_BYTES, file + 25, NONCE_BYTES,
      file, HEADER_BYTES, file + fileLength - TAG_BYTES, TAG_BYTES,
      file + HEADER_BYTES, plain);
  mbedtls_gcm_free(&gcm);
  if (result != 0) { wipe(key, KEY_BYTES); wipe(plain, capacity); return false; }
  plainLength = encrypted - TAG_BYTES;
  return true;
}
}
