#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include "serial.h"
#include "../wifi_mgmt_proxy.h"
#include "../net_if_mgmt_proxy.h"

LOG_MODULE_REGISTER(serial_usb);

 #define UART_DEVICE_NODE DT_NODELABEL(cdc_serial_uart)

const struct device *const cdc_acm_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

static bool usb_connected = false;
static struct serial_config config_serial = {
    .serial_dev = (const struct device*) cdc_acm_dev,
};

static uint8_t rx_buffer[CONFIG_UART_RX_BUFFER_SIZE];

static int init_serial_usb() {
    if (!device_is_ready((const struct device*)cdc_acm_dev)) {
        LOG_ERR("CDC ACM device not found or not ready!");
        return -ENODEV;
    }

    /* USB device stack next is initialized/enabled at boot by
     * CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT.
     */
    usb_connected = true;
    LOG_INF("USB CDC ACM backend is initialized at boot");

    ring_buf_init(&config_serial.rx_ringbuf, sizeof(rx_buffer), rx_buffer);

    initialize_uart(&config_serial);

    wifi_mgmt_proxy_set_device((const struct device *)cdc_acm_dev);
    net_if_mgmt_proxy_set_device((const struct device *)cdc_acm_dev);

    LOG_INF("USB CDC ACM initialized successfully");
    return 0;
}

SYS_INIT(init_serial_usb, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);