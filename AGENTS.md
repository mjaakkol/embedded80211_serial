# Embedded 802.11 Serial

## Scope and hierarchy

These are repository-wide requirements. More-specific `AGENTS.md` files may add
constraints for their directory, but must not weaken these requirements:

- [linux/AGENTS.md](linux/AGENTS.md)
- [embedded80211/AGENTS.md](embedded80211/AGENTS.md)
- [embedded80211/zephyr/device/AGENTS.md](embedded80211/zephyr/device/AGENTS.md)
- [embedded80211/tests/AGENTS.md](embedded80211/tests/AGENTS.md)
- [nrf7002dk/AGENTS.md](nrf7002dk/AGENTS.md)
- [host/AGENTS.md](host/AGENTS.md)
- [cli_test/AGENTS.md](cli_test/AGENTS.md)

Keep changes focused, preserve existing behavior unless the task requires a
behavior change, and update the nearest relevant README or protocol
documentation when externally visible behavior changes.

## Product architecture

- Provide a Radio Co-Processor solution for Zephyr-based microcontrollers.
- Support Wi-Fi, Bluetooth, and OpenThread.
- Support SDIO, UART, SPI, and USB serial transports through one common link
  protocol.
- Support Linux and Zephyr host integration.
- Keep transport adaptation, common framing, technology protocols, and host
  APIs separated so each layer can be tested independently.

## Common link protocol

- The frame header is no larger than 64 bits and includes traffic type, payload
  length, and sequence number.
- Payload length is at most 4096 bytes; payloads are 32-bit aligned.
- Traffic types use at most 4 bits. Existing assignments are stable:
  Wi-Fi management, Wi-Fi data, network-interface management, Bluetooth HCI,
  OpenThread Spinel, network offloading (reserved), AT commands (reserved),
  protocol information, and virtual serial.
- Sequence numbers are 4-bit values in the range 0..15 and wrap after 15.
  Host-to-device and device-to-host counters are independent and start at zero.
- Receivers acknowledge frames with the received sequence number and an
  optional protocol error.
- Protocol errors include incorrect sequence, unrecognized command, out of
  buffers, and excessive length. An incorrect-sequence response resets the
  sender's expected sequence to zero; resend behavior must be explicit.
- Protocol-information responses include protocol version, supported traffic
  types, and the next sequence number.

## Technology and schema rules

- Wi-Fi management maps to Zephyr Wi-Fi management APIs.
- Network-interface management maps to Zephyr `net_if` APIs.
- Wi-Fi data follows the common header directly.
- Bluetooth uses HCI and OpenThread uses Spinel inside common frames.
- Virtual serial preserves baud rate, stop-bit, and parity values for readback,
  but those values do not control transport behavior.
- Network offloading and AT-command traffic remain reserved until implemented.
- Protobuf is the schema language. Never renumber existing fields; use new
  field numbers for additions and reserve removed numbers where possible.
- Wire-compatible changes are preferred. A breaking wire change requires a
  protocol-version update and compatibility documentation.

## Cross-cutting quality requirements

- Treat serial and device-provided data as untrusted.
- Validate lengths, alignment, traffic types, reserved bits, direction,
  acknowledgement state, sequence state, and protocol version before decoding.
- Avoid unnecessary copies in hot paths and bound all allocations from wire
  input.
- Return or report failures explicitly; do not silently downgrade malformed
  input to a successful result.
- Add focused tests for every framing, sequencing, error, schema, or transport
  behavior change.

## Security

- Keep code, examples, README, test assets and Githubs free from personal identifiable data. For example, SSID and passwords used as part of agentic testing shall not be left into the code or part of git commits.
- Never use length or attributes from externally interfacing directly without checking boundary conditions.