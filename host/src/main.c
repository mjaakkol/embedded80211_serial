/*
 * Host application entry point (QEMU board profile).
 *
 * Boots the virtual Wi-Fi / BT HCI drivers (registered via app.overlay),
 * opens the link-protocol bus to the device, and starts the E2E test threads.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <embedded80211/bus.h>

#if CONFIG_HOST_TEST_ENABLED
#include "e2e_test.h"
#endif

LOG_MODULE_REGISTER(e80211_host, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("e80211 host app booting");

	int rc = bus_open();

	if (rc != 0) {
		LOG_ERR("bus_open failed: %d", rc);
	} else {
		LOG_INF("bus synced: supported types 0x%04x",
			bus_supported_types());
	}

#if CONFIG_HOST_TEST_ENABLED
	e2e_test_start();
#endif

	return 0;
}
