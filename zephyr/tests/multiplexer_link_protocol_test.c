/*
 * Multiplexer + link protocol integration test.
 *
 * One comprehensive test case that validates protocol-facing multiplexer
 * behavior without transport hardware dependencies.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <errno.h>
#include <string.h>

#include "../src/multiplexer.h"
#include "../src/serial/serial.h"

ZTEST_SUITE(multiplexer_link_protocol, NULL, NULL, NULL, NULL, NULL);

static int mock_send_called;
static struct serial_config *mock_send_config;
static enum SerialType mock_send_type;
static enum ErrorType mock_send_error;
static bool mock_send_no_ack;
static size_t mock_send_len;
static uint8_t mock_send_payload[SERIAL_MAX_PAYLOAD_LEN];

int serial_send_frame(struct serial_config *config,
                      enum SerialType type,
                      enum ErrorType error,
                      bool no_ack,
                      const uint8_t *payload,
                      size_t len)
{
    ARG_UNUSED(config);

    mock_send_called++;
    mock_send_config = config;
    mock_send_type = type;
    mock_send_error = error;
    mock_send_no_ack = no_ack;
    mock_send_len = len;

    if (payload != NULL && len > 0U) {
        memcpy(mock_send_payload, payload, len);
    }

    return 0;
}

ZTEST(multiplexer_link_protocol, test_multiplexer_link_protocol_single_case)
{
    mock_send_called = 0;
    mock_send_config = NULL;
    mock_send_type = SERIAL_TYPE_UNKNOWN;
    mock_send_error = ERROR_TYPE_NO_ERROR;
    mock_send_no_ack = false;
    mock_send_len = 0;

    uint16_t mask = serial_mux_supported_types_mask();

    /* Protocol-info capability must always be advertised. */
    zassert_true((mask & (1U << SERIAL_TYPE_PROTOCOL_INFO)) != 0U,
                 "Supported types must include PROTOCOL_INFO bit");

    /* Without technology proxy configs in this test app, callbacks are absent. */
    zassert_is_null(serial_mux_get_callbacks(SERIAL_TYPE_WIFI_MGMT),
                    "Unconfigured WIFI_MGMT callback must be NULL");
    zassert_is_null(serial_mux_get_callbacks(SERIAL_TYPE_WIFI_DATA),
                    "Unconfigured WIFI_DATA callback must be NULL");
    zassert_is_null(serial_mux_get_callbacks(SERIAL_TYPE_VSERIAL),
                    "Unconfigured VSERIAL callback must be NULL");
    zassert_is_null(serial_mux_get_callbacks(SERIAL_TYPE_UNKNOWN),
                    "Sentinel type must not resolve to callbacks");

    /* Link protocol invariant: payload limit remains 4096 bytes. */
    zassert_equal(SERIAL_MAX_PAYLOAD_LEN, 4096U,
                  "Link protocol payload max must stay at 4096 bytes");

    /* Sequence range invariant remains 4-bit (0..15). */
    zassert_equal(SEQUENCE_NUMBER_MAX, 15U,
                  "Link protocol sequence max must stay at 15");

    /* No interface registered => multiplexer must fail TX routing cleanly. */
    uint8_t payload[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    int ret = serial_mux_send(SERIAL_TYPE_WIFI_MGMT, payload, sizeof(payload));

    zassert_equal(ret, -ENODEV,
                  "TX without registered interface must fail with -ENODEV");

    /* Host-independent success path: register a mock UART interface. */
    static const struct device fake_uart_dev = {
        .name = "fake-uart",
    };
    static struct serial_config fake_uart_cfg = {
        .serial_dev = &fake_uart_dev,
    };

    ret = serial_mux_register_interface(SERIAL_MUX_IFACE_UART,
                                        &fake_uart_dev,
                                        &fake_uart_cfg);
    zassert_equal(ret, 0,
                  "Mock UART interface registration must succeed");

    ret = serial_mux_send(SERIAL_TYPE_WIFI_MGMT, payload, sizeof(payload));
    zassert_equal(ret, 0,
                  "Routed send should succeed after mock interface registration");

    zassert_equal(mock_send_called, 1,
                  "serial_send_frame should be called once for successful routed send");
    zassert_equal_ptr(mock_send_config, &fake_uart_cfg,
                      "Routed send should use the registered UART serial_config");
    zassert_equal(mock_send_type, SERIAL_TYPE_WIFI_MGMT,
                  "Routed send should preserve traffic type");
    zassert_equal(mock_send_error, ERROR_TYPE_NO_ERROR,
                  "Routed send should use NO_ERROR for normal egress");
    zassert_false(mock_send_no_ack,
                  "Routed send should request ACK by default");
    zassert_equal(mock_send_len, sizeof(payload),
                  "Routed send should preserve payload length");
    zassert_mem_equal(mock_send_payload, payload, sizeof(payload),
                      "Routed send should preserve payload bytes");
}
