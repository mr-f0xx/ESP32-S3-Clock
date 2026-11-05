#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
MBEDTLS_DIR="${MBEDTLS_DIR:-/tmp/mbedtls-v3.6.4}"
if [ ! -f "$MBEDTLS_DIR/library/gcm.c" ]; then
  git clone --depth 1 --branch v3.6.4 --filter=blob:none --sparse \
    https://github.com/Mbed-TLS/mbedtls.git "$MBEDTLS_DIR"
  git -C "$MBEDTLS_DIR" sparse-checkout set library include
fi
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/config.h" <<'EOF'
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_PKCS5_C
EOF
for source in aes gcm cipher cipher_wrap md sha256 sha512 pkcs5 platform_util constant_time; do
  gcc -O2 -I"$MBEDTLS_DIR/include" -I"$MBEDTLS_DIR/library" -I"$WORK" \
    -DMBEDTLS_CONFIG_FILE='"config.h"' -c "$MBEDTLS_DIR/library/$source.c" -o "$WORK/$source.o"
done
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I"$MBEDTLS_DIR/include" -I"$WORK" \
  -DMBEDTLS_CONFIG_FILE='"config.h"' tests/vault_crypto_test.cpp "$WORK"/*.o -o "$WORK/test"
"$WORK/test"
