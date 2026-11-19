#!/usr/bin/env bash
# Launch Renode (board renode_cortex_m33) for the host app, bridging the bus
# UART to the board's common link protocol port.
#
# uart0 (PL011 @ 0x40000000) is the console, written to a log file via
# Renode's file backend (flushed immediately).
# uart1 (PL011 @ 0x40040000) is the bus, exposed via a Renode file backend
# log (TX) and the monitor port (RX, WriteChar commands). The bridge
# (tests/tunnel_bus.py) connects to the monitor port, sends the setup
# commands + LoadELF, then pumps bytes bidirectionally to the board port.
#
# On the nRF7002 DK the bus port is the board's USB CDC-ACM protocol port
# (target USB connector, VID 0x2FE3; the CDC_ACM_0 instance), auto-detected
# by probing with a PROTOCOL_INFO query.
#
# Usage: ./run_renode.sh [board_port]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Board bus port. Auto-detect by probing each /dev/ttyACM* with a
# PROTOCOL_INFO query (the bus port answers; the console port does not).
BOARD_PORT="${1:-$(python3 "$SCRIPT_DIR/tests/find_bus_port.py" 2>/dev/null)}"
if [ -z "$BOARD_PORT" ]; then
	echo "ERROR: no board bus port found; pass it as \$1" >&2
	exit 1
fi

MONITOR_PORT=25997
CONSOLE_LOG=/tmp/e80211-host-console.log
BUS_TX_LOG=/tmp/e80211-bus-tx.log
ELF="$SCRIPT_DIR/../build/host/zephyr/zephyr.elf"
REPL="$SCRIPT_DIR/boards/renode_cortex_m33/support/renode_cortex_m33.repl"

if [ ! -f "$ELF" ]; then
	echo "ERROR: $ELF not found; build the host app first." >&2
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

rm -f "$CONSOLE_LOG" "$BUS_TX_LOG"

RENODE_PID=""
TUNNEL_PID=""
TAIL_PID=""
cleanup() {
	[ -n "$TAIL_PID" ] && kill "$TAIL_PID" 2>/dev/null
	[ -n "$TUNNEL_PID" ] && kill "$TUNNEL_PID" 2>/dev/null
	[ -n "$RENODE_PID" ] && kill "$RENODE_PID" 2>/dev/null
}
trap cleanup EXIT

# Start Renode with the monitor port (no --console script; the tunnel
# bridge sends all commands via the port).
"$RENODE_BIN" -p -P "$MONITOR_PORT" --disable-gui < /dev/null &
RENODE_PID=$!

# Bridge the bus UART to the board port. The bridge connects to the monitor
# port, sends setup + LoadELF, then pumps bytes bidirectionally.
python3 "$SCRIPT_DIR/tests/tunnel_bus.py" \
	"$MONITOR_PORT" "$BOARD_PORT" "$BUS_TX_LOG" "$CONSOLE_LOG" "$ELF" "$REPL" &
TUNNEL_PID=$!

# Wait for the console log file to be created by the tunnel bridge's
# CreateFileBackend command, then stream it. The bridge first opens the
# board port, which can take up to a minute after a board reset (USB
# re-enumeration), so give it room.
for _ in $(seq 1 180); do
	if [ -f "$CONSOLE_LOG" ]; then
		break
	fi
	sleep 0.5
done

# Stream the console log. Run in the background so the EXIT trap can reap
# it; an orphaned tail -f would keep the harness's stdout pipe open.
tail -f "$CONSOLE_LOG" &
TAIL_PID=$!

wait "$TAIL_PID"
