#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include "serial.h"

LOG_MODULE_REGISTER(serial_uart);

#define UART_DEVICE_NODE DT_NODELABEL(uart0)

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

static uint8_t rx_buffer[CONFIG_UART_RX_BUFFER_SIZE];

struct serial_config config_serial_uart = {
    .serial_dev = (const struct device *)uart_dev,
    .serial_tx = NULL,
    .serial_rx = NULL, // This will be set later
};

int  init_uart(void)
{
    LOG_INF("Initializing UART...");
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not found or not ready!");
        return -ENODEV;
    }

    ring_buf_init(&config_serial_uart.rx_ringbuf, sizeof(rx_buffer), rx_buffer);

    initialize_uart(&config_serial_uart);

    LOG_INF("UART initialized successfully");

    return 0;
}

SYS_INIT(init_uart, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);