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
constexpr size_t VAULT_SALT_BYTES = 16;
constexpr size_t VAULT_NONCE_BYTES = 12;
constexpr size_t VAULT_KEY_BYTES = 32;
constexpr size_t VAULT_TAG_BYTES = 16;
constexpr size_t VAULT_HEADER_BYTES = 37;
constexpr size_t VAULT_MAX_PLAINTEXT = 49152;
constexpr size_t VAULT_MAX_FILE = VAULT_HEADER_BYTES + VAULT_MAX_PLAINTEXT + VAULT_TAG_BYTES;
static_assert(VAULT_HEADER_BYTES == 4 + 1 + 4 + VAULT_SALT_BYTES + VAULT_NONCE_BYTES, "vault header layout");
constexpr uint32_t VAULT_KDF_ITERATIONS = 600000;
inline void wipe(void *p, size_t n) { mbedtls_platform_zeroize(p, n); }
inline void put32(uint8_t *p, uint32_t v) { for (uint8_t i = 0; i < 4; ++i) p[i] = v >> (8 * i); }
inline uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline bool deriveKey(const char *pin, size_t pinLength, const uint8_t *salt,
               uint32_t iterations, uint8_t *key) {
  if (!pin || !salt || !key || pinLength < 10 || pinLength > 32 || iterations == 0) return false;
  // ESP-IDF's mbedTLS build exposes the context-free _ext API; the legacy
  // mbedtls_pkcs5_pbkdf2_hmac() symbol is not available there.
  int result = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256, reinterpret_cast<const uint8_t *>(pin), pinLength,
      salt, VAULT_SALT_BYTES, iterations, VAULT_KEY_BYTES, key);
  return result == 0;
}
inline bool seal(const uint8_t key[VAULT_KEY_BYTES], const uint8_t salt[VAULT_SALT_BYTES],
                 const uint8_t nonce[VAULT_NONCE_BYTES], const uint8_t *plain, size_t plainLength,
                 uint8_t *output, size_t capacity, size_t &outputLength) {
  outputLength = 0;
  if (!key || !salt || !nonce || !plain || !output || plainLength > VAULT_MAX_PLAINTEXT ||
      capacity < VAULT_HEADER_BYTES + plainLength + VAULT_TAG_BYTES) return false;
  memcpy(output, "CKV1", 4); output[4] = 1; put32(output + 5, VAULT_KDF_ITERATIONS);
  memcpy(output + 9, salt, VAULT_SALT_BYTES); memcpy(output + 25, nonce, VAULT_NONCE_BYTES);
  mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
  int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (result == 0)
    result = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plainLength,
      nonce, VAULT_NONCE_BYTES, output, VAULT_HEADER_BYTES, plain, output + VAULT_HEADER_BYTES,
      VAULT_TAG_BYTES, output + VAULT_HEADER_BYTES + plainLength);
  mbedtls_gcm_free(&gcm);
  if (result != 0) { wipe(output, VAULT_HEADER_BYTES + plainLength + VAULT_TAG_BYTES); return false; }
  outputLength = VAULT_HEADER_BYTES + plainLength + VAULT_TAG_BYTES;
  return true;
}
inline bool open(const char *pin, size_t pinLength, const uint8_t *file, size_t fileLength,
                 uint8_t *plain, size_t capacity, size_t &plainLength,
                 uint8_t key[VAULT_KEY_BYTES], uint8_t salt[VAULT_SALT_BYTES]) {
  plainLength = 0;
  if (!file || fileLength < VAULT_HEADER_BYTES + VAULT_TAG_BYTES || fileLength > VAULT_MAX_FILE ||
      memcmp(file, "CKV1", 4) || file[4] != 1 || !plain) return false;
  const uint32_t iterations = get32(file + 5);
  const size_t encrypted = fileLength - VAULT_HEADER_BYTES;
  if (encrypted < VAULT_TAG_BYTES || encrypted - VAULT_TAG_BYTES > capacity) return false;
  memcpy(salt, file + 9, VAULT_SALT_BYTES);
  if (!deriveKey(pin, pinLength, salt, iterations, key)) return false;
  mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
  int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (result == 0)
    result = mbedtls_gcm_auth_decrypt(&gcm, encrypted - VAULT_TAG_BYTES, file + 25, VAULT_NONCE_BYTES,
      file, VAULT_HEADER_BYTES, file + fileLength - VAULT_TAG_BYTES, VAULT_TAG_BYTES,
      file + VAULT_HEADER_BYTES, plain);
  mbedtls_gcm_free(&gcm);
  if (result != 0) { wipe(key, VAULT_KEY_BYTES); wipe(plain, capacity); return false; }
  plainLength = encrypted - VAULT_TAG_BYTES;
  return true;
}
}
