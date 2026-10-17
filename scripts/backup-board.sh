#!/bin/bash
# Back up DeskClipboard pairing (NVS) + history (hubdata) [+ bootloader/app] to backups/board-<time>/.
# Board in download mode first: hold BOOT, replug USB, release. Usage: scripts/backup-board.sh [--no-app]
set -euo pipefail
cd "$(dirname "$0")/.."
exec .venv/bin/python scripts/board_image.py ${PORT:+--port "$PORT"} backup "$@"
