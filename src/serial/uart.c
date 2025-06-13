#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include "serial.h"

LOG_MODULE_REGISTER(serial_uart);

#define UART_DEVICE_NODE DT_NODELABEL(uart0)

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

static uint8_t rx_buffer_data[CONFIG_UART_RX_BUFFER_SIZE];
static struct ring_buf rx_ringbuf;


int  init_uart(void)
{
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not found or not ready!");
        return -ENODEV;
    }

    ring_buf_init(&rx_ringbuf, sizeof(rx_buffer_data), rx_buffer_data);

    initialize_uart(uart_dev, &rx_ringbuf);

    LOG_INF("UART initialized successfully");

    return 0;
}

SYS_INIT(init_uart, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);