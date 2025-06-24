#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>

#include "serial.h"

LOG_MODULE_REGISTER(serial, LOG_LEVEL_INF);

static struct client_callbacks client_cb[SERIAL_TYPE_UNKNOWN];
K_MUTEX_DEFINE(serial_tx_mutex);

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
                config->parsing_state = PARSING_STATE_CRC;
                config->bytes_read = 0; // Reset bytes read for new packet
            } else {
                LOG_ERR("Failed to read Serial Type");
            }
            break;
        case PARSING_STATE_CRC:
            if (uart_fifo_read(dev, (uint8_t*)&config->serial_header.crc, 1) == 1) {
                LOG_DBG("Received Serial CRC: %u", config->serial_header.crc);
                config->parsing_state = PARSING_STATE_LENGTH1;
            } else {
                LOG_ERR("Failed to read Serial CRC");
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
                config->serial_header.length = sys_be16_to_cpu(config->serial_header.length); // Convert to host byte order
                if (config->serial_header.length > 0) {
                    config->parsing_state = PARSING_STATE_DATA;
                } else {
                    LOG_ERR("Invalid Serial Length: %u", config->serial_header.length);
                    config->parsing_state = PARSING_STATE_TYPE; // Reset to start
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

static void serial_work_handler(struct k_work *work_item_ptr)
{
    struct serial_config *config =
        CONTAINER_OF(work_item_ptr, struct serial_config, work_item);

    const struct device *dev = config->serial_dev;

    // 1. Acquire technology specific buffer
    // 2. Read data from UART into the buffer
    // 3. Commit changes once all data is read
    // 4a. If not all data is received, exit and wait for next interrupt
    // 4b. If all data is received, process the buffer signaling to the
    //receive technology that the buffer is ready

    uint8_t* data_buffer;

    while (config->bytes_read < config->serial_header.length) {
        size_t buffer_size = config->serial_header.length - config->bytes_read;

        client_cb[config->type].acquire_buffer(&data_buffer, buffer_size);

        // Read data from UART into the buffer
        int bytes_read = uart_fifo_read(dev, data_buffer + config->bytes_read, buffer_size);
        if (bytes_read < 0) {
            LOG_ERR("UART FIFO read error: %d", bytes_read);
            break;
        } else if (bytes_read == 0) {
            LOG_DBG("No more data available, waiting for next interrupt");
            break;
        } else {
            config->bytes_read += bytes_read;

            if (config->bytes_read >= config->serial_header.length) {
                // All data for this packet has been read
                LOG_INF("Received complete packet of length %u", config->serial_header.length);

                // Process the received data here
                client_cb[config->type].commit_data(bytes_read); // Complete the packet
                client_cb[config->type].message_complete(config->serial_header.length);
                // Reset for next packet
                config->parsing_state = PARSING_STATE_TYPE;
                config->type = SERIAL_TYPE_UNKNOWN; // Reset type
                config->bytes_read = 0;

            } else {
                LOG_DBG("Read %d bytes, total bytes read: %zu", bytes_read, config->bytes_read);
                client_cb[config->type].commit_data(bytes_read); // Not complete yet
            }
        }
    }

    uart_irq_rx_enable(dev);
}

void initialize_uart(struct serial_config *config)
{
    LOG_INF("Initializing UART...");
    const struct device *dev = config->serial_dev;

    k_work_init(&config->work_item, serial_work_handler); // Initialize work item, no handler set yet

    // Set the interrupt callback function
    uart_irq_callback_user_data_set(dev, uart_irq_callback, config);

    LOG_INF("About to enable interrupts");
    // Enable RX interrupts
    uart_irq_rx_enable(dev);

    LOG_INF("UART initialized and RX interrupts enabled");
}
