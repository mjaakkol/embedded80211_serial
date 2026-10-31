# cli_test AGENTS Instructions

## Scope
This file defines implementation guidance for the Rust command-line test tool in this repository.
Root-level product constraints are defined in the top-level AGENTS file.

## Project Context
This package is a Rust CLI used to validate device communication and protocol integration.
- It exercises protobuf-defined APIs against the device.
- It manages serial communication in a cross-platform way through Rust abstractions.

## Mandatory Rules

### Coding and APIs
- Use Rust 2024 edition and idiomatic Rust conventions.
- Use an external crate for serial-port abstractions.
- Keep protocol parsing and framing logic reusable from both the CLI and tests.
- Treat all device input as untrusted.
- Use typed errors for transport, timeout, framing, sequencing, and protobuf failures.
- Do not use `unwrap`, `expect`, or unchecked indexing in device-facing code.

### Architecture
- Split reusable logic into a library module and keep the CLI entrypoint thin.
- Keep common link protocol behavior transport-independent and isolated from serial I/O.
- Keep request/response orchestration separate from CLI argument parsing and output formatting.
- Define transport interfaces around readable and writable abstractions so framing tests do not require a physical serial port.
- Ensure every device-facing operation has a total bounded deadline and surfaces timeout failures clearly.
- Handle partial reads and writes explicitly; a serial read timeout must not be treated as successful completion.
- Failed commands must produce a diagnostic and exit with a nonzero status.

### Configuration
- Use Cargo feature flags only when they map to real bearer or transport variants, such as a supported serial transport or test transport.
- Keep feature-gated behavior narrowly scoped and documented.

### Protocol Safety
- Preserve the root-level traffic IDs, header layout, sequence semantics, acknowledgement behavior,
  payload limits, alignment requirements, and protobuf evolution rules.
- Parse frames incrementally: read and validate the fixed header before reading the declared payload.
- Reject short, oversized, misaligned, unsupported, and invalid-sequence frames.
- Validate the declared payload length before allocating or decoding.
- Do not change traffic IDs, header fields, sequence behavior, or protobuf field numbers without
  preserving compatibility and updating the protocol version when wire behavior changes.

## Preferred Patterns (Not Absolute)
- Keep domain-specific logic in separate source files.
- Favor stable module boundaries over file-location mandates; do not hardcode implementation to one file if refactoring improves clarity.
- Prefer small, testable helpers for wire-format construction and parsing.
- Prefer explicit operation deadlines and reusable frame-reader/frame-writer helpers over one-shot reads into fixed-size buffers.

## Verification Expectations
- From `cli_test`, run `cargo fmt --check`, `cargo clippy --all-targets --all-features -- -D warnings`, and `cargo test` after changes.
- For protocol, framing, serialization, sequencing, or timeout changes, add or update focused Rust tests.
- Cover header encoding and decoding, traffic types, error codes, sequence wraparound, malformed and truncated frames, oversized payloads, partial reads, timeout handling, and protobuf round trips.
- If full runtime validation is not possible, document what was verified and the remaining risk.
- Update `cli_test/README.md` when command behavior, options, protocol behavior, or error handling changes.

## Change Safety Checklist
- Confirm no regressions in sequence, acknowledgement, error handling, timeout, and malformed-frame paths when touching protocol logic.
- Confirm that device-facing failures are reported clearly and return a nonzero process status.
