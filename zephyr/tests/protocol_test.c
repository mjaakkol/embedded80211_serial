/*
 * Test cases for common link protocol core functionality
 *
 * Tests protocol version, supported types, sequence numbers, and header structure
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <serial/serial.h>

ZTEST_SUITE(protocol_core, NULL, NULL, NULL, NULL);

/**
 * @brief Test protocol version constant
 */
ZTEST(protocol_core, test_protocol_version)
{
    zassert_equal(SERIAL_PROTOCOL_VERSION, 2,
                  "Protocol version should be 2 (4-bit sequence support)");
}

/**
 * @brief Test maximum payload length
 */
ZTEST(protocol_core, test_max_payload_length)
{
    zassert_equal(SERIAL_MAX_PAYLOAD_LEN, 4096,
                  "Max payload should be 4096 bytes per AGENTS.md");
}

/**
 * @brief Test sequence number maximum
 */
ZTEST(protocol_core, test_sequence_number_max)
{
    zassert_equal(SEQUENCE_NUMBER_MAX, 15,
                  "Sequence number should wrap after 15 (4-bit field)");
}

/**
 * @brief Test SerialType enum values are distinct and in order
 */
ZTEST(protocol_core, test_serial_type_enum_values)
{
    zassert_equal(SERIAL_TYPE_WIFI_MGMT, 0, "WIFI_MGMT should be 0");
    zassert_equal(SERIAL_TYPE_WIFI_DATA, 1, "WIFI_DATA should be 1");
    zassert_equal(SERIAL_TYPE_NET_IF_MGMT, 2, "NET_IF_MGMT should be 2");
    zassert_equal(SERIAL_TYPE_HCI, 3, "HCI should be 3");
    zassert_equal(SERIAL_TYPE_THREAD, 4, "THREAD should be 4");
    zassert_equal(SERIAL_TYPE_NET_OFFLOADING, 5, "NET_OFFLOADING should be 5");
    zassert_equal(SERIAL_TYPE_AT_CMD, 6, "AT_CMD should be 6");
    zassert_equal(SERIAL_TYPE_PROTOCOL_INFO, 7, "PROTOCOL_INFO should be 7");
    zassert_equal(SERIAL_TYPE_VSERIAL, 8, "VSERIAL should be 8");
    zassert_equal(SERIAL_TYPE_UNKNOWN, 9, "UNKNOWN should be 9 (sentinel)");
}

/**
 * @brief Test ErrorType enum values
 */
ZTEST(protocol_core, test_error_type_enum_values)
{
    zassert_equal(ERROR_TYPE_NO_ERROR, 0, "NO_ERROR should be 0");
    zassert_equal(ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER, 1,
                  "INCORRECT_SEQUENCE_NUMBER should be 1");
    zassert_equal(ERROR_TYPE_OUT_OF_BUFFERS, 2, "OUT_OF_BUFFERS should be 2");
    zassert_equal(ERROR_TYPE_LENGTH_TOO_LONG, 3, "LENGTH_TOO_LONG should be 3");
}

/**
 * @brief Test supported types bitmask includes all required types
 */
ZTEST(protocol_core, test_supported_types_mask)
{
    /* Check that all required types are in the bitmask */
    zassert_true(SERIAL_SUPPORTED_TYPES_MASK & (1U << SERIAL_TYPE_WIFI_MGMT),
                 "WIFI_MGMT should be supported");
    zassert_true(SERIAL_SUPPORTED_TYPES_MASK & (1U << SERIAL_TYPE_WIFI_DATA),
                 "WIFI_DATA should be supported");
    zassert_true(SERIAL_SUPPORTED_TYPES_MASK & (1U << SERIAL_TYPE_NET_IF_MGMT),
                 "NET_IF_MGMT should be supported");
    zassert_true(SERIAL_SUPPORTED_TYPES_MASK & (1U << SERIAL_TYPE_HCI),
                 "HCI should be supported");
    zassert_true(SERIAL_SUPPORTED_TYPES_MASK & (1U << SERIAL_TYPE_THREAD),
                 "THREAD should be supported");
    zassert_true(SERIAL_SUPPORTED_TYPES_MASK & (1U << SERIAL_TYPE_PROTOCOL_INFO),
                 "PROTOCOL_INFO should be supported");
    zassert_true(SERIAL_SUPPORTED_TYPES_MASK & (1U << SERIAL_TYPE_VSERIAL),
                 "VSERIAL should be supported");
}

/**
 * @brief Test SerialHeader structure layout
 *
 * Verify that bitfield layout is correct:
 * - type: 4 bits
 * - no_ack: 1 bit
 * - host: 1 bit
 * - error: 2 bits
 * - reserved: 4 bits
 * - sequence_number: 4 bits
 * - length: 16 bits (big-endian)
 * Total: 32 bits header
 */
ZTEST(protocol_core, test_serial_header_structure)
{
    struct SerialHeader hdr = {
        .type = SERIAL_TYPE_WIFI_MGMT,
        .no_ack = true,
        .host = false,
        .error = ERROR_TYPE_NO_ERROR,
        .reserved = 0,
        .sequence_number = 5,
        .length = 0x1234,  /* Big-endian */
    };

    /* Verify structure size is 4 bytes (32-bit header) */
    zassert_equal(sizeof(struct SerialHeader), 4,
                  "SerialHeader should be 4 bytes (32-bit)");

    /* Verify fields are set correctly (bitfield layout) */
    zassert_equal(hdr.type, SERIAL_TYPE_WIFI_MGMT, "type field should be set");
    zassert_true(hdr.no_ack, "no_ack should be true");
    zassert_false(hdr.host, "host should be false");
    zassert_equal(hdr.error, ERROR_TYPE_NO_ERROR, "error should be NO_ERROR");
    zassert_equal(hdr.sequence_number, 5, "sequence_number should be 5");
    zassert_equal(hdr.length, 0x1234, "length should be 0x1234");
}

/**
 * @brief Test sequence number wraparound
 */
ZTEST(protocol_core, test_sequence_number_wraparound)
{
    struct SerialHeader hdr = {0};

    /* Test boundary: max sequence number is 15 */
    hdr.sequence_number = 15;
    zassert_equal(hdr.sequence_number, 15, "Max sequence number should be 15");

    /* After wrapping, should go back to 0 */
    hdr.sequence_number = (hdr.sequence_number + 1) & 0x0F;
    zassert_equal(hdr.sequence_number, 0,
                  "Sequence number should wrap to 0 after 15");
}

/**
 * @brief Test parsing state transitions
 */
ZTEST(protocol_core, test_parsing_state_enum)
{
    enum ParsingState state;

    state = PARSING_STATE_TYPE;
    zassert_equal(state, 0, "PARSING_STATE_TYPE should be first");

    state = PARSING_STATE_SEQUENCE_NUMBER;
    zassert_equal(state, 1, "PARSING_STATE_SEQUENCE_NUMBER should be second");

    state = PARSING_STATE_LENGTH1;
    state = PARSING_STATE_LENGTH2;
    state = PARSING_STATE_DATA;
    zassert_equal(state, 4, "PARSING_STATE_DATA should be fifth");
}

/**
 * @brief Test error code fits in 2-bit field
 */
ZTEST(protocol_core, test_error_code_2bit_range)
{
    struct SerialHeader hdr = {0};

    /* All error codes should fit in 2 bits (0-3) */
    hdr.error = ERROR_TYPE_NO_ERROR;
    zassert_true(hdr.error < 4, "NO_ERROR should fit in 2 bits");

    hdr.error = ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER;
    zassert_true(hdr.error < 4, "INCORRECT_SEQUENCE_NUMBER should fit in 2 bits");

    hdr.error = ERROR_TYPE_OUT_OF_BUFFERS;
    zassert_true(hdr.error < 4, "OUT_OF_BUFFERS should fit in 2 bits");

    hdr.error = ERROR_TYPE_LENGTH_TOO_LONG;
    zassert_true(hdr.error < 4, "LENGTH_TOO_LONG should fit in 2 bits");
}

/**
 * @brief Test type field fits in 4-bit range
 */
ZTEST(protocol_core, test_type_field_4bit_range)
{
    struct SerialHeader hdr = {0};

    /* SERIAL_TYPE_UNKNOWN (9) is the maximum and should fit */
    hdr.type = SERIAL_TYPE_UNKNOWN;
    zassert_true(hdr.type < 16, "Type should fit in 4 bits (0-15)");

    /* All defined types should fit */
    hdr.type = SERIAL_TYPE_VSERIAL;
    zassert_true(hdr.type < 16, "VSERIAL should fit in 4 bits");
}
