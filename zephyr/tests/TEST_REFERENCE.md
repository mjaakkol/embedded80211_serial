# Common Link Protocol Test Case Reference

Complete list of current test cases in the protocol test suite.

## Protocol Core Tests (protocol_test.c)

| Test ID | Test Name | Purpose | Assertions |
|---------|-----------|---------|-----------|
| PRO-001 | `test_protocol_version` | Verify protocol version constant | Version = 1 |
| PRO-002 | `test_max_payload_length` | Verify max payload is 4096 bytes | Max = 4096 |
| PRO-003 | `test_sequence_number_max` | Verify sequence wraps at 15 | SEQUENCE_NUMBER_MAX = 15 |
| PRO-004 | `test_serial_type_enum_values` | Verify all SerialType enum values | 10 types, 0-9 in order |
| PRO-005 | `test_error_type_enum_values` | Verify all ErrorType enum values | 4 error codes, 0-3 |
| PRO-006 | `test_supported_types_mask` | Verify supported types bitmask API | Includes PROTOCOL_INFO bit |
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
| SEQ-001 | `test_sequence_wrap_math` | Test wraparound math at max | 15 → 0 after 16 steps |
| SEQ-002 | `test_sequence_field_encode_decode` | Sequence in header field | Seq 0..15 round-trip in bitfield |
| SEQ-003 | `test_sequence_per_config_independence` | Per-config sequence isolation | Two configs keep independent values |

## Multiplexer Link Protocol Test (multiplexer_link_protocol_test.c)

| Test ID | Test Name | Purpose | Assertions |
|---------|-----------|---------|-----------|
| MUX-001 | `test_multiplexer_link_protocol_single_case` | Integrated multiplexer route path | Protocol-info mask bit, route failure without iface, mock iface registration, routed send success and payload/config integrity |

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

- Total Tests: 44
- Test Suites: 5
- Coverage Areas:
  - Protocol constants and structures
  - Frame construction and parsing
  - Sequence number behavior
  - Multiplexer routed send path
  - Error handling and validation

## Running the Suite

```bash
export ZEPHYR_BASE=/home/mikko/ncs/v3.4.1/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/home/mikko/ncs/toolchains/8285d8ad56/opt/zephyr-sdk
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

west build -p always -b qemu_cortex_m3 .
cd build/tests && west build -t run
```
