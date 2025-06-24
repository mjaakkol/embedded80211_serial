#ifndef _WIFI_MGMT_PROXY_H_
#define _WIFI_MGMT_PROXY_H_

#include <serial/serial.h>

bool wifi_mgmt_acquire_buffer(uint8_t **data, size_t len);
void wifi_mgmt_commit_data(size_t len);
void wifi_mgmt_message_complete(size_t len);


#if defined(CONFIG_WIFI_MGMT_BEARER_SERIAL_UART) || defined(CONFIG_WIFI_MGMT_BEARER_SERIAL_USB)
#define WIFI_MGMT_TX serial_data_tx
#else
#error "Unsupported configuration"
#endif // WIFI_MGMT_TX serial_usb_tx



#endif   // _WIFI_MGMT_PROXY_H_