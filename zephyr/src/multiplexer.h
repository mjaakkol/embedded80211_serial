#ifndef _MULTIPLEXER_H_
#define _MULTIPLEXER_H_

#include <zephyr/device.h>
#include <stdint.h>

#include "serial/serial.h"

enum serial_mux_iface_id {
    SERIAL_MUX_IFACE_UART = 0,
    SERIAL_MUX_IFACE_USB,
    SERIAL_MUX_IFACE_COUNT,
};

int serial_mux_register_interface(enum serial_mux_iface_id iface_id,
                                  const struct device *dev,
                                  struct serial_config *config);

const struct client_callbacks *serial_mux_get_callbacks(enum SerialType type);

uint16_t serial_mux_supported_types_mask(void);

int serial_mux_send(enum SerialType type, const uint8_t *payload, size_t len);

#endif /* _MULTIPLEXER_H_ */
