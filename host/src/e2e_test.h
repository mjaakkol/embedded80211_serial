/*
 * E2E network integration test for the host app.
 *
 * Brings up IPv4 over Wi-Fi (SSID/PSK from Kconfig, build-time only):
 * interface info, connect, DHCP, print the assigned configuration,
 * ping the default gateway and report a pass/fail summary.
 *
 * The Bluetooth E2E part is in bt_e2e.h/bt_e2e.c.
 *
 * Log markers (consumed by host/tests/run_e2e.sh):
 *   E2E WIFI: CONNECTED
 *   E2E WIFI: DHCP BOUND
 *   E2E WIFI: PING SUMMARY
 *   E2E WIFI: OK
 */

#ifndef E80211_HOST_E2E_TEST_H
#define E80211_HOST_E2E_TEST_H

void e2e_test_start(void);

#endif /* E80211_HOST_E2E_TEST_H */
