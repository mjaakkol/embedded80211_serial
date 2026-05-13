#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "serial/serial.h"

#ifdef CONFIG_BLUETOOTH
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/net_buf.h>
#endif

LOG_MODULE_REGISTER(bt_proxy, LOG_LEVEL_DBG);

/* HCI packet buffer - used to accumulate incoming HCI frames from serial link */
#define BT_PROXY_BUFFER_SIZE 260U  /* Maximum HCI packet size + H:4 type byte */

static uint8_t hci_rx_buffer[BT_PROXY_BUFFER_SIZE];
static size_t hci_rx_buffer_len = 0;

#ifdef CONFIG_BLUETOOTH
/* Reference to the HCI device (e.g., the nRF Bluetooth controller) */
static const struct device *hci_dev = NULL;

/* HCI packet type constants (for non-Bluetooth builds) */
#else
#define BT_HCI_H4_CMD   0x01    /* HCI Command packet */
#define BT_HCI_H4_ACL   0x02    /* HCI ACL Data packet */
#define BT_HCI_H4_SCO   0x03    /* HCI Synchronous Data packet */
#define BT_HCI_H4_EVT   0x04    /* HCI Event packet */
#define BT_HCI_H4_ISO   0x05    /* HCI ISO Data packet */
#endif

/**
 * @brief Acquire buffer space for incoming HCI frame from serial protocol.
 *
 * The serial layer calls this to get a buffer for incoming data.
 * We use a static buffer and track the current write position.
 *
 * @param data Output: pointer to the buffer where data should be written
 * @param len Number of bytes the serial layer wants to write
 * @return true if buffer space is available, false otherwise
 */
bool bt_proxy_acquire_buffer(uint8_t **data, size_t len)
{
    /* Check if requested size fits in our buffer from current position */
    if ((hci_rx_buffer_len + len) > BT_PROXY_BUFFER_SIZE) {
        LOG_WRN("bt_proxy: requested %zu bytes exceeds buffer capacity (current: %zu)",
                len, hci_rx_buffer_len);
        return false;
    }

    /* Return pointer to where the next data chunk should be written */
    *data = &hci_rx_buffer[hci_rx_buffer_len];
    return true;
}

/**
 * @brief Commit data chunk - called as serial data arrives.
 *
 * This may be called multiple times as chunks of the HCI frame arrive.
 * We just track how much data we've accumulated.
 *
 * @param len Number of bytes just written to the buffer
 */
void bt_proxy_commit_data(size_t len)
{
    hci_rx_buffer_len += len;
    LOG_DBG("bt_proxy: accumulated %zu bytes (total: %zu)", len, hci_rx_buffer_len);
}

/**
 * @brief Complete HCI message received - send to Bluetooth stack.
 *
 * Called by the serial layer when a complete HCI frame has been received.
 * We parse the H:4 packet type and forward to the appropriate handler.
 *
 * @param len Total length of the complete frame
 */
void bt_proxy_message_complete(size_t len)
{
    if (len == 0 || len > BT_PROXY_BUFFER_SIZE) {
        LOG_ERR("bt_proxy: invalid frame length %zu", len);
        hci_rx_buffer_len = 0;
        return;
    }

    /* Parse H:4 packet type (first byte) */
    uint8_t pkt_type = hci_rx_buffer[0];

    LOG_DBG("bt_proxy: received complete HCI frame - type=0x%02x, len=%zu",
            pkt_type, len);

    /* Validate packet type */
    if (pkt_type != BT_HCI_H4_CMD && pkt_type != BT_HCI_H4_ACL &&
        pkt_type != BT_HCI_H4_SCO && pkt_type != BT_HCI_H4_EVT &&
        pkt_type != BT_HCI_H4_ISO) {
        LOG_WRN("bt_proxy: unknown HCI packet type 0x%02x", pkt_type);
        hci_rx_buffer_len = 0;
        return;
    }

#ifdef CONFIG_BLUETOOTH
    int ret;

    /* If HCI device is not initialized, we can't send */
    if (!hci_dev) {
        LOG_WRN("bt_proxy: HCI device not initialized, dropping frame");
        hci_rx_buffer_len = 0;
        return;
    }

    /* Allocate a net_buf for the HCI packet */
    struct net_buf *buf = net_buf_alloc(&bt_dev_buf_pool, K_NO_WAIT);
    if (!buf) {
        LOG_ERR("bt_proxy: failed to allocate net_buf for HCI frame");
        hci_rx_buffer_len = 0;
        return;
    }

    /* Copy the entire HCI frame (including H:4 type byte) to the net_buf */
    net_buf_add_mem(buf, hci_rx_buffer, len);

    /* Send to Bluetooth controller through the HCI driver */
    ret = bt_hci_send(hci_dev, buf);
    if (ret) {
        LOG_ERR("bt_proxy: failed to send HCI packet to controller: %d", ret);
        net_buf_unref(buf);
    } else {
        LOG_DBG("bt_proxy: HCI packet sent to controller successfully");
    }
#else
    /* Bluetooth not configured - just log and drop */
    LOG_WRN("bt_proxy: Bluetooth not enabled (CONFIG_BLUETOOTH not set), "
            "HCI frame type=0x%02x dropped", pkt_type);
#endif

    /* Reset buffer for next frame */
    hci_rx_buffer_len = 0;
}

#ifdef CONFIG_BLUETOOTH
/**
 * @brief Initialize the Bluetooth proxy.
 *
 * Called during system initialization to set up the HCI device reference.
 *
 * @return 0 on success, negative error code on failure
 */
static int bt_proxy_init(void)
{
    LOG_DBG("bt_proxy: initializing");

    /* Get the HCI device from device tree */
    hci_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_bt_c2h));
    if (!hci_dev) {
        LOG_WRN("bt_proxy: no HCI device available (zephyr,bt-c2h not defined)");
        /* This is not necessarily fatal - the device may not have Bluetooth */
        return -ENODEV;
    }

    LOG_INF("bt_proxy: initialized with HCI device %s", hci_dev->name);
    return 0;
}

/* Register init function to run at APPLICATION level */
SYS_INIT(bt_proxy_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif


