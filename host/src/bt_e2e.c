/*
 * E2E Bluetooth test for the host app.
 *
 * Periodic BLE scan window, logging discovered devices.
 *
 * Log marker (consumed by host/tests/run_e2e.sh):
 *   E2E BT:  SCAN DONE found=<n>
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/addr.h>

#include "bt_e2e.h"

LOG_MODULE_REGISTER(e80211_e2e_bt, LOG_LEVEL_INF);

#define BT_SCAN_WINDOW_S  10U
#define BT_SCAN_PERIOD_S  30U

static int bt_found_count;

static void bt_adv_cb(const bt_addr_le_t *addr, int8_t rssi,
		      uint8_t adv_type, struct net_buf_simple *buf)
{
	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	bt_found_count++;
	LOG_INF("E2E BT: DEV %s rssi=%d", addr_str, rssi);
}

static const struct bt_le_scan_param scan_params[] = BT_LE_SCAN_ACTIVE;

static void bt_e2e_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	/* Wait for the Wi-Fi scan+connect to finish before touching the
	 * BT controller. The nRF7002 shares one IPC link for Wi-Fi and
	 * BT; concurrent HCI and Wi-Fi management commands cause HCI
	 * command timeouts and a fatal assertion.
	 */
	k_sleep(K_SECONDS(15));

	/* Bring up the BT controller, then wait for it to be ready. */
	int err = bt_enable(NULL);

	if (err < 0) {
		LOG_ERR("E2E BT: enable failed: %d", err);
		return;
	}

	while (!bt_is_ready()) {
		k_sleep(K_SECONDS(1));
	}
	LOG_INF("E2E BT: controller ready");


	/* Single scan window: start, collect results, log. The stop
	 * command (LE_Set_Scan_Enable 0x200c disable) times out on the
	 * nRF7002 controller over the IPC link, triggering a fatal HCI
	 * assertion. The scan window is the E2E criterion; stopping the
	 * scanner is not.
	 */
	bt_found_count = 0;
	int rc = bt_le_scan_start(scan_params, bt_adv_cb);

	if (rc != 0) {
		LOG_ERR("E2E BT: scan start failed: %d", rc);
		return;
	}

	k_sleep(K_SECONDS(BT_SCAN_WINDOW_S));
	LOG_INF("E2E BT: SCAN DONE found=%d", bt_found_count);
}

K_THREAD_STACK_DEFINE(bt_stack, 2048);
static struct k_thread bt_thread;

void bt_e2e_start(void)
{
	k_thread_create(&bt_thread, bt_stack,
			K_THREAD_STACK_SIZEOF(bt_stack),
			bt_e2e_thread, NULL, NULL, NULL,
			K_PRIO_COOP(8), 0, K_NO_WAIT);
	k_thread_name_set(&bt_thread, "bt_e2e");
}
