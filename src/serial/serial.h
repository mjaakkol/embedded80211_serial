#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

// Parsing states for serial data
enum ParsingState {
    PARSING_STATE_TYPE,
    PARSING_STATE_CRC,
    PARSING_STATE_LENGTH1,
    PARSING_STATE_LENGTH2,
    PARSING_STATE_DATA,
};

// Serial types for different protocols
enum SerialType {
    SERIAL_TYPE_WIFI_MGMT,
    SERIAL_TYPE_WIFI_DATA,
    SERIAL_TYPE_HCI,
    SERIAL_TYPE_THREAD,
    SERIAL_TYPE_AT_CMD,
    SERIAL_TYPE_UNKNOWN,
};

struct client_callbacks {
    //int (*serial_tx)(const uint8_t *data, size_t len);
    //int (*serial_rx)(const uint8_t *data, size_t len);
    bool (*acquire_buffer)(uint8_t **data, size_t len);
    void (*commit_data)(size_t len);
    void (*message_complete)(size_t len);
};

extern struct client_callbacks client_cb[SERIAL_TYPE_UNKNOWN];

struct serial_config {
    struct k_work work_item;
    const struct device *const serial_dev;
    enum ParsingState parsing_state;
    enum SerialType type;
    struct SerialHeader {
        enum SerialType type;
        uint16_t crc; // CRC16 checksum
        uint8_t no_ack; // No ACK flag
        uint8_t host; // Host flag
        uint8_t error; // Error flag
        uint16_t length; // Length of the data payload
    } serial_header;
    //  Number of bytes read so far. This will be used to check if we have read the entire packet.
    size_t bytes_read;
    struct ring_buf rx_ringbuf;
};



void initialize_uart(struct serial_config *config);
int serial_data_tx(const struct device *dev, const uint8_t *data, size_t len);

int serial_usb_tx(const struct device *dev, const uint8_t *data, size_t len);
int serial_uart_tx(const struct device *dev, const uint8_t *data, size_t len);


#endif // _SERIAL_H_
