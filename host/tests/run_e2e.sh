#!/usr/bin/env bash
# Automated E2E harness: build the host app, launch QEMU + the socat bridge,
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
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$SCRIPT_DIR"

west build -d ../build/host -s . -b qemu_cortex_m3 --no-sysbuild -- \
	-DCONFIG_HOST_TEST_ENABLED=y \
	-DCONFIG_HOST_TEST_WIFI_SSID="\"$SSID\"" \
	-DCONFIG_HOST_TEST_WIFI_PSK="\"$PSK\"" || { echo "BUILD FAIL"; exit 1; }

rm -f /tmp/e80211-host-console.log
# Redirect the harness stdout: run_qemu.sh's console stream must not inherit
# this script's stdout, or a pipe (run_e2e.sh | tail) never sees EOF.
./run_qemu.sh "$BOARD_PORT" > /dev/null 2>&1 &
HARNESS_PID=$!
sleep 120   # bound: WiFi E2E + one 30 s BT scan window
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
