#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>
#include <errno.h>

#include "serial.h"
#include "../multiplexer.h"

LOG_MODULE_REGISTER(serial, LOG_LEVEL_INF);

K_MUTEX_DEFINE(serial_tx_mutex);

/* Bounded wait for TX FIFO space. A healthy host drains the CDC ACM ring at
 * ~128 KB/s (128 B per USB bulk transfer, 1 ms work schedule), so a full
 * 4100 B frame needs well under 100 ms. The bound only guards against a
 * disconnected host, where the ring never drains.
 */
#define SERIAL_TX_TIMEOUT_MS 500

/**
 * @brief Writes a block of data to the specified UART device, applying
 *        backpressure while the TX FIFO is full.
 *
 * The CDC ACM backend's ring (see app.overlay cdc_serial_uart
 * tx-fifo-size) is drained by the USB work queue as the host reads the
 * endpoint; a full ring used to make the old uart_poll_out loop discard
 * bytes silently, truncating large frames. Now the caller waits until the
 * FIFO accepts every byte (bounded by SERIAL_TX_TIMEOUT_MS).
 *
 * @param dev Pointer to the UART device structure.
 * @param data Pointer to the data buffer to send.
 * @param len Number of bytes to send from the data buffer.
 * @return 0 on success, -ENOSPC if the FIFO stayed full for
 *         SERIAL_TX_TIMEOUT_MS, or a driver error.
 */
int serial_data_tx(const struct device *dev, const uint8_t *data, size_t len)
{
    // Only one thread can write to the UART at a time.
    // This also means that USB and serial UART are mutually exclusive, but
    // that's OK as it is not expected that anyone would use USB and UART at
    // the same time.
    size_t written = 0;
    int64_t start = k_uptime_get();

    k_mutex_lock(&serial_tx_mutex, K_FOREVER);
    while (written < len) {
        int n = uart_fifo_fill(dev, data + written, len - written);

        if (n < 0) {
            k_mutex_unlock(&serial_tx_mutex);
            return n;
        }

        if (n > 0) {
            written += (size_t)n;
            continue;
        }

        /* TX FIFO full. A real UART drains in ~10 us per byte. The CDC ACM
         * backend drains only as fast as the host reads the USB endpoint,
         * so apply backpressure instead of silently dropping bytes: the
         * old uart_poll_out loop discarded whatever did not fit in the
         * ring (see app.overlay cdc_serial_uart tx-fifo-size).
         */
        if (k_uptime_get() - start > SERIAL_TX_TIMEOUT_MS) {
            k_mutex_unlock(&serial_tx_mutex);
            LOG_ERR("%u of %u TX bytes unsent (TX FIFO full for %d ms); "
                    "frame is truncated",
                    (unsigned int)written, (unsigned int)len,
                    SERIAL_TX_TIMEOUT_MS);
            return -ENOSPC;
        }
        k_busy_wait(10);
    }
    k_mutex_unlock(&serial_tx_mutex);

    return 0;
}

int serial_send_frame(struct serial_config *config,
                      enum SerialType type,
                      enum ErrorType error,
                      bool no_ack,
                      const uint8_t *payload,
                      size_t len)
{
    if (config == NULL || config->serial_dev == NULL) {
        return -ENODEV;
    }

    if (len > SERIAL_MAX_PAYLOAD_LEN) {
        return -EMSGSIZE;
    }

    struct SerialHeader hdr = {
        .type = type,
        .no_ack = no_ack,
        .host = true,
        .error = error,
        .reserved = 0,
        .sequence_number = config->tx_sequence_number,
        /* Length is transmitted big-endian on the wire: byte2 = high,
         * byte3 = low (the RX parser applies sys_be16_to_cpu).
         */
        .length = sys_cpu_to_be16((uint16_t)len),
    };

    serial_data_tx(config->serial_dev, (const uint8_t *)&hdr, sizeof(hdr));
    if (payload != NULL && len > 0U) {
        serial_data_tx(config->serial_dev, payload, len);
    }

    config->tx_sequence_number = (config->tx_sequence_number >= SEQUENCE_NUMBER_MAX) ? 0
                                 : config->tx_sequence_number + 1;

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
    uint8_t b;


    /*
     * CDC ACM (USB) driver semantics differ from a real UART: this callback
     * is invoked from work context once per RX event, and the driver clears
     * its internal pending/ready flags *before* calling it.  Therefore the
     * classic
     *   while (uart_irq_is_pending(dev) && uart_irq_rx_ready(dev))
     * condition is never true here, the loop body would never run, and bytes
     * would sit in the RX ring forever (the driver would also re-submit this
     * work item while the ring stays non-empty, starving the workqueue).
     *
     * Instead, drain every header byte currently available: uart_fifo_read()
     * returns 0 when the ring is empty, which ends the loop.  The driver
     * re-invokes the callback when more data arrives.
     */
    while (config->parsing_state != PARSING_STATE_DATA) {
        if (uart_fifo_read(dev, &b, 1) != 1) {
            break;
        }

        switch (config->parsing_state) {
        case PARSING_STATE_TYPE:
            *(uint8_t *)&config->serial_header = b;
            LOG_DBG("Received Serial Type: %u", config->serial_header.type);
            config->parsing_state = PARSING_STATE_SEQUENCE_NUMBER;
            config->bytes_read = 0; // Reset bytes read for new packet
            break;
        case PARSING_STATE_SEQUENCE_NUMBER:
            (*((uint8_t *)&config->serial_header + 1)) = b;
            LOG_DBG("Received Serial Seq: %u", config->serial_header.sequence_number);
            config->parsing_state = PARSING_STATE_LENGTH1;
            break;
        case PARSING_STATE_LENGTH1:
            *(uint8_t *)&config->serial_header.length = b;
            config->parsing_state = PARSING_STATE_LENGTH2;
            break;
        case PARSING_STATE_LENGTH2:
            (*((uint8_t *)&config->serial_header.length + 1)) = b;
            config->serial_header.length = sys_be16_to_cpu(config->serial_header.length);
            if (config->serial_header.length == 0) {
                if (config->serial_header.type == SERIAL_TYPE_PROTOCOL_INFO) {
                    config->parsing_state = PARSING_STATE_DATA;
                } else {
                    LOG_ERR("Invalid Serial Length: 0");
                    config->parsing_state = PARSING_STATE_TYPE;
                }
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
                LOG_DBG("Received Serial Length: %u", config->serial_header.length);
                config->parsing_state = PARSING_STATE_DATA;
            }
            break;
        case PARSING_STATE_DATA:
            /* Unreachable: the loop exits before any payload byte is read. */
            break;
        default:
            LOG_ERR("Unknown parsing state: %d", config->parsing_state);
            break;
        }
    }

    /*
     * Complete header: the payload remains in the ring for
     * serial_work_handler, which reads exactly
     * serial_header.length - bytes_read bytes from it.  Queue the handler
     * (no-op if already pending).
     */
    if (config->parsing_state == PARSING_STATE_DATA) {
        (void)k_work_submit(&config->work_item);
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

static uint16_t serial_get_supported_types_mask(void)
{
    return serial_mux_supported_types_mask();
}

static void handle_protocol_info(struct serial_config *config, uint8_t sequence_number)
{
    /* Protocol information response payload: version (1 byte), supported types bitmask
     * (2 bytes, LE), next expected rx sequence number (1 byte). */
    uint8_t payload[4];
    uint16_t supported_types = serial_get_supported_types_mask();

    payload[0] = SERIAL_PROTOCOL_VERSION;
    payload[1] = (uint8_t)(supported_types & 0xFF);
    payload[2] = (uint8_t)((supported_types >> 8) & 0xFF);
    payload[3] = config->rx_sequence_number;

    (void)serial_send_frame(config,
                            SERIAL_TYPE_PROTOCOL_INFO,
                            ERROR_TYPE_NO_ERROR,
                            false,
                            payload,
                            sizeof(payload));
    ARG_UNUSED(sequence_number);
}

static void serial_work_handler(struct k_work *work_item_ptr)
{
    struct serial_config *config =
        CONTAINER_OF(work_item_ptr, struct serial_config, work_item);

    const struct device *dev = config->serial_dev;

    uint8_t *data_buffer;
    enum SerialType frame_type = config->serial_header.type;
    const struct client_callbacks *callbacks = serial_mux_get_callbacks(frame_type);

    /* Length-0 protocol info request: respond immediately, no payload to read. */
    if (frame_type == SERIAL_TYPE_PROTOCOL_INFO && config->serial_header.length == 0) {
        handle_protocol_info(config, config->serial_header.sequence_number);
        config->parsing_state = PARSING_STATE_TYPE;
        config->bytes_read = 0;
        config->serial_header.length = 0;
        return;
    }

    while (config->bytes_read < config->serial_header.length) {
        size_t buffer_size = config->serial_header.length - config->bytes_read;

        /* Protocol Info frames are handled inline — no client buffer needed. */
        if (frame_type == SERIAL_TYPE_PROTOCOL_INFO) {
            uint8_t discard[64];
            size_t to_read = MIN(buffer_size, sizeof(discard));
            int n = uart_fifo_read(dev, discard, to_read);
            if (n < 0) {
                LOG_ERR("UART FIFO read error: %d", n);
                break;
            }
            config->bytes_read += (size_t)n;
            if (config->bytes_read >= config->serial_header.length) {
                handle_protocol_info(config, config->serial_header.sequence_number);
                /* Reset parser for next frame; clear length so the loop
                 * exits instead of re-entering with a stale header.
                 */
                config->parsing_state = PARSING_STATE_TYPE;
                config->bytes_read = 0;
                config->serial_header.length = 0;
            }
            continue;
        }

        /* Validate client callback is wired for this traffic type. */
        if (frame_type >= SERIAL_TYPE_UNKNOWN || callbacks == NULL ||
            callbacks->acquire_buffer == NULL || callbacks->commit_data == NULL ||
            callbacks->message_complete == NULL) {
            LOG_ERR("No callback for type %d, dropping", frame_type);
            uint8_t discard[64];
            size_t to_read = MIN(buffer_size, sizeof(discard));
            int n = uart_fifo_read(dev, discard, to_read);
            if (n < 0) {
                break;
            }
            config->bytes_read += (size_t)n;
            if (config->bytes_read >= config->serial_header.length) {
                send_ack(dev, frame_type, config->serial_header.sequence_number,
                         ERROR_TYPE_LENGTH_TOO_LONG); /* reuse: unrecognised handler */
                config->parsing_state = PARSING_STATE_TYPE;
                config->bytes_read = 0;
            }
            continue;
        }

        if (!callbacks->acquire_buffer(&data_buffer, buffer_size)) {
            LOG_ERR("Buffer acquisition failed for type %d", frame_type);
            send_ack(dev, frame_type, config->serial_header.sequence_number,
                     ERROR_TYPE_OUT_OF_BUFFERS);
            config->parsing_state = PARSING_STATE_TYPE;
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
            if (config->rx_sequence_number != config->serial_header.sequence_number) {
                LOG_ERR("Incorrect sequence number: expected %u, received %u",
                        config->rx_sequence_number, config->serial_header.sequence_number);
                send_ack(dev, frame_type, config->rx_sequence_number,
                         ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER);
                /* Reset expected sequence to 0 per AGENTS spec. */
                config->rx_sequence_number = 0;
            } else {
                /* Sequence OK — commit data and notify client. */
                callbacks->commit_data(config->serial_header.length);
                callbacks->message_complete(config->serial_header.length);

                /* Advance and wrap rx sequence number. */
                config->rx_sequence_number =
                    (config->rx_sequence_number >= SEQUENCE_NUMBER_MAX) ? 0
                    : config->rx_sequence_number + 1;

                /* Send ACK to host unless no_ack was requested. */
                if (!config->serial_header.no_ack) {
                    send_ack(dev, frame_type,
                             config->serial_header.sequence_number,
                             ERROR_TYPE_NO_ERROR);
                }
            }

            /* Reset parser for next frame. */
            config->parsing_state = PARSING_STATE_TYPE;
            config->bytes_read = 0;
        } else {
            LOG_DBG("Read %d bytes, total: %zu/%u", bytes_read, config->bytes_read,
                    config->serial_header.length);
            callbacks->commit_data(bytes_read);
        }
    }

    uart_irq_rx_enable(dev);
}

void initialize_uart(struct serial_config *config)
{
    LOG_INF("Initializing UART...");
    const struct device *dev = config->serial_dev;

    config->parsing_state = PARSING_STATE_TYPE;
    config->bytes_read = 0;
    config->rx_sequence_number = 0;
    config->tx_sequence_number = 0;

    k_work_init(&config->work_item, serial_work_handler);

    uart_irq_callback_user_data_set(dev, uart_irq_callback, config);

    LOG_INF("About to enable interrupts");
    uart_irq_rx_enable(dev);

    LOG_INF("UART initialized and RX interrupts enabled");
}
