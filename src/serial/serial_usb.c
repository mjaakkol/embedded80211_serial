#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/logging/log.h>

#include "serial.h"

LOG_MODULE_REGISTER(serial_usb);

 #define UART_DEVICE_NODE DT_NODELABEL(cdc_serial_uart)

const struct device *const cdc_acm_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

static bool usb_connected = false;
static struct serial_config config_serial = {
    .serial_dev = (const struct device*) cdc_acm_dev,
    .serial_tx = NULL,
    .serial_rx = NULL, // This will be set later
};

static uint8_t rx_buffer[CONFIG_UART_RX_BUFFER_SIZE];

// Only define this callback if USB CDC ACM is not the console
// and if the USB stack is enabled.
static void usb_status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
    ARG_UNUSED(param);

    switch (status) {
    case USB_DC_CONFIGURED:
        LOG_WRN("USB device configured\n");
        usb_connected = true;
        // You might want to signal your main loop that it can start UART communication
        break;
    case USB_DC_RESET:
        LOG_WRN("USB device reset\n");
        usb_connected = false;
        break;
    case USB_DC_SUSPEND:
        LOG_WRN("USB device suspended\n");
        usb_connected = false;
        break;
    case USB_DC_RESUME:
        LOG_WRN("USB device resumed\n");
        // Check if still configured
        // TODO: This needs to be refactored per the latest Zephyr USB API changes
        ///if (usb_get_status(NULL) == USB_DC_CONFIGURED) {
        //    usb_connected = true;
        //}
        break;
    default:
        break;
    }
}

static int init_serial_usb() {
    if (!device_is_ready((const struct device*)cdc_acm_dev)) {
        LOG_ERR("CDC ACM device not found or not ready!");
        return -ENODEV;
    }

    int ret;
    // Initialize USB. If CDC ACM is console, this might be done earlier by the system.
    // If not console, explicitly initialize.

    ret = usb_enable(usb_status_cb); // Pass status callback if not console
    if (ret != 0) {
        LOG_ERR("Failed to enable USB: %d\n", ret);
        return -ENODEV;
    }
    LOG_INF("USB enabled. Waiting for host connection...");

    ring_buf_init(&config_serial.rx_ringbuf, sizeof(rx_buffer), rx_buffer);

    initialize_uart(&config_serial);

    LOG_INF("USB CDC ACM initialized successfully");
    return 0;
}

SYS_INIT(init_serial_usb, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);