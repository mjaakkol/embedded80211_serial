# Rust CLI integration instructions

## Scope

These rules apply to the Rust command-line test tool. Shared protocol
requirements are defined in [../AGENTS.md](../AGENTS.md).

## Design

- Use Rust 2024 and an external serial-port abstraction.
- Keep `main.rs` limited to argument parsing, transport setup, command
  dispatch, and user-facing formatting. Put reusable framing, protocol, and
  request/response logic in the library.
- Keep transport-independent framing testable through `Read`/`Write`-style
  abstractions rather than requiring a physical serial port.
- Handle partial reads and writes explicitly. Read the fixed header first,
  validate its length, then read exactly the payload.
- Give each device operation a total bounded deadline. A serial read timeout
  is an error unless the operation has completed.
- Return typed errors for transport, timeout, framing, sequence, protocol, and
  protobuf failures. Device-facing failures must exit nonzero.
- Treat device input as untrusted. Do not use `unwrap`, `expect`, or unchecked
  indexing on device-facing paths.

## Configuration and compatibility

- Add Cargo features only for real transport or test variants and document
  each feature.
- Preserve traffic IDs, header fields, sequence behavior, and protobuf field
  numbers. Update protocol-version handling when wire compatibility changes.
- Update [README.md](README.md) for command, option, protocol, or error-message
  changes.

## Verification

From `cli_test`, run:

```text
cargo fmt --check
cargo clippy --all-targets --all-features -- -D warnings
cargo test
```

Protocol changes need focused tests for header encoding/decoding, traffic and
error values, sequence wraparound, malformed/truncated/oversized frames,
partial reads, timeout handling, and protobuf round trips.
