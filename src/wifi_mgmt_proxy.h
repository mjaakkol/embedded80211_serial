#ifndef _WIFI_MGMT_PROXY_H_
#define _WIFI_MGMT_PROXY_H_

#include <serial/serial.h>

#ifdef defined(WIFI_MGMT_BEARER_SERIAL_UART) || defined(WIFI_MGMT_BEARER_SERIAL_USB)
#define WIFI_MGMT_TX serial_data_tx
#else
#error "Unsupported configuration"
#endif // WIFI_MGMT_TX serial_usb_tx

#endif   // _WIFI_MGMT_PROXY_H_