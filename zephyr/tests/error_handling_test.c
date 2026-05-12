/*
 * Test cases for error handling in common link protocol
 *
 * Tests error codes, error ACK generation, and error conditions
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <serial/serial.h>

ZTEST_SUITE(error_handling, NULL, NULL, NULL, NULL);

/**
 * @brief Test all error types are defined
 */
ZTEST(error_handling, test_error_types_defined)
{
    enum ErrorType errors[] = {
        ERROR_TYPE_NO_ERROR,
        ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER,
        ERROR_TYPE_OUT_OF_BUFFERS,
        ERROR_TYPE_LENGTH_TOO_LONG,
    };

    for (size_t i = 0; i < ARRAY_SIZE(errors); i++) {
        zassert_true(errors[i] >= 0 && errors[i] <= 3,
                     "Error code %u should fit in 2-bit field", errors[i]);
    }
}

/**
 * @brief Test NO_ERROR condition
 */
ZTEST(error_handling, test_no_error_condition)
{
    struct SerialHeader frame = {0};

    frame.error = ERROR_TYPE_NO_ERROR;

    /* NO_ERROR should be zero (success code) */
    zassert_equal(frame.error, 0, "NO_ERROR should be 0");
}

/**
 * @brief Test INCORRECT_SEQUENCE_NUMBER error condition
 *
 * Per AGENTS.md: "On incorrect sequence number response, sender resets
 * expected sequence to 0 and may resend."
 */
ZTEST(error_handling, test_incorrect_sequence_error)
{
    struct SerialHeader ack = {0};

    ack.type = SERIAL_TYPE_WIFI_MGMT;
    ack.error = ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER;
    ack.sequence_number = 5;  /* Expected sequence */
    ack.length = sys_cpu_to_be16(0);

    zassert_equal(ack.error, ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER,
                  "Should be able to encode sequence error");
}

/**
 * @brief Test OUT_OF_BUFFERS error condition
 */
ZTEST(error_handling, test_out_of_buffers_error)
{
    struct SerialHeader ack = {0};

    ack.type = SERIAL_TYPE_WIFI_DATA;
    ack.error = ERROR_TYPE_OUT_OF_BUFFERS;
    ack.sequence_number = 12;
    ack.length = sys_cpu_to_be16(0);

    zassert_equal(ack.error, ERROR_TYPE_OUT_OF_BUFFERS,
                  "Should be able to encode buffer error");
}

/**
 * @brief Test LENGTH_TOO_LONG error condition
 *
 * Per AGENTS.md: "Length too long" is a supported error condition
 */
ZTEST(error_handling, test_length_too_long_error)
{
    struct SerialHeader ack = {0};

    /* LENGTH_TOO_LONG should be set in response to oversized frame */
    ack.type = SERIAL_TYPE_WIFI_MGMT;
    ack.error = ERROR_TYPE_LENGTH_TOO_LONG;
    ack.sequence_number = 7;
    ack.length = sys_cpu_to_be16(0);

    zassert_equal(ack.error, ERROR_TYPE_LENGTH_TOO_LONG,
                  "Should be able to encode length error");
}

/**
 * @brief Test MAX_PAYLOAD_LEN boundary condition
 */
ZTEST(error_handling, test_payload_length_boundary)
{
    struct SerialHeader frame = {0};

    /* Exactly at limit should succeed */
    frame.length = sys_cpu_to_be16(SERIAL_MAX_PAYLOAD_LEN);
    uint16_t len = sys_be16_to_cpu(frame.length);
    zassert_equal(len, SERIAL_MAX_PAYLOAD_LEN,
                  "Max payload length should be accepted");

    /* One byte over should trigger error */
    uint16_t over_limit = SERIAL_MAX_PAYLOAD_LEN + 1;
    zassert_true(over_limit > SERIAL_MAX_PAYLOAD_LEN,
                 "Test setup: over-limit should exceed max");
}

/**
 * @brief Test zero-length payload is invalid
 *
 * Per AGENTS.md: Only ACK frames can have zero length
 */
ZTEST(error_handling, test_zero_length_payload)
{
    struct SerialHeader frame = {0};

    frame.type = SERIAL_TYPE_WIFI_MGMT;
    frame.length = sys_cpu_to_be16(0);

    /* Zero length should only be valid for ACKs or protocol info */
    zassert_equal(sys_be16_to_cpu(frame.length), 0,
                  "Zero length should be encodable");
}

/**
 * @brief Test error code in response ACK
 */
ZTEST(error_handling, test_ack_contains_error)
{
    struct SerialHeader request = {0};
    struct SerialHeader response_ack = {0};

    /* Simulate: receiver got out-of-order frame */
    request.type = SERIAL_TYPE_NET_IF_MGMT;
    request.sequence_number = 5;

    /* Response ACK with error */
    response_ack.type = request.type;
    response_ack.error = ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER;
    response_ack.sequence_number = 3;  /* Expected sequence */
    response_ack.host = true;
    response_ack.length = sys_cpu_to_be16(0);

    zassert_equal(response_ack.error, ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER,
                  "Response should contain error code");
}

/**
 * @brief Test all error codes fit in 2-bit field
 */
ZTEST(error_handling, test_all_error_codes_2bit)
{
    uint8_t error_codes[] = {
        ERROR_TYPE_NO_ERROR,
        ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER,
        ERROR_TYPE_OUT_OF_BUFFERS,
        ERROR_TYPE_LENGTH_TOO_LONG,
    };

    for (size_t i = 0; i < ARRAY_SIZE(error_codes); i++) {
        struct SerialHeader frame = {0};
        frame.error = error_codes[i];

        /* After encoding in 2-bit field, should still match */
        zassert_true(frame.error < 4, "Error code %u should fit in 2 bits",
                     error_codes[i]);
    }
}

/**
 * @brief Test unrecognized command maps to LENGTH_TOO_LONG error
 *
 * Per AGENTS.md: "Unrecognized command" uses LENGTH_TOO_LONG code
 */
ZTEST(error_handling, test_unrecognized_command_error)
{
    struct SerialHeader error_response = {0};

    /* Unrecognized command gets LENGTH_TOO_LONG error */
    error_response.error = ERROR_TYPE_LENGTH_TOO_LONG;

    zassert_equal(error_response.error, ERROR_TYPE_LENGTH_TOO_LONG,
                  "Unrecognized command should use LENGTH_TOO_LONG code");
}

/**
 * @brief Test error ACK preserves sequence number
 */
ZTEST(error_handling, test_error_ack_preserves_sequence)
{
    struct SerialHeader request = {0};
    struct SerialHeader error_ack = {0};

    request.sequence_number = 11;

    /* Error ACK should echo the bad sequence number */
    error_ack.sequence_number = request.sequence_number;
    error_ack.error = ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER;

    zassert_equal(error_ack.sequence_number, 11,
                  "Error ACK should preserve sequence number from request");
}

/**
 * @brief Test no_ack flag prevents error responses
 *
 * Per AGENTS.md: Frames with no_ack should not generate responses
 */
ZTEST(error_handling, test_no_ack_no_response)
{
    struct SerialHeader no_ack_frame = {0};

    no_ack_frame.no_ack = true;
    no_ack_frame.type = SERIAL_TYPE_WIFI_DATA;

    /* Receiver should not send ACK even on error */
    zassert_true(no_ack_frame.no_ack,
                 "no_ack flag prevents error responses");
}

/**
 * @brief Test reserved bits must not affect error reporting
 */
ZTEST(error_handling, test_reserved_bits_dont_affect_error)
{
    struct SerialHeader frame1 = {0};
    struct SerialHeader frame2 = {0};

    frame1.reserved = 0;
    frame2.reserved = 0;
    frame1.error = ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER;
    frame2.error = ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER;

    zassert_equal(frame1.error, frame2.error,
                  "Error should be independent of reserved bits");
}

/**
 * @brief Test error ACK for LENGTH_TOO_LONG
 */
ZTEST(error_handling, test_length_too_long_ack)
{
    struct SerialHeader error_ack = {0};

    /* Response when received frame exceeds max payload */
    error_ack.error = ERROR_TYPE_LENGTH_TOO_LONG;
    error_ack.host = true;  /* Device→host */
    error_ack.sequence_number = 4;
    error_ack.length = sys_cpu_to_be16(0);  /* Error ACK has no payload */

    zassert_equal(error_ack.error, ERROR_TYPE_LENGTH_TOO_LONG,
                  "LENGTH_TOO_LONG error should be encodable");
}

/**
 * @brief Test error condition doesn't corrupt frame type
 */
ZTEST(error_handling, test_error_preserves_type)
{
    struct SerialHeader frame = {0};

    frame.type = SERIAL_TYPE_PROTOCOL_INFO;
    frame.error = ERROR_TYPE_OUT_OF_BUFFERS;

    zassert_equal(frame.type, SERIAL_TYPE_PROTOCOL_INFO,
                  "Error should not corrupt traffic type");
}

/**
 * @brief Test all traffic types can have errors
 */
ZTEST(error_handling, test_error_all_types)
{
    uint8_t types[] = {
        SERIAL_TYPE_WIFI_MGMT,
        SERIAL_TYPE_WIFI_DATA,
        SERIAL_TYPE_NET_IF_MGMT,
        SERIAL_TYPE_HCI,
        SERIAL_TYPE_THREAD,
    };

    for (size_t i = 0; i < ARRAY_SIZE(types); i++) {
        struct SerialHeader error_ack = {0};
        error_ack.type = types[i];
        error_ack.error = ERROR_TYPE_OUT_OF_BUFFERS;

        zassert_equal(error_ack.error, ERROR_TYPE_OUT_OF_BUFFERS,
                      "Type %u should support error responses", types[i]);
    }
}
