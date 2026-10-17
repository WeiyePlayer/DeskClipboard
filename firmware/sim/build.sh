#!/bin/bash
# Build LVGL (from the firmware's managed component) plus main/ui.c for the host,
# then link the tap simulation. Usage: ./build.sh && build/sim
set -euo pipefail
cd "$(dirname "$0")"
L=../clipboard/managed_components/lvgl__lvgl
mkdir -p build/obj
if [[ ! -f build/liblvgl.a ]]; then
  find "$L/src" -name '*.c' | while read -r f; do
    echo "$f"
  done | xargs -P 8 -n 1 bash -c 'o=build/obj/$(echo "$0" | md5 -q).o; clang -c -O1 -w -DLV_CONF_INCLUDE_SIMPLE -I. -I'"$L"' "$0" -o "$o"'
  ar rcs build/liblvgl.a build/obj/*.o
fi
clang -O0 -g -Wall -Wno-deprecated-declarations -DLV_CONF_INCLUDE_SIMPLE -I. -Istubs -I"$L" -I../clipboard/main \
  ../clipboard/main/ui.c ../clipboard/main/screen.c sim_taps.c sim_replay.c build/liblvgl.a -o build/sim
