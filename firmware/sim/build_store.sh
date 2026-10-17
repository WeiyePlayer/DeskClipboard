#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
mkdir -p build
clang -g -O1 -Wall -Wextra -Wno-deprecated-declarations -fsanitize=address,undefined \
  -I../clipboard/main -I"$IDF/components/json/cJSON" \
  ../clipboard/main/hub_store.c "$IDF/components/json/cJSON/cJSON.c" store_driver.c -o build/store_driver
