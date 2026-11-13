/*
 * Host-side link-protocol bus client.
 *
 * Symmetric mirror of the device-side serial.c: the host is the client, the
 * device is the server. Frames the host transmits have the `host` bit clear
 * (host->device); frames the host receives have it set (device->host).
 *
 * The pure framing primitives below (bus_header_encode/decode, bus_seq_next,
 * bus_validate_*) carry no Zephyr dependency so they can be unit-tested
 * directly (see tests/bus_framing_test.c). The bus client API (bus_open,
 * bus_send, bus_request, ...) requires Zephyr + a UART bound via the DT
 * chosen `embedded80211-bus`.
 */
#ifndef _E80211_BUS_H_
#define _E80211_BUS_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

#include <embedded80211/serial.h>

/* RX payload buffer size (one full frame payload). */
#define E80211_BUS_RX_BUF_SIZE SERIAL_MAX_PAYLOAD_LEN

/*
 * Frame header, mirroring the device `struct SerialHeader` bit layout:
 *
 *   byte0: type(0..3) | no_ack(4) | host(5) | error(6..7)
 *   byte1: reserved(0..3, zero) | seq(4..7)
 *   byte2: payload length, high byte
 *   byte3: payload length, low byte
 *
 * `length` is kept in CPU endianness here; bus_header_encode/decode convert
 * to/from the big-endian wire order.
 */
struct bus_header {
	uint8_t type: 4;
	uint8_t no_ack: 1;
	uint8_t host: 1;
	uint8_t error: 2;
	uint8_t crc: 1;
	uint8_t reserved: 3;
	uint8_t seq: 4;
	uint16_t length;
};

/* ------------------------------------------------------------------------
 * Pure framing primitives (no Zephyr dependency; unit-testable).
 * ---------------------------------------------------------------------- */

/* Encode a header into 4 wire bytes (big-endian payload length). */
static inline void bus_header_encode(const struct bus_header *h, uint8_t out[4])
{
	out[0] = (uint8_t)((h->error << 6) | (h->host << 5) |
			  (h->no_ack << 4) | (h->type & 0x0F));
	out[1] = (uint8_t)((h->seq << 4) | (h->reserved & 0x0E) | (h->crc & 0x01));
	out[2] = (uint8_t)(h->length >> 8);
	out[3] = (uint8_t)(h->length & 0xFF);
}

/* Decode 4 wire bytes into a header. Always succeeds; validate separately. */
static inline void bus_header_decode(const uint8_t in[4], struct bus_header *h)
{
	h->type = in[0] & 0x0F;
	h->no_ack = (in[0] >> 4) & 0x1;
	h->host = (in[0] >> 5) & 0x1;
	h->error = (in[0] >> 6) & 0x3;
	h->crc = in[1] & 0x01;
	h->reserved = (in[1] >> 1) & 0x07;
	h->seq = (in[1] >> 4) & 0x0F;
	h->length = (uint16_t)(((uint16_t)in[2] << 8) | in[3]);
}

/* Next sequence number in the 0..15 space (wraps 15 -> 0). */
static inline uint8_t bus_seq_next(uint8_t seq)
{
	return (seq >= SEQUENCE_NUMBER_MAX) ? 0U : (uint8_t)(seq + 1U);
}

/*
 * Validate a host->device (TX) frame. Returns 0 if acceptable, negative
 * errno otherwise. Length 0 is only legal for a PROTOCOL_INFO query.
 */
static inline int bus_validate_tx(enum SerialType type, uint16_t len)
{
	if (len > SERIAL_MAX_PAYLOAD_LEN) {
		return -EMSGSIZE;
	}
	if (len == 0U && type != SERIAL_TYPE_PROTOCOL_INFO) {
		return -EINVAL;
	}
	return 0;
}

/*
 * Validate a device->host (RX) frame header. Returns 0 if acceptable,
 * negative errno otherwise. Checks reserved bits are zero, the type is known,
 * and the length is in range.
 */
static inline int bus_validate_rx(const struct bus_header *h)
{
	if (h->reserved != 0U) {
		return -EINVAL;
	}
	if (h->type >= SERIAL_TYPE_UNKNOWN) {
		return -EINVAL;
	}
	if (h->length > SERIAL_MAX_PAYLOAD_LEN) {
		return -EMSGSIZE;
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * Bus client API (requires Zephyr + a UART bound via DT chosen).
 * ---------------------------------------------------------------------- */

struct e80211_bus_config {
	const struct device *uart_dev;
	uint8_t rx_buf[E80211_BUS_RX_BUF_SIZE];
};

/* Per-traffic-type receive handler. Invoked from the bus work context with
 * the frame payload; `user` is the value registered via bus_set_handler_user.
 */
typedef void (*bus_rx_handler_t)(const uint8_t *payload, size_t len, void *user);

/* Open the bus: bind the UART, enable RX interrupts, and complete the
 * PROTOCOL_INFO handshake (verifies version, stores the supported-types mask
 * and the device's next expected sequence). Returns 0 on success. */
int bus_open(void);

/* Register a receive handler for a traffic type (WIFI_DATA / WIFI_MGMT /
 * HCI / ...). Returns 0 on success, -EINVAL for an unknown type. */
int bus_register_handler(int type, bus_rx_handler_t cb);

/* Attach a per-type user pointer passed to the handler. */
void bus_set_handler_user(int type, void *user);

/* Send a host->device data frame (no_ack=0). Returns 0 on success. */
int bus_send(int type, const uint8_t *payload, size_t len);

/*
 * Enable or disable the per-frame CRC32C feature on the host bus. When
 * enabled, frames the host transmits set the `crc` header bit and append a
 * 4-byte CRC32C trailer, and received frames carrying the bit are verified
 * (a mismatch drops the frame). Off by default, so the wire format is
 * byte-identical to protocol v1 until both peers opt in.
 */
void bus_set_crc_enabled(bool enabled);

/*
 * Send a request frame and wait for the matching device->host response.
 * `resp`/`*resp_len` receive the response payload. On an INCORRECT_SEQUENCE
 * number error ACK the bus re-syncs via PROTOCOL_INFO and retries exactly
 * once (reference behavior). Returns 0 on success, -ETIMEDOUT, or a
 * device-reported error.
 */
int bus_request(int type, const uint8_t *req, size_t req_len,
		uint8_t *resp, size_t *resp_len, uint32_t timeout_ms);

/* The device's supported-traffic-types mask, as learned at bus_open(). */
uint16_t bus_supported_types(void);

#endif /* _E80211_BUS_H_ */
