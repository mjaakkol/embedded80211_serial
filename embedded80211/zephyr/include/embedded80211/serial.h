#ifndef _SERIAL_H_
#define _SERIAL_H_

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/crc.h>

/* Wire protocol version. Bump when a wire format change is not backward
 * compatible. The optional per-frame CRC32C trailer (the `crc` header bit)
 * is a runtime opt-in, not a version change: frames with the bit clear are
 * byte-identical to the original format, so a v1 peer that never enables it
 * is unaffected.
 */
#define SERIAL_PROTOCOL_VERSION 1

/* Optional per-frame integrity trailer length (CRC32C, big-endian). */
#define SERIAL_CRC_LEN 4U

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

struct serial_config {
    struct k_work work_item;
    const struct device *const serial_dev;
    enum ParsingState parsing_state;
    struct SerialHeader {
        enum SerialType type: 4;     /* Traffic type (SerialType) */
        bool no_ack: 1;              /* No ACK requested */
        bool host: 1;                /* 1 = device→host direction */
        uint8_t error: 2;            /* Error code (ErrorType) */
        bool crc: 1;                  /* 1 = CRC32C trailer follows payload */
        uint8_t reserved: 3;          /* Reserved, must be zero */
        uint8_t sequence_number: 4;  /* Sequence number 0..15 */
        uint16_t length;             /* Payload length in bytes */
    } serial_header;
    /* Number of bytes read so far for the current packet payload. */
    size_t bytes_read;
    uint8_t rx_sequence_number;
    uint8_t tx_sequence_number;
    /* Raw 4 header wire bytes of the frame being parsed (for CRC32C). */
    uint8_t hdr_wire[4];
    /* CRC32C trailer of the frame being parsed, and how many of its
     * SERIAL_CRC_LEN bytes have been read so far (re-entrant RX).
     */
    uint8_t trailer[SERIAL_CRC_LEN];
    size_t trailer_read;
    /* Running CRC32C over header + payload of the frame being parsed.
     * Seeded with the header when it completes; updated per payload chunk.
     */
    uint32_t rx_crc;
    /* Runtime feature: when set, every frame this endpoint transmits
     * carries a CRC32C trailer and the `crc` header bit. Off by default,
     * so frames are byte-identical to protocol v1 until both peers opt in.
     */
    bool crc_enabled;
    struct ring_buf rx_ringbuf;
};



void initialize_uart(struct serial_config *config);
int serial_data_tx(const struct device *dev, const uint8_t *data, size_t len);

int serial_send_frame(struct serial_config *config,
                      enum SerialType type,
                      enum ErrorType error,
                      bool no_ack,
                      const uint8_t *payload,
                      size_t len);

/*
 * Compute the CRC32C (Castagnoli, iSCSI: init 0xFFFFFFFF, final XOR
 * 0xFFFFFFFF) over a 4-byte header and its payload. The receiver compares
 * this against the big-endian 4-byte trailer. `payload` may be NULL when
 * `len` is 0. Shared by the device (serial.c) and host (bus.c) so both sides
 * compute the identical value.
 */
static inline uint32_t serial_frame_crc(const uint8_t hdr[4],
                                        const uint8_t *payload, size_t len)
{
    uint32_t crc = crc32_c(0U, hdr, 4U, true, false);

    return crc32_c(crc, payload, len, false, true);
}

/*
 * Enable or disable the per-frame CRC32C feature on this endpoint. When
 * enabled, frames transmitted afterwards set the `crc` header bit and append
 * a 4-byte CRC32C trailer. Must be called before/after initialize_uart();
 * it takes effect on the next frame sent.
 */
void serial_set_crc_enabled(struct serial_config *config, bool enabled);

#endif // _SERIAL_H_
