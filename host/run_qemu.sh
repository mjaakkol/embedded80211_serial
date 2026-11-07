#!/usr/bin/env bash
# Launch QEMU (qemu_cortex_m3) for the host app, bridging the bus UART to the
# physical board's CDC-ACM protocol port.
#
# uart0 (QEMU serial 0) is the console, written directly to a log file.
# uart1 (QEMU serial 1) is the zephyr,uart-pipe bus, bridged bidirectionally to
# the board's CDC-ACM protocol port via a standalone pty.
#
# Usage: ./run_qemu.sh [board_port]
set -uo pipefail

BOARD_PORT="${1:-/dev/serial/by-id/usb-Test_Vendor_My_Zephyr_CDC-ACM_Device_2994E6BE83CEFC3F-if00}"
BUS=/tmp/e80211-host-bus
CONSOLE_LOG=/tmp/e80211-host-console.log
ELF=../build/host/zephyr/zephyr.elf
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -f "$SCRIPT_DIR/$ELF" ]; then
	echo "ERROR: $SCRIPT_DIR/$ELF not found; build the host app first." >&2
	exit 1
fi

rm -f "$BUS" "$CONSOLE_LOG"
: > "$CONSOLE_LOG"
# Bus pty: bidirectional bridge between QEMU's bus UART and the board port.
socat pty,raw,echo=0,link="$BUS" "$BOARD_PORT" &
SOCAT_BUS_PID=$!

QEMU_PID=""
cleanup() {
	[ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
	kill "$SOCAT_BUS_PID" 2>/dev/null
}
trap cleanup EXIT

sleep 0.5

qemu-system-arm -machine lm3s6965evb -cpu cortex-m3 -nographic \
	-kernel "$SCRIPT_DIR/$ELF" \
	-serial "file:$CONSOLE_LOG" -serial "file:$BUS" -monitor none &
QEMU_PID=$!

# Stream the console log (QEMU writes to $CONSOLE_LOG directly).
tail -f "$CONSOLE_LOG"
