#!/usr/bin/env bash
# Automated E2E harness: build the host app, launch Renode + the bus bridge,
# and assert the E2E log markers.
#
# Usage: E2E_SSID=... E2E_PSK=... ./tests/run_e2e.sh [board_port]
# (SSID/PSK are build-time only; an empty SSID skips the WiFi E2E and the
# harness reports it as SKIPPED, requiring only the BT scan marker.)
set -uo pipefail

# NCS environment: Linux dev host or Mac (v3.4.1; toolchain via ~/.zephyrrc).
if [ -f /home/mikko/ncs/env.sh ]; then
	source /home/mikko/ncs/env.sh
elif [ -f /opt/nordic/ncs/v3.4.1/zephyr/zephyr-env.sh ]; then
	source /opt/nordic/ncs/v3.4.1/zephyr/zephyr-env.sh
else
	echo "ERROR: NCS environment not found" >&2
	exit 1
fi

SSID="${E2E_SSID:-}"
PSK="${E2E_PSK:-}"
# Board bus port; auto-detect by probing each /dev/cu.usbmodem* with a
# PROTOCOL_INFO query (the board's USB CDC-ACM port carrying the common
# link protocol bus answers; the console port and J-Link ports do not).
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD_PORT="${1:-$(python3 "$TESTS_DIR/find_bus_port.py" 2>/dev/null)}"
if [ -z "$BOARD_PORT" ]; then
	echo "ERROR: no board bus port found; pass it as \$1" >&2
	exit 1
fi

# Reset the board via J-Link SWD (optional; skips silently if JLinkExe
# is not on PATH).  A stale board state (leftover bus sequence, stuck
# WiFi state) makes the PI handshake time out.  The host USB port can
# also stay stuck after a reset (the port node vanishes and does not
# reappear on its own); a fresh reset cycle recovers it, so retry
# until the port is back.
if command -v JLinkExe &>/dev/null; then
	cat > /tmp/e80211-reset.jlink <<'JL'
si SWD
speed 1000
connect
r
g
exit
JL
	for attempt in 1 2 3; do
		# Probe SN is required: JLinkExe without -SelectEmuBySN hangs on
		# "Connecting to J-Link ..." in this environment. Use the external
		# probe, not the onboard Segger debugger.
		timeout 30 JLinkExe -device nRF5340 -SelectEmuBySN 852000646 \
			-CommandFile /tmp/e80211-reset.jlink >/dev/null 2>&1 || true
		# Wait for the USB bus port to reappear after re-enumeration.
		for _ in $(seq 1 9); do
			[ -e "$BOARD_PORT" ] && break
			sleep 5
		done
		[ -e "$BOARD_PORT" ] && break
		echo "board port $BOARD_PORT not back after reset attempt $attempt"
	done
	[ -e "$BOARD_PORT" ] || \
		echo "WARNING: board port $BOARD_PORT missing; E2E will likely fail"
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$SCRIPT_DIR"

west build -d ../build/host -s . -b renode_cortex_m33 --no-sysbuild -- \
	-DCONFIG_HOST_TEST_ENABLED=y \
	-DCONFIG_HOST_TEST_WIFI_SSID="\"$SSID\"" \
	-DCONFIG_HOST_TEST_WIFI_PSK="\"$PSK\"" || { echo "BUILD FAIL"; exit 1; }

rm -f /tmp/e80211-host-console.log
# Redirect the harness stdout: run_renode.sh's console stream must not inherit
# this script's stdout, or a pipe (run_e2e.sh | tail) never sees EOF.
./run_renode.sh "$BOARD_PORT" > /dev/null 2>&1 &
HARNESS_PID=$!
sleep 150   # bound: port re-enumeration + WiFi E2E + one 30 s BT scan window
kill "$HARNESS_PID" 2>/dev/null || true

LOG=/tmp/e80211-host-console.log
PASS=1
WIFI_EXPECTED=1
if [ -z "$SSID" ]; then
	WIFI_EXPECTED=0
	echo "WiFi E2E: SKIPPED (no E2E_SSID)"
fi

if [ "$WIFI_EXPECTED" -eq 1 ]; then
	grep -q "E2E WIFI: DHCP BOUND" "$LOG" || { echo "FAIL: no DHCP"; PASS=0; }
	grep -q "E2E WIFI: GATEWAY PING OK" "$LOG" || { echo "FAIL: gateway ping"; PASS=0; }
fi
grep -q "E2E BT: SCAN DONE" "$LOG" || { echo "FAIL: bt scan"; PASS=0; }

[ "$PASS" -eq 1 ] && echo "E2E PASS" || { echo "E2E FAIL"; exit 1; }
