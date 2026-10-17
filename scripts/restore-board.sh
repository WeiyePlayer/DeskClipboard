#!/bin/bash
# Restore a backup made by backup-board.sh (verifies SHA-256 and board MAC first; never erases the chip).
# Usage: scripts/restore-board.sh backups/board-<time> [--app] [--dry-run]
set -euo pipefail
cd "$(dirname "$0")/.."
exec .venv/bin/python scripts/board_image.py ${PORT:+--port "$PORT"} restore "$@"
