#!/bin/bash
# Flash the clipboard firmware. While the clipboard firmware is running, the
# Type-C port is a CDC device and esptool cannot reset it. Hold BOOT, tap
# RESET, then release BOOT so the chip enumerates in download mode.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
PORT="${1:-}"

if [[ ! -f "$IDF/export.sh" ]]; then
  echo "找不到 ESP-IDF：$IDF" >&2
  exit 1
fi

# export.sh expects to be sourced and refers to unset variables.
set +u
# shellcheck disable=SC1091
source "$IDF/export.sh"
set -u

cd "$ROOT"
if [[ -z "${PORT}" ]]; then
  idf.py -b 921600 flash
else
  idf.py -p "$PORT" -b 921600 flash
fi
