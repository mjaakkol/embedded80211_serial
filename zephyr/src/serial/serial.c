#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>

#include "serial.h"

LOG_MODULE_REGISTER(serial, LOG_LEVEL_INF);

K_MUTEX_DEFINE(serial_tx_mutex);

static uint8_t rx_sequence_number = 0;
static uint8_t tx_sequence_number = 0;

/**
 * @brief Writes a block of data to the specified UART device using polling.
 *
 * @param dev Pointer to the UART device structure.
 * @param data Pointer to the data buffer to send.
 * @param len Number of bytes to send from the data buffer.
 * @return 0 on success, -EIO if the device is not ready or an error occurs.
 */
int serial_data_tx(const struct device *dev, const uint8_t *data, size_t len)
{
    // Only one thread can write to the UART at a time.
    // This also means that USB and serial UART are mutually exclusive, but
    // that's OK as it is not expected that anyone would use USB and UART at
    // the same time.
    k_mutex_lock(&serial_tx_mutex, K_FOREVER);
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(dev, data[i]);
    }
    k_mutex_unlock(&serial_tx_mutex);

    return 0;
}

/**
 * @brief UART interrupt callback function.
 *
 * This function is called by the UART driver when an interrupt occurs.
 * It checks for received data and reads it into a ring buffer.
 * It can also handle transmit-ready interrupts if needed.
 *
 * @param dev The UART device structure.
 * @param user_data User data passed to the callback (not used in this simple example).
 */
static void uart_irq_callback(const struct device *dev, void *user_data)
{
    struct serial_config *config = (struct serial_config *)user_data;

    LOG_INF("UART IRQ callback triggered");

    // Check if the interrupt is for received data
    while (uart_irq_is_pending(dev) && uart_irq_rx_ready(dev)) {

        switch (config->parsing_state) {
        case PARSING_STATE_TYPE:
            if (uart_fifo_read(dev, (uint8_t*)&config->serial_header, 1) == 1) {
                LOG_DBG("Received Serial Type: %u", config->serial_header.type);
                config->parsing_state = PARSING_STATE_SEQUENCE_NUMBER;
                config->bytes_read = 0; // Reset bytes read for new packet
            } else {
                LOG_ERR("Failed to read Serial Type");
            }
            break;
        case PARSING_STATE_SEQUENCE_NUMBER:
            if (uart_fifo_read(dev, ((uint8_t*)&config->serial_header)+1, 1) == 1) {
                LOG_DBG("Received Serial Seq: %u", config->serial_header.sequence_number);
                config->parsing_state = PARSING_STATE_LENGTH1;
            } else {
                LOG_ERR("Failed to read Serial Sequence Number");
            }
            break;
        case PARSING_STATE_LENGTH1:
            if (uart_fifo_read(dev, (uint8_t*)&config->serial_header.length, 1) == 1) {
                LOG_DBG("Received Serial Length (byte 1): %u", config->serial_header.length);
                config->parsing_state = PARSING_STATE_LENGTH2;
            } else {
                LOG_ERR("Failed to read Serial Length byte 1");
            }
            break;
        case PARSING_STATE_LENGTH2:
            if (uart_fifo_read(dev, ((uint8_t *)&config->serial_header.length) + 1, 1) == 1) {
                LOG_DBG("Received Serial Length (byte 2): %u", config->serial_header.length);
                config->serial_header.length = sys_be16_to_cpu(config->serial_header.length);
                if (config->serial_header.length == 0) {
                    LOG_ERR("Invalid Serial Length: 0");
                    config->parsing_state = PARSING_STATE_TYPE;
                } else if (config->serial_header.length > SERIAL_MAX_PAYLOAD_LEN) {
                    LOG_ERR("Payload too long: %u > %u", config->serial_header.length,
                            SERIAL_MAX_PAYLOAD_LEN);
                    /* Send LENGTH_TOO_LONG error ACK */
                    struct SerialHeader ack = {
                        .type = config->serial_header.type,
                        .no_ack = false,
                        .host = true,
                        .error = ERROR_TYPE_LENGTH_TOO_LONG,
                        .reserved = 0,
                        .sequence_number = config->serial_header.sequence_number,
                        .length = sys_cpu_to_be16(0),
                    };
                    serial_data_tx(dev, (const uint8_t *)&ack, sizeof(ack));
                    config->parsing_state = PARSING_STATE_TYPE;
                } else {
                    config->parsing_state = PARSING_STATE_DATA;
                }
            } else {
                LOG_ERR("Failed to read Serial Length byte 2");
            }
            break;
        case PARSING_STATE_DATA:
            // Issue system thread to read data
            uart_irq_rx_disable(dev);
            k_work_submit(&config->work_item);
            break;
        default:
            LOG_ERR("Unknown parsing state: %d", config->parsing_state);
            break;
        }
    }
}

static void send_ack(const struct device *dev, enum SerialType type,
                     uint8_t sequence_number, enum ErrorType error)
{
    struct SerialHeader ack = {
        .type = type,
        .no_ack = false,
        .host = true,
        .error = error,
        .reserved = 0,
        .sequence_number = sequence_number,
        .length = sys_cpu_to_be16(0),
    };
    serial_data_tx(dev, (const uint8_t *)&ack, sizeof(ack));
}

static void handle_protocol_info(const struct device *dev, uint8_t sequence_number)
{
    /* Protocol information response payload: version (1 byte), supported types bitmask
     * (2 bytes, LE), next expected rx sequence number (1 byte). */
    uint8_t payload[4];
    payload[0] = SERIAL_PROTOCOL_VERSION;
    payload[1] = (uint8_t)(SERIAL_SUPPORTED_TYPES_MASK & 0xFF);
    payload[2] = (uint8_t)((SERIAL_SUPPORTED_TYPES_MASK >> 8) & 0xFF);
    payload[3] = rx_sequence_number;

    uint16_t len_be = sys_cpu_to_be16(sizeof(payload));
    struct SerialHeader hdr = {
        .type = SERIAL_TYPE_PROTOCOL_INFO,
        .no_ack = false,
        .host = true,
        .error = ERROR_TYPE_NO_ERROR,
        .reserved = 0,
        .sequence_number = tx_sequence_number,
        .length = len_be,
    };

    serial_data_tx(dev, (const uint8_t *)&hdr, sizeof(hdr));
    serial_data_tx(dev, payload, sizeof(payload));

    tx_sequence_number = (tx_sequence_number >= SEQUENCE_NUMBER_MAX) ? 0
                         : tx_sequence_number + 1;
    ARG_UNUSED(sequence_number);
}

static void serial_work_handler(struct k_work *work_item_ptr)
{
    struct serial_config *config =
        CONTAINER_OF(work_item_ptr, struct serial_config, work_item);

    const struct device *dev = config->serial_dev;

    uint8_t *data_buffer;

    while (config->bytes_read < config->serial_header.length) {
        size_t buffer_size = config->serial_header.length - config->bytes_read;

        /* Protocol Info frames are handled inline — no client buffer needed. */
        if (config->serial_header.type == SERIAL_TYPE_PROTOCOL_INFO) {
            uint8_t discard[64];
            size_t to_read = MIN(buffer_size, sizeof(discard));
            int n = uart_fifo_read(dev, discard, to_read);
            if (n < 0) {
                LOG_ERR("UART FIFO read error: %d", n);
                break;
            }
            config->bytes_read += (size_t)n;
            if (config->bytes_read >= config->serial_header.length) {
                handle_protocol_info(dev, config->serial_header.sequence_number);
            }
            continue;
        }

        /* Validate client callback is wired for this traffic type. */
        if (config->type >= SERIAL_TYPE_UNKNOWN ||
            client_cb[config->type].acquire_buffer == NULL) {
            LOG_ERR("No buffer callback for type %d, dropping", config->type);
            uint8_t discard[64];
            size_t to_read = MIN(buffer_size, sizeof(discard));
            int n = uart_fifo_read(dev, discard, to_read);
            if (n < 0) {
                break;
            }
            config->bytes_read += (size_t)n;
            if (config->bytes_read >= config->serial_header.length) {
                send_ack(dev, config->type, config->serial_header.sequence_number,
                         ERROR_TYPE_LENGTH_TOO_LONG); /* reuse: unrecognised handler */
                config->parsing_state = PARSING_STATE_TYPE;
                config->type = SERIAL_TYPE_UNKNOWN;
                config->bytes_read = 0;
            }
            continue;
        }

        if (!client_cb[config->type].acquire_buffer(&data_buffer, buffer_size)) {
            LOG_ERR("Buffer acquisition failed for type %d", config->type);
            send_ack(dev, config->type, config->serial_header.sequence_number,
                     ERROR_TYPE_OUT_OF_BUFFERS);
            config->parsing_state = PARSING_STATE_TYPE;
            config->type = SERIAL_TYPE_UNKNOWN;
            config->bytes_read = 0;
            break;
        }

        int bytes_read = uart_fifo_read(dev, data_buffer + config->bytes_read, buffer_size);
        if (bytes_read < 0) {
            LOG_ERR("UART FIFO read error: %d", bytes_read);
            break;
        } else if (bytes_read == 0) {
            LOG_DBG("No more data available, waiting for next interrupt");
            break;
        }

        config->bytes_read += bytes_read;

        if (config->bytes_read >= config->serial_header.length) {
            LOG_INF("Received complete packet of length %u", config->serial_header.length);

            /* Validate sequence number. */
            if (rx_sequence_number != config->serial_header.sequence_number) {
                LOG_ERR("Incorrect sequence number: expected %u, received %u",
                        rx_sequence_number, config->serial_header.sequence_number);
                send_ack(dev, config->type, rx_sequence_number,
                         ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER);
                /* Reset expected sequence to 0 per AGENTS spec. */
                rx_sequence_number = 0;
            } else {
                /* Sequence OK — commit data and notify client. */
                client_cb[config->type].commit_data(config->serial_header.length);
                client_cb[config->type].message_complete(config->serial_header.length);

                /* Advance and wrap rx sequence number. */
                rx_sequence_number = (rx_sequence_number >= SEQUENCE_NUMBER_MAX) ? 0
                                     : rx_sequence_number + 1;

                /* Send ACK to host unless no_ack was requested. */
                if (!config->serial_header.no_ack) {
                    send_ack(dev, config->type,
                             config->serial_header.sequence_number,
                             ERROR_TYPE_NO_ERROR);
                }
            }

            /* Reset parser for next frame. */
            config->parsing_state = PARSING_STATE_TYPE;
            config->type = SERIAL_TYPE_UNKNOWN;
            config->bytes_read = 0;
        } else {
            LOG_DBG("Read %d bytes, total: %zu/%u", bytes_read, config->bytes_read,
                    config->serial_header.length);
            client_cb[config->type].commit_data(bytes_read);
        }
    }

    uart_irq_rx_enable(dev);
}

void initialize_uart(struct serial_config *config)
{
    LOG_INF("Initializing UART...");
    const struct device *dev = config->serial_dev;

    k_work_init(&config->work_item, serial_work_handler);

    uart_irq_callback_user_data_set(dev, uart_irq_callback, config);

    LOG_INF("About to enable interrupts");
    uart_irq_rx_enable(dev);

    LOG_INF("UART initialized and RX interrupts enabled");
}

uint8_t serial_get_tx_sequence_number(void)
{
    return tx_sequence_number;
}

void serial_advance_tx_sequence_number(void)
{
    tx_sequence_number = (tx_sequence_number >= SEQUENCE_NUMBER_MAX) ? 0
                         : tx_sequence_number + 1;
}
