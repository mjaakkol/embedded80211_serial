#!/usr/bin/env bash
# Automated E2E harness: build the host app, launch QEMU + the socat bridge,
# and assert the E2E log markers.
#
# Usage: E2E_SSID=... E2E_PSK=... ./tests/run_e2e.sh [board_port]
# (SSID/PSK are build-time only; an empty SSID skips the WiFi E2E and the
# harness reports it as SKIPPED, requiring only the BT scan marker.)
set -uo pipefail

source /home/mikko/ncs/env.sh

SSID="${E2E_SSID:-}"
PSK="${E2E_PSK:-}"
BOARD_PORT="${1:-/dev/serial/by-id/usb-Test_Vendor_My_Zephyr_CDC-ACM_Device_2994E6BE83CEFC3F-if00}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$SCRIPT_DIR"

west build -d build/host -s host -b qemu_cortex_m3 --no-sysbuild \
	-DCONFIG_HOST_TEST_ENABLED=y \
	-DCONFIG_HOST_TEST_WIFI_SSID="\"$SSID\"" \
	-DCONFIG_HOST_TEST_WIFI_PSK="\"$PSK\"" || { echo "BUILD FAIL"; exit 1; }

rm -f /tmp/e80211-host-console.log
./run_qemu.sh "$BOARD_PORT" &
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
	grep -q "E2E WIFI: OK"          "$LOG" || { echo "FAIL: wifi e2e"; PASS=0; }
fi
grep -q "E2E BT: SCAN DONE" "$LOG" || { echo "FAIL: bt scan"; PASS=0; }

[ "$PASS" -eq 1 ] && echo "E2E PASS" || { echo "E2E FAIL"; exit 1; }
