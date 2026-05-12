# Embedded 802.11 Serial

## Scope
This file defines product-level requirements and protocol invariants for the full repository.
Detailed platform implementation rules belong to platform-specific AGENTS files.

## Product Goals (Mandatory)
- Provide a Radio Co-Processor solution for Zephyr-based microcontrollers.
- Support Wi-Fi, Bluetooth, and OpenThread technology domains.
- Support serial transports: SDIO, UART, SPI, and USB.
- Support host/platform integration for Linux and Zephyr.

## Common Link Protocol (Mandatory)

### Transport Multiplexing
- One common link protocol must be used over supported serial transports.
- Traffic classes supported by the protocol:
	- Wi-Fi management
	- Wi-Fi data
	- Network L2 interface management
	- Bluetooth
	- OpenThread
	- Network offloading (reserved)
	- AT commands (reserved)
	- Protocol information
	- Virtual serial interfaces
- Maximum number of traffic types is 16.

### Frame Format
- Protocol header must be at most 64 bits.
- Header must include:
	- Traffic type
	- Payload length (max 4096 bytes)
	- Sequence number
- Payload data must be 32-bit aligned.

### Sequencing and Reliability
- Sequence number range is 0..15 (4-bit), wrapping after 15.
- Sequence numbering starts at 0 on system initialization.
- Host-to-device and device-to-host use separate sequence counters.
- Receiver must acknowledge packets with sequence number and optional error code.

### Error Handling
- Supported protocol error conditions:
	- Incorrect sequence number
	- Unrecognized command
	- Out of buffers
	- Length too long
- On incorrect sequence number response, sender resets expected sequence to 0 and may resend.

### Protocol Information Response
- Must include:
	- Protocol version
	- Supported traffic types
	- Next sequence number

## API Mapping Requirements (Mandatory)
- Wi-Fi management messages map to Zephyr Wi-Fi management APIs.
- Network L2 interface management messages map to Zephyr net_if APIs:
	https://docs.zephyrproject.org/latest/doxygen/html/group__net__if.html
- Protobuf must be used as the message schema language: https://protobuf.dev/

## Technology-Specific Rules (Mandatory)
- Wi-Fi data traffic is carried after the common protocol header.
- Bluetooth traffic uses HCI encapsulated by the common protocol header.
- OpenThread traffic uses Spinel encapsulated by the common protocol header.
- Virtual serial functionality is provided over the common protocol and exposes serial-port semantics.
- For virtual serial APIs, baud rate, stop bits, and parity are ignored for transport behavior, but values must be shadowed for readback compatibility.
- Network offloading traffic ID is reserved and currently not implemented.
- AT command traffic ID is reserved and currently not implemented.

## Compatibility and Evolution Policy (Mandatory)
- Changes that alter wire behavior must preserve backward compatibility when feasible.
- Breaking wire-format changes require a protocol version update.
- Protobuf evolution rules:
	- Never renumber existing fields.
	- Use new field numbers for new fields.
	- Reserve removed field numbers when possible.

## Quality Gates (Recommended)
- Validate malformed frame handling for length, alignment, and sequence checks.
- Avoid unnecessary data copies in hot data paths.
- Document any protocol-level behavior change in commit messages or PR descriptions.

