#ifndef _NET_IF_MGMT_PROXY_H_
#define _NET_IF_MGMT_PROXY_H_

#include <serial/serial.h>

bool net_if_mgmt_acquire_buffer(uint8_t **data, size_t len);
void net_if_mgmt_commit_data(size_t len);
void net_if_mgmt_message_complete(size_t len);

/**
 * @brief Set the serial device used to transmit net_if management responses.
 *
 * Must be called once from the serial transport init after the device is ready.
 */
void net_if_mgmt_proxy_set_device(const struct device *dev);

#endif /* _NET_IF_MGMT_PROXY_H_ */
