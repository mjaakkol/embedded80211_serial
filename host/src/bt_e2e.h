/*
 * E2E Bluetooth test for the host app.
 *
 * Periodic BLE scan window, logging discovered devices.
 *
 * Log marker (consumed by host/tests/run_e2e.sh):
 *   E2E BT:  SCAN DONE found=<n>
 */

#ifndef E80211_HOST_BT_E2E_H
#define E80211_HOST_BT_E2E_H

void bt_e2e_start(void);

#endif /* E80211_HOST_BT_E2E_H */
