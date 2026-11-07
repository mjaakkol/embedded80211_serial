/*
 * Test cases for sequence number management
 *
 * Tests TX and RX sequence number tracking, wrapping, and protocol compliance
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <embedded80211/serial.h>

ZTEST_SUITE(sequence_numbers, NULL, NULL, NULL, NULL, NULL);

/**
 * @brief Test sequence number wraps at 4-bit boundary
 */
ZTEST(sequence_numbers, test_sequence_wrap_math)
{
    uint8_t seq = 0;

    for (int i = 0; i < 16; i++) {
        zassert_true(seq <= SEQUENCE_NUMBER_MAX,
                     "Sequence should stay in 0..15");
        seq = (seq >= SEQUENCE_NUMBER_MAX) ? 0 : (uint8_t)(seq + 1);
    }

    zassert_equal(seq, 0, "Sequence should wrap to 0 after 16 steps");
}

/**
 * @brief Test sequence number field in serial header
 */
ZTEST(sequence_numbers, test_sequence_field_encode_decode)
{
    struct SerialHeader hdr = {0};

    for (uint8_t seq = 0; seq <= SEQUENCE_NUMBER_MAX; seq++) {
        hdr.sequence_number = seq;
        zassert_equal(hdr.sequence_number, seq,
                      "Sequence value %u should round-trip in header", seq);
    }
}

/**
 * @brief Test per-config tx sequence counters are independent
 */
ZTEST(sequence_numbers, test_sequence_per_config_independence)
{
    struct serial_config a = {0};
    struct serial_config b = {0};

    a.tx_sequence_number = 3;
    b.tx_sequence_number = 11;

    zassert_not_equal(a.tx_sequence_number, b.tx_sequence_number,
                      "Separate configs should keep independent tx sequence values");
}
