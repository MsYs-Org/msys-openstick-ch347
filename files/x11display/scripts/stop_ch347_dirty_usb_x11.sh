#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RUN_DIR="${RUN_DIR:-/tmp/ch347_dirty_usb_x11}"
PID_FILE="$RUN_DIR/pids"
IFACE_ID="${CH347_IFACE_ID:-1-1.1:1.4}"
DISPLAY_ID="${DISPLAY_ID:-:24}"

if [ -f "$PID_FILE" ]; then
    tac "$PID_FILE" | while read -r pid; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -CONT "$pid" 2>/dev/null || true
            kill "$pid" 2>/dev/null || true
        fi
    done
fi

pkill -f "ffmpeg .*x11grab.*${DISPLAY_ID}" 2>/dev/null || true
pkill -f "$PROJECT_DIR/bin/ch347_dirty_usb_sink" 2>/dev/null || true

sleep 1

if [ -f "$PID_FILE" ]; then
    tac "$PID_FILE" | while read -r pid; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -CONT "$pid" 2>/dev/null || true
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
fi

if [ -d /sys/bus/usb/drivers/ch34x_pis ] &&
        [ ! -e "/sys/bus/usb/drivers/ch34x_pis/$IFACE_ID" ]; then
    printf '%s' "$IFACE_ID" > /sys/bus/usb/drivers/ch34x_pis/bind 2>/dev/null || true
fi

rm -f "$PID_FILE"
echo "stopped dirty USB X11 CH347 stream"
