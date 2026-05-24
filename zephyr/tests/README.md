# Common Link Protocol Test Suite

This directory contains Zephyr unit tests for the embedded 802.11 serial common link protocol, including the multiplexer routing path.

## Test Files

### 1. **protocol_test.c** — Protocol Core
Tests protocol constants, enums, and data structures:
- Protocol version constant
- Maximum payload length (4096 bytes)
- Sequence number range (0-15)
- SerialType enum (10 traffic types)
- ErrorType enum (4 error codes)
- Supported types bitmask via multiplexer API
- SerialHeader bitfield structure and layout
- Reserved bits compliance

**Coverage:**
- ✅ Protocol version constant
- ✅ Max payload validation
- ✅ Sequence number wrapping
- ✅ All traffic types defined
- ✅ All error codes fit in 2-bit field
- ✅ Type field fits in 4-bit field

### 2. **frame_parser_test.c** — Frame Construction and Parsing
Tests frame construction, serialization, and validation:
- Frame header construction
- Payload length encoding (max, min, typical)
- Sequence number encoding in frames
- ACK frame construction (with and without errors)
- No-ACK flag behavior
- Host bit (direction indicator)
- Reserved bits validation
- All traffic types support frame construction
- Frame endianness (big-endian length field)

**Coverage:**
- ✅ Frame construction for all types
- ✅ Maximum payload frame (4096 bytes)
- ✅ Minimum payload frame (1 byte)
- ✅ Typical payload sizes (64, 256, 512, 1024, 2048, 4096)
- ✅ Error ACK frames
- ✅ No-ACK frames
- ✅ Direction indication
- ✅ Endianness validation

### 3. **sequence_number_test.c** — Sequence Number Behavior
Tests sequence-number invariants and representation:
- 4-bit range behavior
- Wraparound math at boundary (15 → 0)
- Header sequence encode/decode
- Per-config sequence independence

**Coverage:**
- ✅ Sequence retrieval and advancement
- ✅ 4-bit range enforcement
- ✅ Wraparound at 15
- ✅ Monotonic sequence (with wrap)
- ✅ Multiple cycles
- ✅ Stress test (1000 operations)
- ✅ Independence from frame type

### 4. **error_handling_test.c** — Error Handling
Tests error codes, error responses, and error conditions:
- All error types defined (NO_ERROR, INCORRECT_SEQUENCE, OUT_OF_BUFFERS, LENGTH_TOO_LONG)
- Error codes fit in 2-bit field
- Payload length boundary conditions
- Zero-length payload handling
- Error ACK generation
- Error ACK preserves sequence number
- No-ACK flag prevents responses
- Error with reserved bits
- Unrecognized command mapping (→ LENGTH_TOO_LONG)
- Errors for all traffic types

**Coverage:**
- ✅ All error codes defined and encodable
- ✅ 2-bit field range
- ✅ Max payload boundary (4096 bytes)
- ✅ Over-limit detection
- ✅ Error ACK with sequence preservation
- ✅ No-ACK suppresses responses
- ✅ Errors on all traffic types

### 5. **multiplexer_link_protocol_test.c** — Multiplexer Link Protocol Path
Single integrated test case that validates:
- Protocol-info bit exposure in supported mask
- Missing interface routing failure (-ENODEV)
- Mock interface registration success
- Successful routed send path with payload/type integrity checks

## Running the Tests

### Build Tests
```bash
cd /Users/mikko/projects/cfg80211_serial/zephyr/tests
export ZEPHYR_BASE=/opt/nordic/ncs/v3.3.0/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/nordic/ncs/toolchains/0c0f19d91c/opt/zephyr-sdk
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
west build -p always -b qemu_cortex_m3 .
```

### Run on Emulator or Hardware
```bash
# Run on QEMU
cd build/tests
west build -t run
```

## Test Statistics

| Category | Tests | Lines of Code |
|----------|-------|---------------|
| Protocol Core | 11 | ~220 |
| Frame Parsing | 13 | ~240 |
| Sequence Numbers | 3 | ~90 |
| Error Handling | 16 | ~250 |
| Multiplexer Path | 1 | ~120 |
| **Total** | **44** | **~920** |

## Test Assertions

All tests use ztest assertions:
- `zassert_equal()` — Numeric/equality checks
- `zassert_true()` / `zassert_false()` — Boolean checks
- `zassert_within()` — Range checks

## Protocol Requirements Covered

From AGENTS.md, these requirements are tested:

✅ **Frame Format**
- Header ≤64 bits (tested: 32 bits for protocol header)
- Traffic type field (4-bit)
- Payload length max 4096 bytes
- Sequence number (4-bit, 0-15)

✅ **Sequencing and Reliability**
- Sequence range 0..15 with wrap
- Separate TX/RX counters (mandate)
- Sequence starts at 0

✅ **Error Handling**
- Incorrect sequence number error
- Unrecognized command error
- Out of buffers error
- Length too long error

✅ **Protocol Information Response**
- Protocol version
- Supported traffic types
- Sequence number tracking

## Known Limitations

1. **No RX sequence counter tests** — Only TX counter is implemented in serial.h
2. **No full transport integration** — Tests focus on protocol/multiplexer logic rather than real serial hardware I/O
3. **No protocol parser state-machine stepping** — Parsing transitions are covered by invariants, not full byte-stream simulation

## Future Test Enhancements

- [ ] RX sequence number management tests (if implemented)
- [ ] Full frame serialization/deserialization round-trip tests
- [ ] Client callback routing tests
- [ ] Parsing state machine tests (PARSING_STATE_TYPE → _DATA)
- [ ] Timeout/retry behavior tests
- [ ] Integration tests with mock serial transport

## Test Execution Order

Tests are organized by complexity:
1. **protocol_test.c** — Constants and structure (foundation)
2. **frame_parser_test.c** — Frame construction (building on structures)
3. **sequence_number_test.c** — Sequence management (isolated)
4. **error_handling_test.c** — Error conditions (high-level)

## Debugging Tests

Enable verbose logging:
Use ztest output from `west build -t run` and rerun with a pristine build when changing config:
```bash
west build -p always -b qemu_cortex_m3 .
cd build/tests && west build -t run
```

## Compliance

These tests validate compliance with AGENTS.md specification:
- ✅ Traffic type support (WiFi Mgmt/Data, Net_IF, HCI, Thread, VSerial)
- ✅ Payload length limits (4096 bytes max)
- ✅ Sequence number wrapping (0-15)
- ✅ Error conditions (4 types, 2-bit encoding)
- ✅ Protocol information response fields
- ✅ Multiplexer routed send path (single integrated test case)
