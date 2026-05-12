# Quick Start — Common Link Protocol Tests

## One-Minute Setup

```bash
cd /Users/mikko/projects/cfg80211_serial/zephyr/tests

# Make scripts executable
chmod +x build_tests.sh run_tests.sh

# Build and run all tests
./run_tests.sh
```

## What Gets Tested

✅ **Protocol Constants** (8 tests)
- Protocol version = 2
- Max payload = 4096 bytes
- Sequence numbers 0-15 (4-bit)
- 10 traffic types
- 4 error codes

✅ **Frame Structure** (13 tests)
- Frame header construction
- Sequence number encoding
- ACK frames (with/without errors)
- Payload length validation
- Endianness handling

✅ **Sequence Management** (11 tests)
- Get/advance sequence number API
- Wraparound at boundary (15→0)
- Monotonic increment
- Stress test (1000+ operations)
- Type independence

✅ **Error Handling** (16 tests)
- All 4 error codes
- Length validation (max 4096)
- Error ACK generation
- No-ACK flag behavior
- Error preservation across types

## Test Statistics

- **Total Tests:** 44
- **Test Code:** ~1,140 lines
- **Assertions:** 100+
- **Time to Run:** <1 second

## Files Created

```
tests/
├── CMakeLists.txt              # Build configuration
├── prj.conf                    # Zephyr config (minimal)
├── app.overlay                 # Device tree (empty)
├── protocol_test.c             # Protocol constants (8 tests)
├── frame_parser_test.c         # Frame structure (13 tests)
├── sequence_number_test.c      # Sequence management (11 tests)
├── error_handling_test.c       # Error conditions (16 tests)
├── build_tests.sh              # Build helper script
├── run_tests.sh                # Run helper script
├── README.md                   # Full documentation
├── TEST_REFERENCE.md           # Test case reference
└── QUICKSTART.md              # This file
```

## Running Tests

### Option 1: Automated Script (Easiest)
```bash
cd tests
./run_tests.sh
```

### Option 2: Manual Build
```bash
cd tests
cmake -B build \
  -DZephyr_DIR:PATH=/opt/nordic/ncs/v3.3.0/zephyr/cmake \
  -DBOARD=native_sim .
cmake --build build
ctest -V --test-dir build
```

### Option 3: Run Specific Suite
```bash
ctest -R "protocol_core" -V --test-dir tests/build
ctest -R "sequence_numbers" -V --test-dir tests/build
ctest -R "error_handling" -V --test-dir tests/build
```

## Expected Output

```
Test project /Users/.../tests/build
    Start  1: zephyr_1
1/1 Test  #1: zephyr_1 .......................   Passed    0.XX sec

100% tests passed, 0 tests failed out of 1

Test project time =   0.XX sec
```

## Test Results Interpretation

| Result | Meaning | Action |
|--------|---------|--------|
| ✓ Passed | All assertions passed | None — protocol compliant |
| ✗ Failed | One+ assertions failed | Check AGENTS.md compliance |
| ⚠ Skipped | Test not run | Check dependencies |

## Debugging Failed Tests

### Verbose Output
```bash
ctest -V --output-on-failure --test-dir tests/build
```

### Enable Logging
```bash
cmake -B build -DCONFIG_LOG=y -DCONFIG_ZTEST_ASSERT_VERBOSE=y ...
```

### Run Single Test
```bash
cd tests/build
ninja && ./zephyr/zephyr.elf  # Then grep output for test name
```

## Troubleshooting

### Build Error: "serial/serial.h: No such file"
**Fix:** Ensure `tests/` is a subdirectory of `zephyr/`
```bash
pwd  # Should end in /cfg80211_serial/zephyr/tests
```

### Build Error: "zephyr not found"
**Fix:** Set Zephyr environment
```bash
export ZEPHYR_BASE=/opt/nordic/ncs/v3.3.0/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/nordic/ncs/toolchains/0c0f19d91c/opt/zephyr-sdk
```

### Test Fails: "Sequence should wrap to 0 after max"
**Issue:** TX sequence counter not wrapping correctly
**Check:** `serial_get_tx_sequence_number()` and `serial_advance_tx_sequence_number()` in serial.c

## What Each Test File Tests

### protocol_test.c (8 tests)
Tests protocol-level constants and structures defined in `serial.h`:
- SERIAL_PROTOCOL_VERSION = 2
- SERIAL_MAX_PAYLOAD_LEN = 4096
- SEQUENCE_NUMBER_MAX = 15
- SerialType enum (10 types)
- ErrorType enum (4 types)
- SerialHeader bitfield structure

### frame_parser_test.c (13 tests)
Tests frame construction and wire format:
- Typical frame construction
- Min/max payload sizes
- Sequence encoding
- ACK frame generation
- No-ACK flag
- Direction bit (host/device)
- Reserved bit compliance
- Endianness

### sequence_number_test.c (11 tests)
Tests sequence number management APIs:
- `serial_get_tx_sequence_number()`
- `serial_advance_tx_sequence_number()`
- Wraparound at 15
- Monotonicity over 20+ ops
- Range 0-15 (4-bit) enforcement
- Stress test (1000 ops)

### error_handling_test.c (16 tests)
Tests error handling and validation:
- All 4 error codes
- Length boundary (4096 max)
- Error ACK generation
- Error preservation
- No-ACK flag preventing responses
- Errors for all traffic types

## Advanced Usage

### Custom Configuration
```bash
cd tests
cmake -B build \
  -DCONFIG_VSERIAL_RX_BUF_SIZE=512 \
  -DCONFIG_VSERIAL_TX_BUF_SIZE=512 \
  ...
```

### Filter Tests by Pattern
```bash
ctest -R "sequence" -V --test-dir tests/build  # All sequence tests
ctest -R "wrap" -V --test-dir tests/build      # All wraparound tests
```

### Generate Test Report
```bash
ctest --test-dir tests/build -T Test -O report.xml
```

## AGENTS.md Compliance

These tests verify compliance with embedded 802.11 serial requirements:

✅ Common Link Protocol
- Frame header ≤64 bits (32-bit tested)
- Traffic type field (4-bit)
- Payload length (≤4096 bytes)
- Sequence number (0-15, wrapping)
- Error codes (4 types, 2-bit field)

✅ Error Handling
- Incorrect sequence number
- Out of buffers
- Length too long
- Unrecognized command (→ LENGTH_TOO_LONG)

✅ Sequencing
- Sequence starts at 0
- Separate TX/RX counters (tested TX)
- Monotonic increment with wraparound
- Per AGENTS.md section: "Sequence Numbering"

## Next Steps

1. **Extend Protocol Tests:** Add RX sequence, frame parsing state machine
2. **Integration Tests:** Test with actual serial transport
3. **Performance Tests:** Measure throughput, latency
4. **Stress Tests:** Long-running with pattern variations

## Reference

- [README.md](README.md) — Full documentation
- [TEST_REFERENCE.md](TEST_REFERENCE.md) — All 44 test cases
- [AGENTS.md](../AGENTS.md) — Product requirements
- [serial.h](../src/serial/serial.h) — Protocol definitions

## Support

For issues or questions:
1. Check [TEST_REFERENCE.md](TEST_REFERENCE.md) for test details
2. Review [README.md](README.md) for advanced options
3. Verify [AGENTS.md](../AGENTS.md) compliance

---

**Happy Testing!** 🧪
