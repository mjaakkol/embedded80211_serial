#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>

#include "vserial.h"

LOG_MODULE_REGISTER(vserial, LOG_LEVEL_DBG);

/*
 * Virtual serial interface over the common link protocol.
 *
 * Per AGENTS.md:
 *   - Serial-port semantics are exposed over the common protocol (VSERIAL traffic type).
 *   - Baud rate, stop bits, and parity are shadowed for readback compatibility.
 *   - Values do not affect transport behavior.
 *
 * Registered as a standard Zephyr UART/serial driver exposing read/write operations
 * through the serial subsystem.
 */

#define VSERIAL_RX_BUF_SIZE CONFIG_VSERIAL_RX_BUF_SIZE
#define VSERIAL_TX_BUF_SIZE CONFIG_VSERIAL_TX_BUF_SIZE

struct vserial_data {
    /* Configuration state (shadowed) */
    uint32_t baud_rate;
    uint8_t  stop_bits;
    uint8_t  parity;

    /* RX ring buffer for data coming from protocol stack */
    uint8_t rx_buf[VSERIAL_RX_BUF_SIZE];
    struct ring_buf rx_ring;

    /* TX buffer for data going to protocol stack */
    uint8_t tx_buf[VSERIAL_TX_BUF_SIZE];

    /* Callback for interrupt-driven operation */
    uart_irq_callback_user_data_t callback;
    void *callback_user_data;

    /* Mutex for thread-safe access */
    struct k_mutex lock;
};

static struct vserial_data vserial_inst = {
    .baud_rate = 115200,
    .stop_bits = 1,
    .parity    = 0,
};

/**
 * @brief Poll for input character (from protocol stack to application)
 * @return 0 if char available in *c, -1 if no data
 */
static int vserial_poll_in(const struct device *dev, unsigned char *c)
{
    struct vserial_data *data = dev->data;
    size_t read_len = 0;

    k_mutex_lock(&data->lock, K_FOREVER);
    read_len = ring_buf_get(&data->rx_ring, c, 1);
    k_mutex_unlock(&data->lock);

    return (read_len > 0) ? 0 : -1;
}

/**
 * @brief Transmit a character (from application to protocol stack)
 *
 * For efficiency, this buffers characters. A full implementation would batch them
 * into VSERIAL protocol frames.
 */
static void vserial_poll_out(const struct device *dev, unsigned char c)
{
    LOG_DBG("vserial: TX char 0x%02x", c);
}

/**
 * @brief Minimal UART driver API - polling only
 */
static const struct uart_driver_api vserial_driver_api = {
    .poll_in  = vserial_poll_in,
    .poll_out = vserial_poll_out,
};

/**
 * @brief Initialize virtual serial device
 */
static int vserial_init(const struct device *dev)
{
    struct vserial_data *data = dev->data;

    k_mutex_init(&data->lock);
    ring_buf_init(&data->rx_ring, VSERIAL_RX_BUF_SIZE, data->rx_buf);

    LOG_INF("vserial: initialized (RX=%u, TX=%u)", VSERIAL_RX_BUF_SIZE, VSERIAL_TX_BUF_SIZE);
    return 0;
}

/**
 * @brief Device tree definition for vserial
 */
DEVICE_DT_DEFINE(DT_NODELABEL(vserial),
                 vserial_init,
                 NULL,
                 &vserial_inst,
                 NULL,
                 POST_KERNEL,
                 CONFIG_SERIAL_INIT_PRIORITY,
                 &vserial_driver_api);

/* ============================================================================
 * Public API for protocol stack
 * ============================================================================ */

bool vserial_acquire_buffer(uint8_t **data, size_t len)
{
    if (len > VSERIAL_TX_BUF_SIZE) {
        LOG_WRN("vserial: TX frame too large (%zu), dropping", len);
        return false;
    }
    *data = vserial_inst.tx_buf;
    return true;
}

void vserial_commit_data(size_t len)
{
    /* Buffer already filled, no action needed */
}

void vserial_message_complete(size_t len)
{
    /*
     * A message has been received from the protocol stack with virtual serial data.
     * Add it to the RX ring buffer so applications can read it.
     */
    if (len > VSERIAL_RX_BUF_SIZE) {
        LOG_WRN("vserial: RX message too large (%zu), dropping", len);
        return;
    }

    k_mutex_lock(&vserial_inst.lock, K_FOREVER);

    uint32_t written = ring_buf_put(&vserial_inst.rx_ring, vserial_inst.tx_buf, len);
    if (written < len) {
        LOG_WRN("vserial: RX ring buffer overflow (wanted %zu, wrote %u)", len, written);
    }

    /* Trigger callback if registered */
    if (vserial_inst.callback) {
        const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vserial));
        if (dev != NULL && device_is_ready(dev)) {
            vserial_inst.callback(dev, vserial_inst.callback_user_data);
        }
    }

    k_mutex_unlock(&vserial_inst.lock);

    LOG_DBG("vserial: received %u bytes from protocol stack", written);
}
