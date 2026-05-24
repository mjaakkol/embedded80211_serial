#ifndef _WIFI_MGMT_PROXY_H_
#define _WIFI_MGMT_PROXY_H_

#include <serial/serial.h>

bool wifi_mgmt_acquire_buffer(uint8_t **data, size_t len);
void wifi_mgmt_commit_data(size_t len);
void wifi_mgmt_message_complete(size_t len);

#endif   /* _WIFI_MGMT_PROXY_H_ */