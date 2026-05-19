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

### Architecture
- Split reusable logic into a library module and keep the CLI entrypoint thin.
- Keep common link protocol behavior transport-independent and isolated from serial I/O.
- Ensure every device-facing operation has a bounded timeout and surfaces timeout failures clearly.

### Configuration
- Use Cargo feature flags only when they map to real bearer or transport variants.
- Keep feature-gated behavior narrowly scoped and documented.

## Preferred Patterns (Not Absolute)
- Keep domain-specific logic in separate source files.
- Favor stable module boundaries over file-location mandates; do not hardcode implementation to one file if refactoring improves clarity.
- Prefer small, testable helpers for wire-format construction and parsing.

## Verification Expectations
- Run `cargo check` or `cargo test` in `cli_test` after changes.
- For protocol, framing, or serialization changes, add or update focused Rust tests.
- If full runtime validation is not possible, document what was verified and the remaining risk.

## Change Safety Checklist
- Confirm no regressions in sequence, ack, or error handling paths when touching protocol logic.