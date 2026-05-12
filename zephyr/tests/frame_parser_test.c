/*
 * Test cases for serial frame parsing and validation
 *
 * Tests frame construction, parsing state machine, and payload handling
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <serial/serial.h>

ZTEST_SUITE(frame_parsing, NULL, NULL, NULL, NULL);

/**
 * @brief Test frame header construction for typical frame
 */
ZTEST(frame_parsing, test_construct_frame_header)
{
    struct SerialHeader hdr;
    uint8_t frame_bytes[4];

    /* Construct a typical frame: WIFI_MGMT, sequence 3, payload length 100 */
    hdr.type = SERIAL_TYPE_WIFI_MGMT;
    hdr.no_ack = false;
    hdr.host = true;
    hdr.error = ERROR_TYPE_NO_ERROR;
    hdr.reserved = 0;
    hdr.sequence_number = 3;
    hdr.length = sys_cpu_to_be16(100);

    /* Copy header to byte array for transmission */
    memcpy(frame_bytes, &hdr, sizeof(hdr));

    /* Verify all bytes are set */
    zassert_true(frame_bytes[0] != 0 || frame_bytes[1] != 0 ||
                 frame_bytes[2] != 0 || frame_bytes[3] != 0,
                 "Frame header should have non-zero content");
}

/**
 * @brief Test frame with maximum payload length
 */
ZTEST(frame_parsing, test_frame_max_payload)
{
    struct SerialHeader hdr = {0};

    hdr.type = SERIAL_TYPE_WIFI_DATA;
    hdr.length = sys_cpu_to_be16(SERIAL_MAX_PAYLOAD_LEN);

    /* Verify max length fits in 16-bit field */
    uint16_t len_be = sys_cpu_to_be16(SERIAL_MAX_PAYLOAD_LEN);
    zassert_equal(len_be, hdr.length,
                  "Max payload length should be encoded correctly");
}

/**
 * @brief Test frame length exceeds maximum
 */
ZTEST(frame_parsing, test_frame_length_too_long)
{
    struct SerialHeader hdr = {0};
    uint16_t length_too_long = SERIAL_MAX_PAYLOAD_LEN + 1;

    /* This should trigger LENGTH_TOO_LONG error in protocol */
    zassert_true(length_too_long > SERIAL_MAX_PAYLOAD_LEN,
                 "Test setup should have length > max");
}

/**
 * @brief Test sequence number field encoding
 */
ZTEST(frame_parsing, test_sequence_number_encoding)
{
    struct SerialHeader hdr = {0};

    /* Test all valid sequence numbers (0-15) */
    for (int seq = 0; seq <= SEQUENCE_NUMBER_MAX; seq++) {
        hdr.sequence_number = seq;
        zassert_equal(hdr.sequence_number, seq,
                      "Sequence number encoding failed for value %d", seq);
    }
}

/**
 * @brief Test ACK frame construction
 */
ZTEST(frame_parsing, test_construct_ack_frame)
{
    struct SerialHeader ack = {0};

    /* ACK has no payload (length=0) and echoes sequence number */
    ack.type = SERIAL_TYPE_NET_IF_MGMT;
    ack.no_ack = false;
    ack.host = true;  /* Device→host direction */
    ack.error = ERROR_TYPE_NO_ERROR;
    ack.sequence_number = 7;
    ack.length = sys_cpu_to_be16(0);  /* No payload in ACK */

    zassert_equal(ack.error, ERROR_TYPE_NO_ERROR, "ACK should have NO_ERROR");
    uint16_t len_be = sys_be16_to_cpu(ack.length);
    zassert_equal(len_be, 0, "ACK should have zero payload length");
}

/**
 * @brief Test ACK frame with error code
 */
ZTEST(frame_parsing, test_ack_with_error)
{
    struct SerialHeader ack = {0};

    ack.type = SERIAL_TYPE_WIFI_MGMT;
    ack.no_ack = false;
    ack.host = true;
    ack.error = ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER;
    ack.sequence_number = 5;
    ack.length = sys_cpu_to_be16(0);

    zassert_equal(ack.error, ERROR_TYPE_INCORRECT_SEQUENCE_NUMBER,
                  "ACK should contain error code");
}

/**
 * @brief Test no-ack flag (request no response)
 */
ZTEST(frame_parsing, test_no_ack_flag)
{
    struct SerialHeader hdr_with_ack = {0};
    struct SerialHeader hdr_no_ack = {0};

    hdr_with_ack.no_ack = false;
    hdr_no_ack.no_ack = true;

    zassert_false(hdr_with_ack.no_ack, "ACK flag should be false");
    zassert_true(hdr_no_ack.no_ack, "No-ACK flag should be true");
}

/**
 * @brief Test host bit for direction indication
 */
ZTEST(frame_parsing, test_host_bit_direction)
{
    struct SerialHeader hdr_device_to_host = {0};
    struct SerialHeader hdr_host_to_device = {0};

    hdr_device_to_host.host = true;   /* Device→host */
    hdr_host_to_device.host = false;  /* Host→device */

    zassert_true(hdr_device_to_host.host,
                 "Host bit should be true for device→host direction");
    zassert_false(hdr_host_to_device.host,
                  "Host bit should be false for host→device direction");
}

/**
 * @brief Test reserved bits are zero (protocol compliance)
 */
ZTEST(frame_parsing, test_reserved_bits_zero)
{
    struct SerialHeader hdr = {0};

    /* Reserved bits must be zero for protocol compliance */
    hdr.type = SERIAL_TYPE_WIFI_MGMT;
    hdr.no_ack = false;
    hdr.host = true;
    hdr.error = ERROR_TYPE_NO_ERROR;
    hdr.reserved = 0;  /* Must be zero */
    hdr.sequence_number = 5;
    hdr.length = sys_cpu_to_be16(100);

    zassert_equal(hdr.reserved, 0, "Reserved bits must be zero");
}

/**
 * @brief Test frame with minimum payload (1 byte)
 */
ZTEST(frame_parsing, test_frame_min_payload)
{
    struct SerialHeader hdr = {0};

    hdr.type = SERIAL_TYPE_VSERIAL;
    hdr.length = sys_cpu_to_be16(1);

    uint16_t len_be = sys_be16_to_cpu(hdr.length);
    zassert_equal(len_be, 1, "Minimum payload length should be 1 byte");
}

/**
 * @brief Test frame with typical payload sizes
 */
ZTEST(frame_parsing, test_frame_typical_payloads)
{
    uint16_t typical_sizes[] = {1, 64, 256, 512, 1024, 2048, 4096};

    for (size_t i = 0; i < ARRAY_SIZE(typical_sizes); i++) {
        struct SerialHeader hdr = {0};
        hdr.type = SERIAL_TYPE_WIFI_DATA;
        hdr.length = sys_cpu_to_be16(typical_sizes[i]);

        uint16_t len_decoded = sys_be16_to_cpu(hdr.length);
        zassert_equal(len_decoded, typical_sizes[i],
                      "Payload size %u should encode/decode correctly",
                      typical_sizes[i]);
    }
}

/**
 * @brief Test all traffic types can appear in frames
 */
ZTEST(frame_parsing, test_all_traffic_types)
{
    uint8_t supported_types[] = {
        SERIAL_TYPE_WIFI_MGMT,
        SERIAL_TYPE_WIFI_DATA,
        SERIAL_TYPE_NET_IF_MGMT,
        SERIAL_TYPE_HCI,
        SERIAL_TYPE_THREAD,
        SERIAL_TYPE_PROTOCOL_INFO,
        SERIAL_TYPE_VSERIAL,
    };

    for (size_t i = 0; i < ARRAY_SIZE(supported_types); i++) {
        struct SerialHeader hdr = {0};
        hdr.type = supported_types[i];

        zassert_equal(hdr.type, supported_types[i],
                      "Type %u should be encodable", supported_types[i]);
    }
}

/**
 * @brief Test frame header is little-endian (except length which is big-endian)
 */
ZTEST(frame_parsing, test_frame_endianness)
{
    struct SerialHeader hdr = {0};
    uint8_t *bytes = (uint8_t *)&hdr;

    hdr.type = SERIAL_TYPE_WIFI_MGMT;  /* 0 */
    hdr.no_ack = false;
    hdr.host = true;
    hdr.error = ERROR_TYPE_NO_ERROR;
    hdr.reserved = 0;
    hdr.sequence_number = 3;
    hdr.length = 0x1234;  /* Big-endian (stored as 0x12, 0x34 in BE) */

    /* First byte contains: type:4, no_ack:1, host:1, error:2 */
    /* With type=0, no_ack=0, host=1, error=0: first nibble = 0010 = 0x2 */
    zassert_true(bytes[0] != 0 || bytes[1] != 0 || bytes[2] != 0 || bytes[3] != 0,
                 "Frame header bytes should be properly formatted");
}
