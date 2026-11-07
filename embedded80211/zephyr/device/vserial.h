#ifndef _VSERIAL_H_
#define _VSERIAL_H_

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

/**
 * @brief Acquire buffer for outgoing virtual serial data
 * @param data Pointer to receive buffer address
 * @param len Requested length
 * @return true if buffer acquired, false if too large or unavailable
 */
bool vserial_acquire_buffer(uint8_t **data, size_t len);

/**
 * @brief Mark data committed to buffer (does nothing, data is already in buffer)
 * @param len Length of data
 */
void vserial_commit_data(size_t len);

/**
 * @brief Process complete incoming virtual serial message
 * @param len Length of message payload
 */
void vserial_message_complete(size_t len);

#endif /* _VSERIAL_H_ */
