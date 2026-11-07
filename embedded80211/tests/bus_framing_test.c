/*
 * Unit tests for the host-side link-protocol framing primitives in
 * <embedded80211/bus.h>.
 *
 * These exercise the pure header encode/decode, sequence-wrap, and
 * validation functions (no hardware, no UART) and mirror the MockPort
 * unit tests in cli_test/src/protocol.rs.
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <string.h>

#include <embedded80211/serial.h>
#include <embedded80211/bus.h>

ZTEST_SUITE(bus_framing, NULL, NULL, NULL, NULL, NULL);

/* Header encode/decode round-trip: every header field must survive the
 * encode -> decode cycle, and the wire bytes must be big-endian length. */
ZTEST(bus_framing, test_header_roundtrip)
{
	struct bus_header h = {
		.type = SERIAL_TYPE_WIFI_MGMT,
		.no_ack = 1,
		.host = 1,
		.error = ERROR_TYPE_OUT_OF_BUFFERS,
		.reserved = 0,
		.seq = 11,
		.length = 0x1234,
	};
	uint8_t wire[4];
	struct bus_header d;

	bus_header_encode(&h, wire);
	bus_header_decode(wire, &d);

	zassert_equal(d.type, h.type, "type");
	zassert_equal(d.no_ack, h.no_ack, "no_ack");
	zassert_equal(d.host, h.host, "host");
	zassert_equal(d.error, h.error, "error");
	zassert_equal(d.reserved, h.reserved, "reserved");
	zassert_equal(d.seq, h.seq, "seq");
	zassert_equal(d.length, h.length, "length");

	/* Verify the exact wire layout (bit positions). */
	zassert_equal(wire[0],
		      (uint8_t)((h.error << 6) | (h.host << 5) |
				(h.no_ack << 4) | (h.type & 0x0F)),
		      "byte0 bit layout");
	zassert_equal(wire[1], (uint8_t)(h.seq << 4), "byte1 = seq<<4");
	zassert_equal(wire[2], 0x12, "length high byte (big-endian)");
	zassert_equal(wire[3], 0x34, "length low byte");
}

ZTEST(bus_framing, test_header_minimal)
{
	struct bus_header h = {
		.type = SERIAL_TYPE_WIFI_DATA,
		.no_ack = 0,
		.host = 0,
		.error = 0,
		.reserved = 0,
		.seq = 0,
		.length = 0,
	};
	uint8_t wire[4];

	bus_header_encode(&h, wire);

	zassert_equal(wire[0], 0x01, "type=1 in low nibble only");
	zassert_equal(wire[1], 0x00, "all zero");
	zassert_equal(wire[2], 0x00, "len hi");
	zassert_equal(wire[3], 0x00, "len lo");
}

/* Sequence number wraps 15 -> 0. */
ZTEST(bus_framing, test_seq_wrap)
{
	zassert_equal(bus_seq_next(0), 1, "0->1");
	zassert_equal(bus_seq_next(7), 8, "7->8");
	zassert_equal(bus_seq_next(14), 15, "14->15");
	zassert_equal(bus_seq_next(15), 0, "15->0 (wrap)");

	/* A full 0..15 cycle returns to 0. */
	uint8_t s = 0;

	for (int i = 0; i < 16; i++) {
		s = bus_seq_next(s);
	}
	zassert_equal(s, 0, "16 steps wraps to 0");
}

/* ACK frame encoding: length 0, echoed seq, error in header. */
ZTEST(bus_framing, test_ack_encoding)
{
	struct bus_header ack = {
		.type = SERIAL_TYPE_HCI,
		.no_ack = 0,
		.host = 1,           /* device->host ACK */
		.error = ERROR_TYPE_NO_ERROR,
		.reserved = 0,
		.seq = 5,
		.length = 0,
	};
	uint8_t wire[4];

	bus_header_encode(&ack, wire);

	zassert_equal(wire[0] & 0x0F, SERIAL_TYPE_HCI, "ack type");
	zassert_equal((wire[0] >> 5) & 0x1, 1, "host bit set");
	zassert_equal((wire[0] >> 4) & 0x1, 0, "no_ack clear");
	zassert_equal((wire[0] >> 6) & 0x3, ERROR_TYPE_NO_ERROR, "no error");
	zassert_equal((wire[1] >> 4) & 0x0F, 5, "echoed seq");
	zassert_equal(wire[2], 0, "len hi 0");
	zassert_equal(wire[3], 0, "len lo 0");
}

/* PROTOCOL_INFO response layout: [version, mask_lo, mask_hi, next_seq]. */
ZTEST(bus_framing, test_protocol_info_layout)
{
	uint8_t payload[4];
	uint16_t mask = 0x019F;

	payload[0] = SERIAL_PROTOCOL_VERSION;
	payload[1] = (uint8_t)(mask & 0xFF);
	payload[2] = (uint8_t)((mask >> 8) & 0xFF);
	payload[3] = 7;

	zassert_equal(payload[0], 1, "version");
	zassert_equal((uint16_t)(payload[1] | (payload[2] << 8)), mask,
		      "mask little-endian");
	zassert_equal(payload[3], 7, "next_seq");

	/* The response frame header is type=PROTOCOL_INFO, host=1, len=4. */
	struct bus_header hdr = {
		.type = SERIAL_TYPE_PROTOCOL_INFO,
		.no_ack = 0,
		.host = 1,
		.error = 0,
		.reserved = 0,
		.seq = 0,
		.length = 4,
	};
	uint8_t wire[4];

	bus_header_encode(&hdr, wire);
	zassert_equal(wire[0] & 0x0F, SERIAL_TYPE_PROTOCOL_INFO, "type");
	zassert_equal(wire[2], 0x00, "len hi");
	zassert_equal(wire[3], 0x04, "len lo");
}

/* TX validation: length 0 is only legal for PROTOCOL_INFO. */
ZTEST(bus_framing, test_tx_length_zero_rejection)
{
	zassert_equal(bus_validate_tx(SERIAL_TYPE_WIFI_MGMT, 0), -EINVAL,
		      "len 0 non-PROTO rejected");
	zassert_equal(bus_validate_tx(SERIAL_TYPE_WIFI_DATA, 0), -EINVAL,
		      "len 0 data rejected");
	zassert_equal(bus_validate_tx(SERIAL_TYPE_PROTOCOL_INFO, 0), 0,
		      "len 0 PROTO allowed");
	zassert_equal(bus_validate_tx(SERIAL_TYPE_WIFI_MGMT, 100), 0,
		      "normal len ok");
	zassert_equal(bus_validate_tx(SERIAL_TYPE_WIFI_MGMT,
				      SERIAL_MAX_PAYLOAD_LEN), 0,
		      "max len ok");
}

/* TX validation: oversize length is rejected. */
ZTEST(bus_framing, test_tx_oversize)
{
	zassert_equal(bus_validate_tx(SERIAL_TYPE_WIFI_MGMT,
				      SERIAL_MAX_PAYLOAD_LEN + 1),
		      -EMSGSIZE, "4097 rejected");
}

/* RX validation: reserved bits must be zero. */
ZTEST(bus_framing, test_rx_reserved_rejection)
{
	struct bus_header h = {
		.type = SERIAL_TYPE_WIFI_MGMT,
		.no_ack = 0,
		.host = 1,
		.error = 0,
		.reserved = 1,   /* illegal */
		.seq = 0,
		.length = 10,
	};

	zassert_equal(bus_validate_rx(&h), -EINVAL, "reserved bits set");

	h.reserved = 0;
	zassert_equal(bus_validate_rx(&h), 0, "reserved clear ok");
}

/* RX validation: unknown type is rejected. */
ZTEST(bus_framing, test_rx_unknown_type)
{
	struct bus_header h = {
		.type = SERIAL_TYPE_UNKNOWN,
		.no_ack = 0,
		.host = 1,
		.error = 0,
		.reserved = 0,
		.seq = 0,
		.length = 10,
	};

	zassert_equal(bus_validate_rx(&h), -EINVAL, "unknown type");

	h.type = SERIAL_TYPE_VSERIAL;
	zassert_equal(bus_validate_rx(&h), 0, "highest known type ok");
}

/* RX validation: oversize length is rejected. */
ZTEST(bus_framing, test_rx_oversize)
{
	struct bus_header h = {
		.type = SERIAL_TYPE_WIFI_DATA,
		.no_ack = 0,
		.host = 1,
		.error = 0,
		.reserved = 0,
		.seq = 0,
		.length = SERIAL_MAX_PAYLOAD_LEN + 1,
	};

	zassert_equal(bus_validate_rx(&h), -EMSGSIZE, "4097 rejected");

	h.length = SERIAL_MAX_PAYLOAD_LEN;
	zassert_equal(bus_validate_rx(&h), 0, "max len ok");
}

/* End-to-end: encode a host->device frame, decode the wire bytes, and
 * confirm the device-side parser would accept the same header. */
ZTEST(bus_framing, test_host_device_header_agreement)
{
	/* Host->device data frame. */
	struct bus_header h = {
		.type = SERIAL_TYPE_WIFI_MGMT,
		.no_ack = 0,
		.host = 0,
		.error = 0,
		.reserved = 0,
		.seq = 3,
		.length = 42,
	};
	uint8_t wire[4];
	struct bus_header d;

	bus_header_encode(&h, wire);
	bus_header_decode(wire, &d);

	zassert_equal(d.host, 0, "host bit clear for host->device");
	zassert_equal(d.seq, 3, "seq");
	zassert_equal(d.length, 42, "length");
	zassert_equal(bus_validate_rx(&d), 0, "device would accept header");
}
