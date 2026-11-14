/*
 * Host-side link-protocol bus client.
 *
 * Async mirror of the device-side serial.c. The host is the client: frames it
 * transmits clear the `host` bit (host->device), frames it receives set it
 * (device->host). The RX ISR parses the header and drains the payload out of
 * the UART FIFO; a single bus work item then validates and dispatches the
 * frame to a per-type handler or wakes a pending bus_request().
 *
 * Request/response semantics mirror cli_test/src/protocol.rs (Session::request):
 * drain-before-send, PROTOCOL_INFO resync consumes no sequence slot, and an
 * INCORRECT_SEQUENCE_NUMBER or LENGTH_TOO_LONG (CRC mismatch) error ACK
 * triggers exactly one resync+retry.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

#include <embedded80211/serial.h>
#include <embedded80211/bus.h>

LOG_MODULE_REGISTER(bus, LOG_LEVEL_INF);

#define BUS_TX_TIMEOUT_MS 500
#define BUS_SYNC_RETRIES 4
#define BUS_SYNC_TIMEOUT_MS 500

#if DT_NODE_EXISTS(DT_CHOSEN(embedded80211_bus))
#define BUS_UART DT_CHOSEN(embedded80211_bus)
#else
#define BUS_UART -1
#endif

struct bus_state {
	const struct device *uart;
	/* RX state machine (header + payload drained in ISR context). */
	enum ParsingState state;
	struct bus_header hdr;
	size_t bytes_read;
	/* Raw 4 header wire bytes + CRC32C trailer of the frame being parsed. */
	uint8_t hdr_wire[4];
	uint8_t trailer[SERIAL_CRC_LEN];
	size_t trailer_read;
	uint8_t rx_buf[E80211_BUS_RX_BUF_SIZE];
	struct k_work rx_work;
	/* Sequence counters. tx_seq: host->device. rx_seq: device->host (the
	 * host does not sequence-check device->host frames; see note below). */
	uint8_t tx_seq;
	uint8_t rx_seq;
	/* Request/response handshake. */
	struct k_sem rx_sem;
	struct k_mutex tx_mutex;
	volatile bool request_pending;
	struct bus_header last_hdr;
	/* Learned at bus_open(). */
	uint16_t supported_types;
	bus_rx_handler_t handler[SERIAL_TYPE_UNKNOWN];
	void *handler_user[SERIAL_TYPE_UNKNOWN];
	bool opened;
	/* Runtime per-frame CRC32C feature (off by default). */
	bool crc_enabled;
};

static struct bus_state bus = {
	.rx_sem = Z_SEM_INITIALIZER(bus.rx_sem, 0, 1),
	.tx_mutex = Z_MUTEX_INITIALIZER(bus.tx_mutex),
};

/*
 * Write a block to the UART with backpressure. Mirrors serial_data_tx(). A
 * healthy device drains the pipe fast; the bound guards against a wedged
 * device, where the FIFO never drains.
 */
static int bus_data_tx(const struct device *dev, const uint8_t *data, size_t len)
{
	size_t written = 0;
	int64_t start = k_uptime_get();

	while (written < len) {
		int n = uart_fifo_fill(dev, data + written, len - written);

		if (n < 0) {
			return n;
		}
		if (n > 0) {
			written += (size_t)n;
			continue;
		}
		if (k_uptime_get() - start > BUS_TX_TIMEOUT_MS) {
			LOG_ERR("%u of %u TX bytes unsent (TX FIFO full %d ms)",
				(unsigned int)written, (unsigned int)len, BUS_TX_TIMEOUT_MS);
			return -ENOSPC;
		}
		k_busy_wait(10);
	}
	return 0;
}

static void bus_send_raw(const struct bus_header *hdr, const uint8_t *payload,
			 size_t len)
{
	struct bus_header h = *hdr;
	uint8_t wire[4];

	if (bus.crc_enabled) {
		h.crc = 1;
	}
	bus_header_encode(&h, wire);
	k_mutex_lock(&bus.tx_mutex, K_FOREVER);
	bus_data_tx(bus.uart, wire, sizeof(wire));
	if (payload != NULL && len > 0U) {
		bus_data_tx(bus.uart, payload, len);
	}
	if (bus.crc_enabled) {
		uint32_t crc = serial_frame_crc(wire, payload, len);
		uint8_t trailer[SERIAL_CRC_LEN] = {
			(uint8_t)(crc >> 24), (uint8_t)(crc >> 16),
			(uint8_t)(crc >> 8),  (uint8_t)(crc),
		};

		bus_data_tx(bus.uart, trailer, sizeof(trailer));
	}
	k_mutex_unlock(&bus.tx_mutex);
}

static void bus_send_ack(enum SerialType type, uint8_t seq, enum ErrorType error)
{
	struct bus_header ack = {
		.type = type,
		.no_ack = false,
		.host = false,
		.error = error,
		.reserved = 0,
		.seq = seq,
		.length = 0,
	};

	bus_send_raw(&ack, NULL, 0);
}

/*
 * UART RX interrupt callback. Reads header bytes (4) one at a time from the
 * FIFO and advances the parse state machine, then drains the payload (and
 * CRC32C trailer, if requested) out of the FIFO as well.
 *
 * Draining happens here, in ISR context, and not in the work item: the RX
 * interrupt is level-triggered on the FIFO level. If the ISR left a full
 * payload in the FIFO, the interrupt would stay asserted and re-enter this
 * handler forever, starving the system workqueue that is supposed to drain
 * the FIFO — a hard wedge. Byte-at-a-time serial hardware hides this (the
 * FIFO level falls between byte arrivals); emulators that deliver a whole
 * frame at once expose it.
 */
static void bus_rx_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t b;

	while (bus.state != PARSING_STATE_DATA) {
		if (uart_fifo_read(dev, &b, 1) != 1) {
			break;
		}

		switch (bus.state) {
		case PARSING_STATE_TYPE:
			bus.hdr.type = b & 0x0F;
			bus.hdr.no_ack = (b >> 4) & 0x1;
			bus.hdr.host = (b >> 5) & 0x1;
			bus.hdr.error = (b >> 6) & 0x3;
			bus.hdr_wire[0] = b;
			bus.bytes_read = 0;
			bus.state = PARSING_STATE_SEQUENCE_NUMBER;
			break;
		case PARSING_STATE_SEQUENCE_NUMBER:
			bus.hdr.crc = b & 0x01;
			bus.hdr.reserved = (b >> 1) & 0x07;
			bus.hdr.seq = (b >> 4) & 0x0F;
			bus.hdr_wire[1] = b;
			bus.state = PARSING_STATE_LENGTH1;
			break;
		case PARSING_STATE_LENGTH1:
			bus.hdr.length = (uint16_t)(b << 8);
			bus.hdr_wire[2] = b;
			bus.state = PARSING_STATE_LENGTH2;
			break;
		case PARSING_STATE_LENGTH2:
			bus.hdr.length |= b;
			bus.hdr_wire[3] = b;
			bus.trailer_read = 0;
			if (bus.hdr.length == 0U) {
				/*
				 * Length 0: an ACK (any type) or a PROTOCOL_INFO
				 * query. Both complete the header; the payload
				 * stage is a no-op.
				 */
				bus.state = PARSING_STATE_DATA;
			} else if (bus.hdr.length > SERIAL_MAX_PAYLOAD_LEN) {
				LOG_ERR("Payload too long: %u > %u", bus.hdr.length,
					SERIAL_MAX_PAYLOAD_LEN);
				bus_send_ack(bus.hdr.type, bus.hdr.seq,
						ERROR_TYPE_LENGTH_TOO_LONG);
				bus.state = PARSING_STATE_TYPE;
			} else {
				bus.state = PARSING_STATE_DATA;
			}
			break;
		default:
			bus.state = PARSING_STATE_TYPE;
			break;
		}
	}

	if (bus.state != PARSING_STATE_DATA) {
		return;
	}

	/* Drain payload + trailer until the FIFO is empty or the frame is
	 * complete.
	 */
	const size_t total = bus.hdr.length +
		((size_t)bus.hdr.crc * SERIAL_CRC_LEN);

	while (bus.bytes_read + bus.trailer_read < total) {
		uint8_t *dst;
		size_t remaining;
		int n;

		if (bus.bytes_read < bus.hdr.length) {
			dst = bus.rx_buf + bus.bytes_read;
			remaining = bus.hdr.length - bus.bytes_read;
		} else {
			dst = bus.trailer + bus.trailer_read;
			remaining = SERIAL_CRC_LEN - bus.trailer_read;
		}

		n = uart_fifo_read(dev, dst, remaining);
		if (n <= 0) {
			/* FIFO empty: frame continues on the next RX event. */
			break;
		}
		if (bus.bytes_read < bus.hdr.length) {
			bus.bytes_read += (size_t)n;
		} else {
			bus.trailer_read += (size_t)n;
		}
	}

	if (bus.bytes_read + bus.trailer_read == total) {
		(void)k_work_submit(&bus.rx_work);
	}
}

/*
 * Bus work item: dispatch a fully-drained frame or wake the requester.
 * The ISR has already moved the header, payload, and trailer out of the
 * UART FIFO (see bus_rx_isr); this runs in thread context and only
 * validates and dispatches.
 *
 * The host does NOT sequence-check device->host frames. The device is the
 * authority for host->device sequencing; the device->host stream is read in
 * order. (Sequence-checking here would desync after the PROTOCOL_INFO
 * handshake, whose response advances the device's TX counter. This matches
 * the reference host in cli_test/src/protocol.rs, which never checks the
 * device->host sequence.)
 */
static void bus_rx_work(struct k_work *work)
{
	ARG_UNUSED(work);
	enum SerialType type = bus.hdr.type;
	const size_t length = bus.hdr.length;
	const bool has_crc = bus.hdr.crc != 0;

	/* Validate the header. */
	if (bus_validate_rx(&bus.hdr) != 0) {
		LOG_ERR("Dropping invalid frame (type %u len %u)",
			bus.hdr.type, bus.hdr.length);
		bus.state = PARSING_STATE_TYPE;
		bus.bytes_read = 0;
		bus.trailer_read = 0;
		return;
	}

	/* Verify the CRC32C trailer when requested. A mismatch drops the
	 * frame without dispatching or acknowledging it. */
	if (has_crc) {
		uint32_t expected = ((uint32_t)bus.trailer[0] << 24) |
				    ((uint32_t)bus.trailer[1] << 16) |
				    ((uint32_t)bus.trailer[2] << 8) |
				    (uint32_t)bus.trailer[3];
		uint32_t actual = serial_frame_crc(bus.hdr_wire, bus.rx_buf, length);

		if (actual != expected) {
			LOG_ERR("CRC32C mismatch (type %u len %u): expected 0x%08x, "
				"got 0x%08x", type, (unsigned int)length,
				(unsigned int)expected, (unsigned int)actual);
			bus.state = PARSING_STATE_TYPE;
			bus.bytes_read = 0;
			bus.trailer_read = 0;
			return;
		}
	}

	if (bus.hdr.length == 0U) {
		/* ACK from the device. */
		if (bus.request_pending) {
			bus.last_hdr = bus.hdr;
			k_sem_give(&bus.rx_sem);
		}
	} else {
		/* Data frame from the device. Acknowledge it (the device
		 * discards host ACKs; this keeps the link symmetric). */
		if (!bus.hdr.no_ack) {
			bus_send_ack(bus.hdr.type, bus.hdr.seq, ERROR_TYPE_NO_ERROR);
		}
		if (bus.request_pending) {
			bus.last_hdr = bus.hdr;
			k_sem_give(&bus.rx_sem);
		} else {
			bus_rx_handler_t cb = bus.handler[type];

			if (cb != NULL) {
				cb(bus.rx_buf, bus.hdr.length, bus.handler_user[type]);
			}
		}
	}

	bus.state = PARSING_STATE_TYPE;
	bus.bytes_read = 0;
	bus.trailer_read = 0;
}

static int bus_sync(void)
{
	for (int attempt = 0; attempt < BUS_SYNC_RETRIES; attempt++) {
		struct bus_header query = {
			.type = SERIAL_TYPE_PROTOCOL_INFO,
			.no_ack = true,
			.host = false,
			.error = 0,
			.reserved = 0,
			.seq = bus.tx_seq,
			.length = 0,
		};

		/* Discard any stale frame so the handshake reads a fresh
		 * response. */
		while (k_sem_take(&bus.rx_sem, K_NO_WAIT) == 0) {
			;
		}

		bus.request_pending = true;
		bus_send_raw(&query, NULL, 0);
		/* PROTOCOL_INFO consumes no sequence slot: tx_seq unchanged. */

		if (k_sem_take(&bus.rx_sem, K_MSEC(BUS_SYNC_TIMEOUT_MS)) != 0) {
			bus.request_pending = false;
			continue;
		}

		if (bus.last_hdr.type == SERIAL_TYPE_PROTOCOL_INFO &&
		    bus.last_hdr.length == 4U) {
			const uint8_t *p = bus.rx_buf;

			bus.request_pending = false;
			if (p[0] != SERIAL_PROTOCOL_VERSION) {
				LOG_ERR("Protocol version mismatch: %u (want %u)",
					p[0], SERIAL_PROTOCOL_VERSION);
				return -ENOTSUP;
			}
			bus.supported_types = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
			bus.tx_seq = p[3];
			LOG_INF("Bus synced: version %u, types 0x%04x, next seq %u",
				p[0], bus.supported_types, p[3]);
			return 0;
		}
		bus.request_pending = false;
	}
	return -ETIMEDOUT;
}

int bus_open(void)
{
	if (bus.opened) {
		return 0;
	}

#if DT_NODE_EXISTS(DT_CHOSEN(embedded80211_bus))
	bus.uart = DEVICE_DT_GET(BUS_UART);
#else
	LOG_ERR("No embedded80211-bus UART chosen in device tree");
	return -ENODEV;
#endif

	if (!device_is_ready(bus.uart)) {
		LOG_ERR("Bus UART not ready");
		return -ENODEV;
	}

	bus.state = PARSING_STATE_TYPE;
	bus.bytes_read = 0;
	bus.rx_seq = 0;
	bus.tx_seq = 0;

	k_work_init(&bus.rx_work, bus_rx_work);
	/* rx_sem / tx_mutex are statically initialized. */

	uart_irq_callback_user_data_set(bus.uart, bus_rx_isr, NULL);
	uart_irq_rx_enable(bus.uart);

	int rc = bus_sync();

	/*
	 * Mark opened only on a completed handshake. virt_wifi / virt_bt call
	 * bus_open() from device init, before main(); if that first sync fails
	 * (device not up yet), a later bus_open() must retry the handshake
	 * instead of returning a cached success with supported_types == 0.
	 */
	if (rc == 0) {
		bus.opened = true;
	}
	return rc;
}

int bus_register_handler(int type, bus_rx_handler_t cb)
{
	if (type < 0 || type >= SERIAL_TYPE_UNKNOWN) {
		return -EINVAL;
	}
	bus.handler[type] = cb;
	return 0;
}

void bus_set_handler_user(int type, void *user)
{
	if (type >= 0 && type < SERIAL_TYPE_UNKNOWN) {
		bus.handler_user[type] = user;
	}
}

int bus_send(int type, const uint8_t *payload, size_t len)
{
	if (!bus.opened) {
		return -ENODEV;
	}
	if (type < 0 || type >= SERIAL_TYPE_UNKNOWN) {
		return -EINVAL;
	}
	if (bus_validate_tx((enum SerialType)type, (uint16_t)len) != 0) {
		return -EINVAL;
	}

	struct bus_header hdr = {
		.type = (enum SerialType)type,
		.no_ack = false,
		.host = false,
		.error = 0,
		.reserved = 0,
		.seq = bus.tx_seq,
		.length = (uint16_t)len,
	};

	bus.tx_seq = bus_seq_next(bus.tx_seq);
	bus_send_raw(&hdr, payload, len);
	return 0;
}

int bus_request(int type, const uint8_t *req, size_t req_len,
		uint8_t *resp, size_t *resp_len, uint32_t timeout_ms)
{
	if (!bus.opened) {
		return -ENODEV;
	}
	if (type < 0 || type >= SERIAL_TYPE_UNKNOWN) {
		return -EINVAL;
	}
	if (bus_validate_tx((enum SerialType)type, (uint16_t)req_len) != 0) {
		return -EINVAL;
	}

	bool retried = false;

	for (int attempt = 0; attempt < 2 && !retried; attempt++) {
		struct bus_header hdr = {
			.type = (enum SerialType)type,
			.no_ack = false,
			.host = false,
			.error = 0,
			.reserved = 0,
			.seq = bus.tx_seq,
			.length = (uint16_t)req_len,
		};

		bus.tx_seq = bus_seq_next(bus.tx_seq);
		bus.request_pending = true;
		bus_send_raw(&hdr, req, req_len);

		while (true) {
			if (k_sem_take(&bus.rx_sem, K_MSEC(timeout_ms)) != 0) {
				bus.request_pending = false;
				return -ETIMEDOUT;
			}

			struct bus_header last = bus.last_hdr;

			if (last.length == 0U) {
				/* ACK. */
				if ((last.error == ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER ||
				     last.error == ERROR_TYPE_LENGTH_TOO_LONG) &&
				    !retried) {
					/* Resync and retry exactly once. */
					bus.request_pending = false;
					if (bus_sync() != 0) {
						return -ETIMEDOUT;
					}
					retried = true;
					break;
				}
				/* Success ACK: the response follows. */
				continue;
			}

			/* Data frame. */
			if (last.type == (enum SerialType)type) {
				if (resp != NULL && resp_len != NULL) {
					size_t n = MIN(last.length,
							E80211_BUS_RX_BUF_SIZE);

					memcpy(resp, bus.rx_buf, n);
					*resp_len = n;
				}
				bus.request_pending = false;
				return 0;
			}

			/* Asynchronous frame of another type: dispatch and
			 * keep waiting for the matching response. */
			bus_rx_handler_t cb = bus.handler[last.type];

			if (cb != NULL) {
				cb(bus.rx_buf, last.length, bus.handler_user[last.type]);
			}
		}
	}

	bus.request_pending = false;
	return -ETIMEDOUT;
}

uint16_t bus_supported_types(void)
{
	return bus.supported_types;
}

void bus_set_crc_enabled(bool enabled)
{
	bus.crc_enabled = enabled;
	LOG_INF("Per-frame CRC32C %s", enabled ? "enabled" : "disabled");
}
