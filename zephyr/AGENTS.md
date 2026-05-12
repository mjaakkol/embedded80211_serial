# Zephyr AGENTS Instructions

## Scope
This file defines Zephyr-specific implementation guidance for this repository.
Root-level product constraints are defined in the top-level AGENTS file.

## Project Context
The Zephyr part of the project implements:
- Device-side functionality where a host commands Wi-Fi/BT connectivity over serial.
- Zephyr host-driver functionality to access a device over serial.
- Feature enablement through Kconfig options for host/device roles.

## Mandatory Rules

### Coding and APIs
- Follow Zephyr upstream coding style and C best practices.
- Keep networking data path efficient; avoid unnecessary memory copies.
- Use Protobuf with nanopb in Zephyr:
    https://jpa.kapsi.fi/nanopb/docs/

### Architecture
- Serial interfaces (SDIO/USB/UART/SPI) must expose a common abstraction via shared headers.
- Common link protocol behavior must remain transport-independent.
- Transport-specific adaptation must stay isolated from protocol core logic.
- Wi-Fi management path must decode protobuf requests, call Zephyr Wi-Fi management APIs, and encode protobuf responses/events.
- Wi-Fi data path should bypass management proxy logic and flow directly to Zephyr networking path.

### Configuration
- Host/device behavior toggles must be controlled through Kconfig.
- New optional features must include Kconfig guard(s) and sensible defaults.

### Files not to be modified
- network_interface.proto
- wifi.proto

## Preferred Patterns (Not Absolute)
- Keep technology-specific logic in separate source files by domain (Wi-Fi mgmt, Wi-Fi data, BT, OT, serial adapters).
- Use dedicated worker context (thread/workqueue) for blocking proxy operations when needed.
- Favor stable module boundaries over file-location mandates; do not hardcode implementation to one file if refactoring improves clarity.

## Verification Expectations
- For protocol or proxy behavior changes, run a Zephyr build for the active target configuration.
- For message schema changes, verify nanopb generation and compile success.
- For link protocol changes, run loopback or equivalent integration test where available.
- If full runtime validation is not possible, document what was verified and remaining risk.

## Change Safety Checklist
- Confirm protobuf field additions are backward compatible.
- Confirm Kconfig options are documented and wired correctly.
- Confirm buffer sizes and length checks on all externally sourced payloads.
- Confirm no regressions in sequence/ack/error handling paths when touching protocol logic.