#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include "serial.h"

#define UART_DEVICE_NODE DT_NODELABEL(uart0)

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

static uint8_t rx_buffer_data[CONFIG_UART_BUFFER_SIZE];
static struct ring_buf rx_ringbuf;


void init_uart(void)
{
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not found or not ready!\n");
        return;
    }

    initialize_uart(uart_dev, &rx_ringbuf);

    LOG_INF("UART initialized successfully\n");
}

SYS_INIT(init_uart, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);