#!/usr/bin/env bash
# Launch QEMU (qemu_cortex_m3) for the host app, bridging the bus UART to the
# board's common link protocol port.
#
# uart0 (QEMU serial 0) is the console, written directly to a log file.
# uart1 (QEMU serial 1) is the zephyr,uart-pipe bus, bridged bidirectionally
# to the board's bus port via a standalone pty.
#
# On the nRF7002 DK the bus port is the board's USB CDC-ACM protocol port
# (target USB connector, VID 0x2FE3; the CDC_ACM_0 instance), auto-detected
# by probing with a PROTOCOL_INFO query.
#
# Usage: ./run_qemu.sh [board_port]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Board bus port. Auto-detect by probing each /dev/cu.usbmodem* with a
# PROTOCOL_INFO query (the bus port answers; the console port does not).
BOARD_PORT="${1:-$(python3 "$SCRIPT_DIR/tests/find_bus_port.py" 2>/dev/null)}"
if [ -z "$BOARD_PORT" ]; then
	echo "ERROR: no board bus port found; pass it as \$1" >&2
	exit 1
fi
BUS=/tmp/e80211-host-bus
CONSOLE_LOG=/tmp/e80211-host-console.log
ELF=../build/host/zephyr/zephyr.elf

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
TAIL_PID=""
cleanup() {
	[ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
	[ -n "$TAIL_PID" ] && kill "$TAIL_PID" 2>/dev/null
	kill "$SOCAT_BUS_PID" 2>/dev/null
}
trap cleanup EXIT

sleep 0.5

qemu-system-arm -machine lm3s6965evb -cpu cortex-m3 -nographic \
	-kernel "$SCRIPT_DIR/$ELF" \
	-serial "file:$CONSOLE_LOG" -serial "file:$BUS" -monitor none &
QEMU_PID=$!

# Stream the console log (QEMU writes to $CONSOLE_LOG directly). Run in the
# background so the EXIT trap can reap it; an orphaned tail -f would keep the
# harness's stdout pipe open (run_e2e.sh | tail never sees EOF).
tail -f "$CONSOLE_LOG" &
TAIL_PID=$!
wait "$TAIL_PID"
