#!/bin/bash
# Build main/pair_crypto.c for the host against ESP-IDF's mbedtls sources
# (default mbedtls config) so test_pair_crypto.py can check it against the
# RFC 9382 vectors and the Python reference.
set -euo pipefail
cd "$(dirname "$0")"
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
MB="$IDF/components/mbedtls/mbedtls"
mkdir -p build/mbedtls
if [[ ! -f build/libmbedcrypto.a ]]; then
  for f in "$MB"/library/*.c; do
    clang -c -O1 -w -I"$MB/include" -I"$MB/library" "$f" -o "build/mbedtls/$(basename "$f" .c).o"
  done
  ar rcs build/libmbedcrypto.a build/mbedtls/*.o
fi
clang -g -O1 -Wall -Wextra -fsanitize=address,undefined -I../clipboard/main -I"$MB/include" \
  ../clipboard/main/pair_crypto.c pair_driver.c build/libmbedcrypto.a -o build/pair_driver
