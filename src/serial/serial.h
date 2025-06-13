#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

void initialize_uart(const struct device *dev, struct ring_buf *rx_ringbuf);
int serial_data_tx(const struct device *dev, const uint8_t *data, size_t len);


#endif // _SERIAL_H_
