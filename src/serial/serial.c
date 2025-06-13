#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/logging.h>

#include "serial.h"

LOG_MODULE_REGISTER(serial, LOG_LEVEL_DBG);


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
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(dev, data[i]);
    }

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
    ARG_UNUSED(user_data);

    struct ring_buf *rx_ringbuf = (struct ring_buf *)user_data;

    // Ensure the interrupt is for our UART device
    if (dev != uart_dev) {
        return;
    }

    // Check if the interrupt is for received data
    while (uart_irq_is_pending(dev) && uart_irq_rx_ready(dev)) {
        uint8_t rx_data[128]; // Temporary buffer to read from FIFO
        int bytes_read;

        bytes_read = uart_fifo_read(dev, rx_data, sizeof(rx_data));
        if (bytes_read < 0) {
            // Should not happen if uart_irq_rx_ready() is true, but good practice
            LOG_ERR("UART FIFO read error: %d\n", bytes_read);
            break;
        }
        if (bytes_read > 0) {
            int bytes_written = ring_buf_put(rx_data, bytes_read);
            if (bytes_written < bytes_read) {
                printk("RX Ring buffer full, %d bytes dropped!\n", bytes_read - bytes_written);
            }
            // Optionally, signal a thread here that data is available
            // For example, using k_sem_give() or k_work_submit()
        }
    }
}

void initialize_uart(const struct device *dev, struct ring_buf *rx_ringbuf)
{
    if (!device_is_ready(dev)) {
        printk("UART device not found or not ready!\n");
        return;
    }

    // Set the interrupt callback function
    uart_irq_callback_user_data_set(dev, uart_irq_callback, rx_ringbuf);

    // Enable RX interrupts
    uart_irq_rx_enable(dev);

    printk("UART initialized and RX interrupts enabled");
}