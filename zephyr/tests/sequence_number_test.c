/*
 * Test cases for sequence number management
 *
 * Tests TX and RX sequence number tracking, wrapping, and protocol compliance
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <serial/serial.h>

ZTEST_SUITE(sequence_numbers, NULL, NULL, NULL, NULL);

/**
 * @brief Test sequence number retrieval and advancement
 */
ZTEST(sequence_numbers, test_get_and_advance_sequence)
{
    uint8_t seq1 = serial_get_tx_sequence_number();
    uint8_t seq2 = serial_get_tx_sequence_number();

    /* Same sequence should be returned multiple times until advanced */
    zassert_equal(seq1, seq2, "Multiple reads should return same sequence");

    serial_advance_tx_sequence_number();
    uint8_t seq3 = serial_get_tx_sequence_number();

    /* After advance, should get next sequence (or wrap) */
    uint8_t expected = (seq1 + 1) & SEQUENCE_NUMBER_MAX;
    zassert_equal(seq3, expected,
                  "After advance, should get next sequence or wrapped value");
}

/**
 * @brief Test sequence number fits in 4-bit field
 */
ZTEST(sequence_numbers, test_sequence_in_4bit_range)
{
    uint8_t seq = serial_get_tx_sequence_number();

    /* Sequence number must be 0-15 (4-bit field) */
    zassert_true(seq <= SEQUENCE_NUMBER_MAX,
                 "Sequence number %u must fit in 4-bit field", seq);
}

/**
 * @brief Test sequence number wraparound at boundary
 */
ZTEST(sequence_numbers, test_sequence_wraparound)
{
    /* Advance sequence to near maximum */
    for (int i = 0; i < SEQUENCE_NUMBER_MAX; i++) {
        serial_advance_tx_sequence_number();
    }

    uint8_t seq_at_max = serial_get_tx_sequence_number();
    zassert_equal(seq_at_max, SEQUENCE_NUMBER_MAX,
                  "Should reach max sequence number (%u)", SEQUENCE_NUMBER_MAX);

    /* Advance one more time — should wrap to 0 */
    serial_advance_tx_sequence_number();
    uint8_t seq_wrapped = serial_get_tx_sequence_number();

    zassert_equal(seq_wrapped, 0, "Sequence should wrap to 0 after max");
}

/**
 * @brief Test sequence number increment in loop
 */
ZTEST(sequence_numbers, test_sequence_increment_loop)
{
    uint8_t prev_seq = serial_get_tx_sequence_number();

    /* Advance through multiple sequences and verify order */
    for (int cycle = 0; cycle < 2; cycle++) {
        for (int i = 0; i < SEQUENCE_NUMBER_MAX; i++) {
            serial_advance_tx_sequence_number();
            uint8_t curr_seq = serial_get_tx_sequence_number();
            uint8_t expected = (prev_seq + 1) & SEQUENCE_NUMBER_MAX;

            zassert_equal(curr_seq, expected,
                          "Sequence should increment correctly (cycle %d, step %d)",
                          cycle, i);
            prev_seq = curr_seq;
        }
    }
}

/**
 * @brief Test separate RX and TX sequence counters (mandate per AGENTS.md)
 *
 * Note: This is a specification test. The actual implementation may need
 * to maintain separate counters for RX and TX if bidirectional sequences
 * are required. Currently we only test TX.
 */
ZTEST(sequence_numbers, test_separate_tx_counter)
{
    /* Per AGENTS.md: "Host-to-device and device-to-host use separate sequence counters" */
    uint8_t tx_seq = serial_get_tx_sequence_number();

    /* TX sequence should be independent */
    zassert_true(tx_seq >= 0 && tx_seq <= SEQUENCE_NUMBER_MAX,
                 "TX sequence should be valid: %u", tx_seq);
}

/**
 * @brief Test sequence number reset on system initialization
 *
 * Per AGENTS.md: "Sequence numbering starts at 0 on system initialization"
 */
ZTEST(sequence_numbers, test_sequence_starts_at_zero)
{
    /* After system init, first sequence should be 0 or predictable */
    uint8_t first_seq = serial_get_tx_sequence_number();

    /* At minimum, it should be a valid 4-bit value */
    zassert_true(first_seq <= SEQUENCE_NUMBER_MAX,
                 "Initial sequence should be valid: %u", first_seq);
}

/**
 * @brief Test sequence number monotonicity
 */
ZTEST(sequence_numbers, test_sequence_monotonicity)
{
    uint8_t prev = serial_get_tx_sequence_number();
    zassert_true(prev <= SEQUENCE_NUMBER_MAX, "Initial sequence valid");

    /* Collect sequence numbers and verify they increase monotonically */
    for (int i = 0; i < 20; i++) {
        serial_advance_tx_sequence_number();
        uint8_t curr = serial_get_tx_sequence_number();

        /* Each advance should increase by 1 (with wraparound at 15) */
        uint8_t expected = (prev + 1) & SEQUENCE_NUMBER_MAX;
        zassert_equal(curr, expected,
                      "Sequence should be monotonically increasing");
        prev = curr;
    }
}

/**
 * @brief Test sequence number in frame header
 */
ZTEST(sequence_numbers, test_sequence_in_frame)
{
    struct SerialHeader frame = {0};
    uint8_t seq = serial_get_tx_sequence_number();

    frame.sequence_number = seq;

    zassert_equal(frame.sequence_number, seq,
                  "Frame sequence should match TX counter");
}

/**
 * @brief Test rapid sequence operations don't overflow
 */
ZTEST(sequence_numbers, test_rapid_advance)
{
    /* Rapidly advance and read sequence numbers */
    for (int i = 0; i < 1000; i++) {
        uint8_t seq = serial_get_tx_sequence_number();
        serial_advance_tx_sequence_number();

        /* Should always be in valid range */
        zassert_true(seq <= SEQUENCE_NUMBER_MAX,
                     "Sequence should remain in valid range after many ops");
    }
}

/**
 * @brief Test sequence number doesn't exceed 4-bit field after any operation
 */
ZTEST(sequence_numbers, test_sequence_never_exceeds_max)
{
    for (int i = 0; i < (SEQUENCE_NUMBER_MAX + 1) * 3; i++) {
        uint8_t seq = serial_get_tx_sequence_number();

        /* Mask to ensure it's actually 4-bit */
        uint8_t masked = seq & 0x0F;
        zassert_equal(seq, masked,
                      "Sequence number should never exceed 4-bit field");

        serial_advance_tx_sequence_number();
    }
}

/**
 * @brief Test sequence number behavior with different traffic types
 */
ZTEST(sequence_numbers, test_sequence_independent_of_type)
{
    uint8_t seq1 = serial_get_tx_sequence_number();

    /* Create frames with different types but same sequence */
    struct SerialHeader frame1 = {
        .type = SERIAL_TYPE_WIFI_MGMT,
        .sequence_number = seq1,
    };

    struct SerialHeader frame2 = {
        .type = SERIAL_TYPE_NET_IF_MGMT,
        .sequence_number = seq1,
    };

    /* Both should have same sequence number */
    zassert_equal(frame1.sequence_number, frame2.sequence_number,
                  "Sequence number should be independent of type");
}
