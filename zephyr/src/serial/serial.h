#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

/* Wire protocol version. Bump when wire format changes are not backward compatible. */
#define SERIAL_PROTOCOL_VERSION 1

/* Maximum payload length per frame (AGENTS.md: max 4096 bytes). */
#define SERIAL_MAX_PAYLOAD_LEN 4096U

/* Sequence number wraps after 15 (4-bit range 0..15). */
#define SEQUENCE_NUMBER_MAX 15U

// Parsing states for serial data
enum ParsingState {
    PARSING_STATE_TYPE,
    PARSING_STATE_SEQUENCE_NUMBER,
    PARSING_STATE_LENGTH1,
    PARSING_STATE_LENGTH2,
    PARSING_STATE_DATA,
};

// Serial types for different protocols (max 16 types, 4-bit field)
enum SerialType {
    SERIAL_TYPE_WIFI_MGMT      = 0,
    SERIAL_TYPE_WIFI_DATA      = 1,
    SERIAL_TYPE_NET_IF_MGMT    = 2,  /* Network L2 interface management */
    SERIAL_TYPE_HCI            = 3,  /* Bluetooth HCI */
    SERIAL_TYPE_THREAD         = 4,  /* OpenThread Spinel */
    SERIAL_TYPE_NET_OFFLOADING = 5,  /* Network offloading (reserved) */
    SERIAL_TYPE_AT_CMD         = 6,  /* AT commands (reserved) */
    SERIAL_TYPE_PROTOCOL_INFO  = 7,  /* Protocol information */
    SERIAL_TYPE_VSERIAL        = 8,  /* Virtual serial interface */
    SERIAL_TYPE_UNKNOWN        = 9,  /* Must be last — used as array size sentinel */
};

// Error types for serial communication (2-bit field, values 0..3)
enum ErrorType {
    ERROR_TYPE_NO_ERROR                  = 0,
    ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER = 1,
    ERROR_TYPE_OUT_OF_BUFFERS            = 2,
    ERROR_TYPE_LENGTH_TOO_LONG           = 3,  /* Also used for unrecognized command */
};

struct client_callbacks {
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
        enum SerialType type: 4;     /* Traffic type (SerialType) */
        bool no_ack: 1;              /* No ACK requested */
        bool host: 1;                /* 1 = device→host direction */
        uint8_t error: 2;            /* Error code (ErrorType) */
        uint8_t reserved: 4;         /* Reserved, must be zero */
        uint8_t sequence_number: 4;  /* Sequence number 0..15 */
        uint16_t length;             /* Payload length in bytes */
    } serial_header;
    /* Number of bytes read so far for the current packet payload. */
    size_t bytes_read;
    struct ring_buf rx_ringbuf;
};



void initialize_uart(struct serial_config *config);
int serial_data_tx(const struct device *dev, const uint8_t *data, size_t len);

int serial_usb_tx(const struct device *dev, const uint8_t *data, size_t len);
int serial_uart_tx(const struct device *dev, const uint8_t *data, size_t len);

uint8_t serial_get_tx_sequence_number(void);
void serial_advance_tx_sequence_number(void);


#endif // _SERIAL_H_
