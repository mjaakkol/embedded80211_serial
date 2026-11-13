# ADR 0001: Common link protocol v1

- **Date**: 2025-03-31
- **Status**: accepted

## Context

The project multiplexes Wi-Fi management, Wi-Fi data, network-interface
management, Bluetooth HCI, OpenThread Spinel, and a virtual serial port over
one serial interface. The link layer needed a small, fixed-size header that
any MCU can parse with minimal RAM, plus ordering and acknowledgement
semantics over a medium with no built-in reliability.

## Decision

- 64-bit (4-byte) header: traffic type (4 bits), no-ack (1), host direction
  (1), error (2), reserved (4), seq (4), length (16, big-endian). Payloads
  are 0..4096 bytes and 32-bit aligned.
- Traffic type is a 4-bit enum; assignments are stable (WIFI_MGMT, WIFI_DATA,
  NET_IF_MGMT, BT_H4, THREAD, NET_OFFLOADING reserved, AT_CMD reserved,
  PROTOCOL_INFO, VSERIAL).
- Sequence numbers are 4-bit, wrap after 15, and are independent per
  direction, both starting at zero.
- Every frame is acknowledged with a length-0 frame of the same traffic type
  carrying a 2-bit error (NO_ERROR, INCORRECT_SEQUENCE, OUT_OF_BUFFERS,
  LENGTH_TOO_LONG/unrecognized). An incorrect-sequence response resets the
  sender's expected sequence to zero; the sender resends explicitly.
- The `no_ack` bit is reserved and not implemented in v1: effectively one
  in-flight frame per direction.
- Connection starts with a length-0 `PROTOCOL_INFO` query answered by a
  4-byte device-configuration packet (version, traffic-type mask, next seq)
  that consumes no sequence slot.

Embodied in
`embedded80211/zephyr/include/embedded80211/serial.h` and the
`embedded80211/zephyr/device/` multiplexer + `embedded80211/zephyr/host/`
bus client; the spec is mirrored in
[../architecture.md](../architecture.md) §5.

## Consequences

- The header is trivially parseable on both ends with a 32-bit-aligned
  payload read; framing never needs to cross buffer boundaries in code.
- The strict per-direction ACK scheme caps throughput (one in-flight frame
  per direction) — accepted for a development protocol; a higher-throughput
  revision (no_ack) can drop in without API changes.
- The wire format is invariant: any breaking change requires a
  protocol-version update and compatibility documentation.
- The `PROTOCOL_INFO` mask makes unsupported traffic types fail clearly at
  enumeration time on any host.
