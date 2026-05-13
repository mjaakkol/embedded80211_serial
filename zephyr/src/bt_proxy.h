#ifndef _BT_PROXY_H_
#define _BT_PROXY_H_

#include <zephyr/device.h>

/**
 * @brief Acquire buffer space for incoming HCI frame data.
 *
 * Called by the serial protocol layer when it needs to store incoming HCI data.
 * The proxy provides a buffer where the data will be written.
 *
 * @param data Output: pointer to allocated buffer
 * @param len Number of bytes to allocate
 * @return true if buffer allocated successfully, false otherwise
 */
bool bt_proxy_acquire_buffer(uint8_t **data, size_t len);

/**
 * @brief Commit data - called when a chunk of HCI frame data is written.
 *
 * May be called multiple times as data arrives in chunks.
 *
 * @param len Number of bytes committed to the buffer
 */
void bt_proxy_commit_data(size_t len);

/**
 * @brief Complete HCI message received - send to Bluetooth stack.
 *
 * Called by the serial protocol layer when a complete HCI frame is available.
 * The proxy forwards this to the Zephyr Bluetooth controller.
 *
 * @param len Total length of the complete HCI frame
 */
void bt_proxy_message_complete(size_t len);

/**
 * @brief Initialize the Bluetooth proxy.
 *
 * Sets up the HCI device reference and initializes the proxy.
 *
 * @return 0 on success, negative error code on failure
 */
int bt_proxy_init(void);

#endif /* _BT_PROXY_H_ */
