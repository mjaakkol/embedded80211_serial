/*
 * E2E test for the host app.
 *
 * WiFi: bring up the virtual interface, scan, connect (SSID/PSK from Kconfig,
 * build-time only), wait for DHCP, then ping the gateway.
 * BT: periodic BLE scan windows, logging discovered devices.
 *
 * Log markers (consumed by host/tests/run_e2e.sh):
 *   E2E WIFI: IFACE UP
 *   E2E WIFI: CONNECTED
 *   E2E WIFI: DHCP BOUND <ip> gw <gw>
 *   E2E WIFI: PING OK <n>/<N>
 *   E2E BT:  SCAN DONE found=<n>
 */

#ifndef E80211_HOST_E2E_TEST_H
#define E80211_HOST_E2E_TEST_H

void e2e_test_start(void);

#endif /* E80211_HOST_E2E_TEST_H */
