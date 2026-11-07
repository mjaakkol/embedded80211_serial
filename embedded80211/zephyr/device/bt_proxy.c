/*
 * bt_proxy.c - Bluetooth HCI proxy (raw HCI bridge)
 *
 * Bridges the common link protocol's HCI traffic type (3) to the Zephyr
 * raw HCI channel (CONFIG_BT_HCI_RAW):
 *
 *  host -> device: link frames (type HCI) carrying raw H:4 HCI packets
 *  (command / ACL data) are validated and injected into the controller
 *  with bt_send().
 *
 *  device -> host: packets received from the controller (events, ACL
 *  data) are queued by the raw HCI channel into an internal fifo, drained
 *  by a dedicated thread and reported to the host as link frames (type
 *  HCI) through the serial multiplexer.
 *
 * The controller is the nRF5340 on-chip SoftDevice Controller running on
 * the network core; the application core reaches it over IPC
 * (zephyr,bt-hci chosen = bt_hci_ipc0). The host is the Bluetooth host:
 * it drives the controller with HCI commands (e.g. HCI_Reset) and
 * receives controller events.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci_raw.h>
#include <zephyr/bluetooth/hci_types.h>

#include <embedded80211/multiplexer.h>
#include <embedded80211/serial.h>

LOG_MODULE_REGISTER(bt_proxy, LOG_LEVEL_INF);

/* H:4 packet types (subset of the Zephyr HCI H4 definitions). */
#define BT_HCI_H4_CMD 0x01 /* HCI Command packet */
#define BT_HCI_H4_ACL 0x02 /* HCI ACL Data packet */
#define BT_HCI_H4_SCO 0x03 /* HCI Synchronous Data packet */
#define BT_HCI_H4_EVT 0x04 /* HCI Event packet */
#define BT_HCI_H4_ISO 0x05 /* HCI ISO Data packet */

/*
 * Accumulation buffer for incoming HCI frames from the serial link.
 * Sized for the largest H:4 packet the host may send (255 B ACL + 1 B
 * H:4 type); larger link payloads are rejected with OUT_OF_BUFFERS.
 */
#define BT_PROXY_BUFFER_SIZE 512U

static uint8_t hci_rx_buffer[BT_PROXY_BUFFER_SIZE];
static size_t hci_rx_buffer_len = 0;

/*
 * Raw HCI channel receive fifo: the controller's packets (delivered via
 * the IPC HCI driver) are queued here as net_bufs, each prefixed with
 * its H:4 type byte (see bt_buf_get_rx() in the BT subsystem).
 */
K_FIFO_DEFINE(hci_raw_rx_fifo);

/*
 * Controller -> host: drain the raw channel fifo and forward each HCI
 * packet to the host as an HCI traffic frame.
 *
 * Runs in a dedicated thread because serial egress applies backpressure
 * (bounded wait for TX space); blocking the system workqueue here would
 * stall the serial RX parser and every other work item.
 */
static void hci_event_tx_thread(void *fifo, void *arg1, void *arg2)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	struct k_fifo *queue = (struct k_fifo *)fifo;

	while (true) {
		struct net_buf *buf = k_fifo_get(queue, K_FOREVER);

		if (buf == NULL) {
			continue;
		}

		LOG_DBG("Controller -> host: H4=0x%02x len=%u",
			buf->data[0], buf->len);

		int err = serial_mux_send(SERIAL_TYPE_HCI, buf->data, buf->len);

		if (err != 0) {
			LOG_WRN("Failed to forward HCI event to host (%d)", err);
		}

		net_buf_unref(buf);
	}
}

K_THREAD_STACK_DEFINE(hci_event_tx_stack, 2048);
static struct k_thread hci_event_tx_thread_data;

/**
 * @brief Acquire buffer space for incoming HCI frame from serial protocol.
 *
 * The serial layer calls this to get a buffer for incoming data.
 * A static buffer is used; the serial layer writes the frame payload in
 * chunks and reports each chunk with bt_proxy_commit_data().
 */
bool bt_proxy_acquire_buffer(uint8_t **data, size_t len)
{
	if ((hci_rx_buffer_len + len) > BT_PROXY_BUFFER_SIZE) {
		LOG_WRN("Requested %zu bytes exceeds buffer capacity (current: %zu)",
			len, hci_rx_buffer_len);
		return false;
	}

	*data = &hci_rx_buffer[hci_rx_buffer_len];
	return true;
}

/**
 * @brief Commit data chunk - called as serial data arrives.
 *
 * May be called multiple times as chunks of the HCI frame arrive.
 */
void bt_proxy_commit_data(size_t len)
{
	hci_rx_buffer_len += len;
	LOG_DBG("Accumulated %zu bytes (total: %zu)", len, hci_rx_buffer_len);
}

/**
 * @brief Validate the accumulated H:4 packet.
 *
 * @return 0 if the packet is well formed, -EINVAL otherwise.
 */
static int hci_packet_validate(uint8_t h4_type, size_t len)
{
	switch (h4_type) {
	case BT_HCI_H4_CMD: {
		/* opcode (2) + param_len (1) + params */
		size_t param_len = hci_rx_buffer[3];

		return (len == 4U + param_len) ? 0 : -EINVAL;
	}
	case BT_HCI_H4_ACL: {
		/* addr (2) + len (2) + pb/flags (1) + payload */
		uint16_t payload_len = sys_le16_to_cpu(*(const uint16_t *)&hci_rx_buffer[3]);

		return (len == 6U + payload_len) ? 0 : -EINVAL;
	}
	default:
		return -ENOTSUP;
	}
}

/**
 * @brief Complete HCI message received - send to the Bluetooth controller.
 *
 * Called by the serial layer when a complete HCI frame (raw H:4 packet)
 * is available. The packet is validated, mapped to a raw HCI TX buffer
 * and handed to the controller.
 */
void bt_proxy_message_complete(size_t len)
{
	if (len < 2U || len > BT_PROXY_BUFFER_SIZE) {
		LOG_ERR("Invalid HCI frame length %zu", len);
		hci_rx_buffer_len = 0;
		return;
	}

	uint8_t h4_type = hci_rx_buffer[0];

	LOG_DBG("Received HCI frame: H4=0x%02x len=%zu", h4_type, len);

	int err = hci_packet_validate(h4_type, len);
	if (err != 0) {
		LOG_WRN("Malformed HCI packet (H4=0x%02x len=%zu): %d",
			h4_type, len, err);
		hci_rx_buffer_len = 0;
		return;
	}

	enum bt_buf_type buf_type;

	switch (h4_type) {
	case BT_HCI_H4_CMD:
		buf_type = BT_BUF_CMD;
		break;
	case BT_HCI_H4_ACL:
		buf_type = BT_BUF_ACL_OUT;
		break;
	default:
		/* Unreachable: validated above. */
		hci_rx_buffer_len = 0;
		return;
	}

	/* bt_buf_get_tx() prepends the H:4 type byte; pass the payload
	 * after it.
	 */
	struct net_buf *buf =
		bt_buf_get_tx(buf_type, K_NO_WAIT, &hci_rx_buffer[1], len - 1);

	if (buf == NULL) {
		LOG_ERR("No HCI TX buffer available (H4=0x%02x)", h4_type);
		hci_rx_buffer_len = 0;
		return;
	}

	err = bt_send(buf);
	if (err != 0) {
		LOG_ERR("Failed to send HCI packet to controller: %d", err);
		net_buf_unref(buf);
	} else {
		LOG_DBG("HCI packet sent to controller (H4=0x%02x len=%zu)",
			h4_type, len);
	}

	/* Reset buffer for the next frame. */
	hci_rx_buffer_len = 0;
}

static int bt_proxy_init(void)
{
	int err = bt_enable_raw(&hci_raw_rx_fifo);

	if (err != 0) {
		LOG_ERR("bt_enable_raw failed: %d", err);
		return err;
	}

	k_thread_create(&hci_event_tx_thread_data, hci_event_tx_stack,
		K_THREAD_STACK_SIZEOF(hci_event_tx_stack),
		hci_event_tx_thread, (void *)&hci_raw_rx_fifo, NULL, NULL,
		7, 0, K_NO_WAIT);
	k_thread_start(&hci_event_tx_thread_data);

	LOG_INF("BT HCI proxy ready (raw HCI channel over IPC)");
	return 0;
}

SYS_INIT(bt_proxy_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
