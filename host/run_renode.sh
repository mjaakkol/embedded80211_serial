#!/usr/bin/env bash
# Launch Renode (board renode_cortex_m3) for the host app, bridging the bus
# UART to the board's common link protocol port.
#
# uart0 (PL011 @ 0x40000000) is the console, written directly to a log file
# (Renode file backend, flushed immediately).
# uart1 (PL011 @ 0x40040000) is the bus, exposed as a Renode socket terminal
# on 127.0.0.1:25999 and bridged bidirectionally to the board port by
# tests/tunnel_bus.py.
#
# On the nRF7002 DK the bus port is the board's USB CDC-ACM protocol port
# (target USB connector, VID 0x2FE3; the CDC_ACM_0 instance), auto-detected
# by probing with a PROTOCOL_INFO query.
#
# Usage: ./run_renode.sh [board_port]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Board bus port. Auto-detect by probing each /dev/cu.usbmodem* with a
# PROTOCOL_INFO query (the bus port answers; the console port does not).
BOARD_PORT="${1:-$(python3 "$SCRIPT_DIR/tests/find_bus_port.py" 2>/dev/null)}"
if [ -z "$BOARD_PORT" ]; then
	echo "ERROR: no board bus port found; pass it as \$1" >&2
	exit 1
fi
BUS_PORT=25999
CONSOLE_LOG=/tmp/e80211-host-console.log
ELF=../build/host/zephyr/zephyr.elf

if [ ! -f "$SCRIPT_DIR/$ELF" ]; then
	echo "ERROR: $SCRIPT_DIR/$ELF not found; build the host app first." >&2
	exit 1
fi

# Locate the Renode binary: $RENODE, PATH, or the default install location.
RENODE_BIN="${RENODE:-$(command -v renode || true)}"
if [ -z "$RENODE_BIN" ] && [ -x "$HOME/tools/renode/Renode.app/Contents/MacOS/renode" ]; then
	RENODE_BIN="$HOME/tools/renode/Renode.app/Contents/MacOS/renode"
fi
if [ -z "$RENODE_BIN" ]; then
	echo "ERROR: renode not found; set \$RENODE or install Renode (https://github.com/renode/renode)." >&2
	exit 1
fi

rm -f "$CONSOLE_LOG"
: > "$CONSOLE_LOG"

# Generate the runner script: bus socket terminal, machine, console file
# backend, bus connection, ELF load.
RUN_RESC=/tmp/e80211-host-renode.resc
{
	echo "emulation CreateServerSocketTerminal $BUS_PORT \"bustcp\" false"
	echo
	echo "mach add \"host\""
	echo "machine LoadPlatformDescription @$SCRIPT_DIR/boards/renode_cortex_m3/support/renode_cortex_m3.repl"
	echo
	echo "sysbus.uart0 CreateFileBackend @$CONSOLE_LOG true"
	echo "connector Connect sysbus.uart1 bustcp"
	echo "sysbus LoadELF @$SCRIPT_DIR/$ELF"
	echo "s"
} > "$RUN_RESC"

RENODE_PID=""
TUNNEL_PID=""
TAIL_PID=""
cleanup() {
	[ -n "$RENODE_PID" ] && kill "$RENODE_PID" 2>/dev/null
	[ -n "$TAIL_PID" ] && kill "$TAIL_PID" 2>/dev/null
	[ -n "$TUNNEL_PID" ] && kill "$TUNNEL_PID" 2>/dev/null
	exec 9>&- 2>/dev/null
	rm -f "$STDIN_FIFO"
}
trap cleanup EXIT

# Keep renode's stdin open for the whole run: a closed stdin makes the
# monitor exit. A FIFO opened for read+write stays open without a writer.
STDIN_FIFO=/tmp/e80211-host-renode-stdin
rm -f "$STDIN_FIFO"
mkfifo "$STDIN_FIFO"
exec 9<>"$STDIN_FIFO"

"$RENODE_BIN" -p --console "$RUN_RESC" <&9 &
RENODE_PID=$!

# Bridge the bus socket to the board port. The bridge retries the socket
# connect itself, so it can start before Renode is up.
python3 "$SCRIPT_DIR/tests/tunnel_bus.py" "$BUS_PORT" "$BOARD_PORT" &
TUNNEL_PID=$!

# Stream the console log (Renode writes to $CONSOLE_LOG directly). Run in
# the background so the EXIT trap can reap it; an orphaned tail -f would
# keep the harness's stdout pipe open (run_e2e.sh | tail never sees EOF).
tail -f "$CONSOLE_LOG" &
TAIL_PID=$!
wait "$TAIL_PID"
