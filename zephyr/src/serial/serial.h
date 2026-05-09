#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

// Parsing states for serial data
enum ParsingState {
    PARSING_STATE_TYPE,
    PARSING_STATE_SEQUENCE_NUMBER,
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

// Error types for serial communication
enum ErrorType {
    ERROR_TYPE_NO_ERROR,
    ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER,
    ERROR_TYPE_BUFFER_OVERFLOW,
    ERROR_TYPE_RESERVED_1
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
        enum SerialType type: 4; // 4 bits for type
        bool no_ack: 1; // No ACK flag
        bool host: 1; // Host flag
        uint8_t error: 2; // Error flag
        uint8_t reserved: 5; // Reserved bits
        uint8_t sequence_number: 3; // Sequence number for tracking packets
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
