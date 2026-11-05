#include "../arduino/08_LVGL_Clock/src/vault_crypto.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
  const char pin[] = "1234567890";
  uint8_t salt[16], nonce[12], key[32];
  for (uint8_t i = 0; i < 16; ++i) salt[i] = i;
  for (uint8_t i = 0; i < 12; ++i) nonce[i] = i + 20;
  assert(VaultCrypto::deriveKey(pin, 10, salt, 100000, key));
  const uint8_t expected[] = {0x0f,0x1a,0x74,0xc8,0x6f,0xc7,0x60,0xa7,0xf7,0x41,0xac,0x94,0x08,0x57,0x51,0xb1,0x71,0x72,0xc5,0xc2,0xfa,0xbe,0xda,0x44,0x8c,0xf5,0x21,0xdf,0xa3,0x35,0xd1,0x78};
  assert(memcmp(key, expected, 32) == 0);
  assert(VaultCrypto::deriveKey(pin, 10, salt, VaultCrypto::VAULT_KDF_ITERATIONS, key));
  const char message[] = "{\"records\":[{\"title\":\"test\",\"password\":\"secret\"}]}";
  std::vector<uint8_t> file(VaultCrypto::VAULT_MAX_FILE), plain(VaultCrypto::VAULT_MAX_PLAINTEXT);
  size_t fileLength = 0, plainLength = 0;
  assert(VaultCrypto::seal(key, salt, nonce, reinterpret_cast<const uint8_t *>(message),
                           sizeof(message), file.data(), file.size(), fileLength));
  assert(fileLength == VaultCrypto::VAULT_HEADER_BYTES + sizeof(message) + 16);
  uint8_t openedKey[32], openedSalt[16];
  assert(VaultCrypto::open(pin, 10, file.data(), fileLength, plain.data(), plain.size(),
                           plainLength, openedKey, openedSalt));
  assert(plainLength == sizeof(message) && memcmp(plain.data(), message, plainLength) == 0);
  assert(memcmp(openedKey, key, 32) == 0);
  assert(!VaultCrypto::open("0987654321", 10, file.data(), fileLength, plain.data(), plain.size(),
                            plainLength, openedKey, openedSalt));
  // Every authenticated header field and the tag must be tamper-evident.
  for (size_t index : {size_t(0), size_t(4), size_t(5), size_t(9), size_t(25),
                       VaultCrypto::VAULT_HEADER_BYTES, fileLength - 1}) {
    auto copy = file; copy[index] ^= 1;
    assert(!VaultCrypto::open(pin, 10, copy.data(), fileLength, plain.data(), plain.size(),
                              plainLength, openedKey, openedSalt));
  }
  assert(!VaultCrypto::open(pin, 10, file.data(), fileLength - 1, plain.data(), plain.size(),
                            plainLength, openedKey, openedSalt));
  assert(!VaultCrypto::seal(key, salt, nonce, plain.data(), VaultCrypto::VAULT_MAX_PLAINTEXT + 1,
                            file.data(), file.size(), fileLength));
  assert(!VaultCrypto::deriveKey("123456789", 9, salt, 100000, key));
  puts("vault crypto: PBKDF2 vector, GCM round-trip, wrong PIN, header/ciphertext/tag tamper, bounds OK");
}
