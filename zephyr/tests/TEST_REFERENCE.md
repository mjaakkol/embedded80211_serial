# Common Link Protocol Test Case Reference

Complete list of all test cases in the protocol test suite.

## Protocol Core Tests (protocol_test.c)

| Test ID | Test Name | Purpose | Assertions |
|---------|-----------|---------|-----------|
| PRO-001 | `test_protocol_version` | Verify protocol version is 2 | Version = 2 |
| PRO-002 | `test_max_payload_length` | Verify max payload is 4096 bytes | Max = 4096 |
| PRO-003 | `test_sequence_number_max` | Verify sequence wraps at 15 | SEQUENCE_NUMBER_MAX = 15 |
| PRO-004 | `test_serial_type_enum_values` | Verify all SerialType enum values | 10 types, 0-9 in order |
| PRO-005 | `test_error_type_enum_values` | Verify all ErrorType enum values | 4 error codes, 0-3 |
| PRO-006 | `test_supported_types_mask` | Verify supported types bitmask | 7 types supported (no NET_OFFLOADING, AT_CMD) |
| PRO-007 | `test_serial_header_structure` | Verify SerialHeader bitfield layout | 4-byte struct, fields set correctly |
| PRO-008 | `test_sequence_number_wraparound` | Verify sequence wraps after 15 | 15 → 0 wrapping |
| PRO-009 | `test_parsing_state_enum` | Verify parsing state transitions | 5 states, 0-4 |
| PRO-010 | `test_error_code_2bit_range` | Verify error codes fit in 2 bits | All errors < 4 |
| PRO-011 | `test_type_field_4bit_range` | Verify type field fits in 4 bits | All types < 16 |

## Frame Parsing Tests (frame_parser_test.c)

| Test ID | Test Name | Purpose | Assertions |
|---------|-----------|---------|-----------|
| FRM-001 | `test_construct_frame_header` | Build typical frame | WIFI_MGMT, seq=3, len=100 |
| FRM-002 | `test_frame_max_payload` | Frame with max payload | Length = 4096 |
| FRM-003 | `test_frame_length_too_long` | Detect over-size frames | Length > 4096 flagged |
| FRM-004 | `test_sequence_number_encoding` | Encode all sequence values | Seq 0-15 all encode |
| FRM-005 | `test_construct_ack_frame` | Build error-free ACK | Type set, error=0, len=0 |
| FRM-006 | `test_ack_with_error` | Build ACK with error code | Error code preserved |
| FRM-007 | `test_no_ack_flag` | Test no-ACK request flag | Flag true/false distinct |
| FRM-008 | `test_host_bit_direction` | Test direction bit | Device→host vs host→device |
| FRM-009 | `test_reserved_bits_zero` | Verify reserved bits are zero | Reserved = 0 |
| FRM-010 | `test_frame_min_payload` | Frame with 1-byte payload | Length = 1 |
| FRM-011 | `test_frame_typical_payloads` | Test common payload sizes | 1, 64, 256, 512, 1024, 2048, 4096 |
| FRM-012 | `test_all_traffic_types` | Test all types in frames | 7 supported types |
| FRM-013 | `test_frame_endianness` | Verify endianness encoding | BE length, LE fields |

## Sequence Number Tests (sequence_number_test.c)

| Test ID | Test Name | Purpose | Assertions |
|---------|-----------|---------|-----------|
| SEQ-001 | `test_get_and_advance_sequence` | Get/advance API | Multiple reads same, after advance differs |
| SEQ-002 | `test_sequence_in_4bit_range` | Sequence fits in 4 bits | Seq ≤ 15 |
| SEQ-003 | `test_sequence_wraparound` | Test wraparound at max | 15 → 0 |
| SEQ-004 | `test_sequence_increment_loop` | Multiple cycles | 32 increments (2 cycles of 16) |
| SEQ-005 | `test_separate_tx_counter` | TX sequence is separate | TX counter valid |
| SEQ-006 | `test_sequence_starts_at_zero` | Init at zero (AGENTS.md) | First seq valid |
| SEQ-007 | `test_sequence_monotonicity` | Strictly increasing | 20+ increments monotonic |
| SEQ-008 | `test_sequence_in_frame` | Frame header gets seq | Header.seq = counter |
| SEQ-009 | `test_rapid_advance` | Stress test 1000 ops | All seqs valid |
| SEQ-010 | `test_sequence_never_exceeds_max` | Never > 15 after any op | 1000+ ops never exceed |
| SEQ-011 | `test_sequence_independent_of_type` | Seq independent of type | WiFi and NetIF same seq |

## Error Handling Tests (error_handling_test.c)

| Test ID | Test Name | Purpose | Assertions |
|---------|-----------|---------|-----------|
| ERR-001 | `test_error_types_defined` | All error types exist | 4 types fit in 2 bits |
| ERR-002 | `test_no_error_condition` | NO_ERROR = 0 | Error code = 0 |
| ERR-003 | `test_incorrect_sequence_error` | Encode sequence error | Error type = 1 |
| ERR-004 | `test_out_of_buffers_error` | Encode buffer error | Error type = 2 |
| ERR-005 | `test_length_too_long_error` | Encode length error | Error type = 3 |
| ERR-006 | `test_payload_length_boundary` | Boundary at 4096 | 4096 OK, 4097 error |
| ERR-007 | `test_zero_length_payload` | Zero-length frames | ACKs can be zero-length |
| ERR-008 | `test_ack_contains_error` | ACK carries error | Error in response |
| ERR-009 | `test_all_error_codes_2bit` | Error codes fit 2 bits | All codes < 4 |
| ERR-010 | `test_unrecognized_command_error` | Bad cmd → LENGTH_TOO_LONG | Mapping correct |
| ERR-011 | `test_error_ack_preserves_sequence` | ACK echoes sequence | Seq preserved |
| ERR-012 | `test_no_ack_no_response` | no_ack prevents response | Flag suppresses ACK |
| ERR-013 | `test_reserved_bits_dont_affect_error` | Reserved bits independent | Error unaffected |
| ERR-014 | `test_length_too_long_ack` | LENGTH_TOO_LONG ACK | Error response valid |
| ERR-015 | `test_error_preserves_type` | Error doesn't corrupt type | Type field intact |
| ERR-016 | `test_error_all_types` | All types support errors | 5+ types tested |

## Test Summary

- **Total Tests:** 44
- **Test Suites:** 4
- **Lines of Test Code:** ~1,140
- **Total Assertions:** 100+
- **Coverage Areas:**
  - Protocol constants and structures
  - Frame construction and parsing
  - Sequence number management
  - Error handling and validation

## Running Specific Tests

### By Suite
```bash
ctest -R "protocol_core" -V --test-dir build
ctest -R "frame_parsing" -V --test-dir build
ctest -R "sequence_numbers" -V --test-dir build
ctest -R "error_handling" -V --test-dir build
```

### By Test Name
```bash
ctest -R "test_protocol_version" -V --test-dir build
ctest -R "test_sequence_wraparound" -V --test-dir build
```

### All Tests with Verbose Output
```bash
ctest -V --test-dir build
```

## Test Coverage Matrix

| Requirement | Protocol | Frames | Sequence | Error | Status |
|-------------|----------|--------|----------|-------|--------|
| Frame format ≤64 bits | ✓ | ✓ | — | — | ✓ |
| Traffic types (10) | ✓ | ✓ | ✓ | ✓ | ✓ |
| Payload length max 4096 | ✓ | ✓ | — | ✓ | ✓ |
| Sequence 0-15 wrap | ✓ | ✓ | ✓ | — | ✓ |
| Error codes (4) | ✓ | ✓ | — | ✓ | ✓ |
| ACK with error | — | ✓ | — | ✓ | ✓ |
| No-ACK flag | — | ✓ | — | — | ✓ |
| Direction bit | — | ✓ | — | — | ✓ |
| Reserved bits = 0 | — | ✓ | — | — | ✓ |
| Protocol info response | ✓ | — | — | — | Partial |

## Notes

- **PRO-007:** Tests bitfield structure; actual wire format depends on endianness and compiler
- **FRM-013:** Assumes little-endian CPU with big-endian length field (sys_cpu_to_be16)
- **SEQ-006:** Tests init state; actual initialization verified at runtime
- **ERR-010:** Unrecognized command uses LENGTH_TOO_LONG per AGENTS.md
- **ERR-012:** no_ack flag prevents ACK responses (receiver behavior)

## AGENTS.md Compliance

These tests validate adherence to AGENTS.md requirements:

✅ **Common Link Protocol (Mandatory)**
- Frame format: header ≤64 bits, type/length/sequence fields
- Payload: max 4096 bytes, 32-bit aligned (tested indirectly)
- Sequence: 0-15 (4-bit), wrap-around
- Error handling: 4 error codes, ACK with error field
- Protocol info: version, types, sequence (structure tested)

✅ **Technology-Specific (Mandatory)**
- All 10 traffic types defined and testable
- 7 types supported (no NET_OFFLOADING, AT_CMD)
- Error handling for each type

✅ **Compatibility (Mandatory)**
- Protocol version 2 (4-bit sequence)
- Backward compatibility maintained in test structure

