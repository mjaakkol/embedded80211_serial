#ifndef _NET_IF_MGMT_PROXY_H_
#define _NET_IF_MGMT_PROXY_H_

#include <serial/serial.h>

bool net_if_mgmt_acquire_buffer(uint8_t **data, size_t len);
void net_if_mgmt_commit_data(size_t len);
void net_if_mgmt_message_complete(size_t len);

#endif /* _NET_IF_MGMT_PROXY_H_ */
