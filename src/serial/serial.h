#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

void initialize_uart(const struct device *dev, struct ring_buf *rx_ringbuf);
int serial_data_tx(const struct device *dev, const uint8_t *data, size_t len);

int (*serial_tx)(const uint8_t *data, size_t len);
int (*serial_rx)(uint8_t *data, size_t len);


enum SerialType {
    SERIAL_TYPE_WIFI_MGMT = 0x01,
    SERIAL_TYPE_WIFI_DATA,
    SERIAL_TYPE_HCI,
    SERIAL_TYPE_THREAD,
    SERIAL_TYPE_AT_CMD
};

struct header {
    uint8_t type: 4;
    uint8_t crc : 1;
    uint8_t no_ack: 1;
    uint8_t host: 1;
    uint8_t error: 1;
    uint16_t length;
};

#endif // _SERIAL_H_
