/*
 * Host-side virtual Bluetooth HCI driver.
 *
 * Exposes a Zephyr BT HCI driver (`compatible = "embedded80211,bt-hci-virtual"`)
 * that proxies H:4 HCI traffic to the radio co-processor over the
 * link-protocol bus client. It is the symmetric mirror of the device's
 * bt_proxy.c.
 *
 *   - TX: H:4 net_bufs from the BT stack -> bus_send(SERIAL_TYPE_HCI).
 *   - RX: H:4 bytes from the bus -> net_buf from the matching BT pool ->
 *     the registered bt_hci_recv_t callback.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/drivers/bluetooth.h>
#include <string.h>

#include <embedded80211/bus.h>
#include <embedded80211/serial.h>

LOG_MODULE_REGISTER(virt_bt, LOG_LEVEL_INF);

#define DT_DRV_COMPAT embedded80211_bt_hci_virtual

static bt_hci_recv_t bt_recv;

static void hci_rx(const uint8_t *payload, size_t len, void *user)
{
	struct device *dev = user;

	if (bt_recv == NULL || len == 0) {
		return;
	}

	uint8_t h4_type = payload[0];
	enum bt_buf_type type = bt_buf_type_from_h4(h4_type, BT_BUF_IN);
	struct net_buf *buf = bt_buf_get_rx(type, K_NO_WAIT);

	if (buf == NULL) {
		LOG_WRN("HCI RX: no buffer for type 0x%02x", h4_type);
		return;
	}

	net_buf_add_mem(buf, payload, len);
	bt_recv(dev, buf);
}

static int virt_bt_open(const struct device *dev, bt_hci_recv_t recv)
{
	bt_recv = recv;
	bus_register_handler(SERIAL_TYPE_HCI, hci_rx);
	bus_set_handler_user(SERIAL_TYPE_HCI, dev);
	return 0;
}

static int virt_bt_close(const struct device *dev)
{
	ARG_UNUSED(dev);
	bt_recv = NULL;
	return 0;
}

static int virt_bt_send(const struct device *dev, struct net_buf *buf)
{
	ARG_UNUSED(dev);

	const uint8_t *data = buf->data;
	size_t len = buf->len;
	int rc = bus_send(SERIAL_TYPE_HCI, data, len);

	net_buf_unref(buf);
	return rc;
}

static const struct bt_hci_driver_api virt_bt_api = {
	.open = virt_bt_open,
	.close = virt_bt_close,
	.send = virt_bt_send,
};

static int virt_bt_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return bus_open();
}

DEVICE_DT_DEFINE(DT_DRV_INST(0), virt_bt_init, NULL, NULL, NULL,
		 POST_KERNEL, CONFIG_BT_HCI_INIT_PRIORITY, &virt_bt_api);
