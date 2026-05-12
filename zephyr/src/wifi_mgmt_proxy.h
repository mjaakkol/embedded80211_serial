#ifndef _WIFI_MGMT_PROXY_H_
#define _WIFI_MGMT_PROXY_H_

#include <serial/serial.h>

bool wifi_mgmt_acquire_buffer(uint8_t **data, size_t len);
void wifi_mgmt_commit_data(size_t len);
void wifi_mgmt_message_complete(size_t len);

/**
 * @brief Set the serial device used to transmit Wi-Fi management responses.
 *
 * Must be called once from the serial transport init (serial_usb.c / uart.c)
 * after the device is confirmed ready.
 */
void wifi_mgmt_proxy_set_device(const struct device *dev);

#if defined(CONFIG_WIFI_MGMT_BEARER_SERIAL_UART) || defined(CONFIG_WIFI_MGMT_BEARER_SERIAL_USB)
#define WIFI_MGMT_TX serial_data_tx
#else
#error "Unsupported configuration"
#endif /* WIFI_MGMT_TX */

#endif   /* _WIFI_MGMT_PROXY_H_ */