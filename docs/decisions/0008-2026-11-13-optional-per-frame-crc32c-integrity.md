# ADR 0008: Optional per-frame CRC32C integrity

- **Date**: 2026-11-13
- **Status**: accepted

## Context

The common link protocol (ADR 0001) provides ordering and acknowledgement
(4-bit per-direction sequence, per-frame ACK) but **no integrity check**: a
corrupted frame in transit is indistinguishable from a valid one. Corruption
is only caught structurally — a bad length, a reserved-bit violation, or a
sequence gap. A payload bit flip that keeps the length and sequence valid is
silently delivered to the technology proxy.

The project targets three link-protocol implementations that must stay in
lockstep: the Zephyr device, the Zephyr/Linux host, and the Rust CLI harness.
Any integrity mechanism therefore needs a primitive available on all three
with identical output, at negligible cost, and without forcing a
protocol-version bump (the project is not yet in production, and the framing
is a non-negotiable invariant).

## Decision

Add an **optional, runtime-enabled** per-frame CRC32C trailer. It is additive
and off by default, so it is *not* a protocol-version change — frames with the
bit clear are byte-identical to the original v1 format, and `SERIAL_PROTOCOL_VERSION`
stays `1`.

- **Wire format**: the previously-reserved header bit 8 (`crc`) selects the
  feature. When set, a 4-byte **big-endian CRC32C** is appended after the
  payload. The remaining header bit 0 of byte 1 (`reserved`) is reduced from
  4 to 3 bits.
- **CRC parameters**: CRC32C (Castagnoli / iSCSI) — polynomial `0x1EDC6F41`,
  reflected, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. Computed over the
  4-byte header **plus** the payload.
- **Primitives** (all produce the identical value; check vector
  `crc32c("123456789") = 0xE3069283` is pinned by unit tests on each side):
  - Zephyr device + host: `crc32_c()` (`<zephyr/sys/crc.h>`), shared helper
    `serial_frame_crc()` in `serial.h`.
  - Linux kernel: `crc32c()` (`<linux/crc32c.h>`).
  - Rust: the `crc32c` crate.
- **Runtime opt-in**: each endpoint enables the feature independently.
  - Device: `struct serial_config.crc_enabled` + `serial_set_crc_enabled()`.
  - Host: `bus_set_crc_enabled()`.
  - Rust: `Session::set_crc_enabled()` (a `crc` parameter threads through
    `Frame::encode_host_frame` / `protocol_info_query`).
  A sender only appends the trailer and sets the bit when it has opted in; a
  receiver verifies only when the bit is set.
- **Rejection on mismatch**: a receiver that verifies a CRC and finds a
  mismatch **drops** the frame (no decode, no dispatch to the proxy) and does
  **not** advance its expected sequence. The device replies with an error ACK
  carrying the unchanged expected sequence. Because the frame was not
  accepted, the link stays in sync and the sender simply retransmits.
- **Error code**: the device signals a CRC mismatch with the existing
  `LENGTH_TOO_LONG` code — repurposed as the generic "frame not accepted"
  code (it already doubles for unrecognized traffic type). The host bus
  (`bus_request`) and the Rust harness (`Session::request`) treat
  `LENGTH_TOO_LONG` like `INCORRECT_SEQUENCE`: exactly one resync + retry.

Embodied in `embedded80211/zephyr/include/embedded80211/serial.h` and
`bus.h` (header bit + `serial_frame_crc`),
`embedded80211/zephyr/device/serial/serial.c` and
`embedded80211/zephyr/host/bus.c` (TX trailer, RX verify + reject),
`cli_test/src/protocol.rs` (Rust framing + session), and the framing unit
tests. Spec mirrored in [../architecture.md](../architecture.md) §5.7.

## Consequences

- **Corruption detection closes the main integrity gap** with a 4-byte,
  ~32-cycle-per-frame cost, only when an endpoint opts in. Ordering/ACK
  behavior is unchanged.
- **No version bump**: a peer that never enables the feature is unaffected;
  the two peers must both opt in to gain the protection, which is the intended
  deployment model for a development link.
- **`LENGTH_TOO_LONG` is now an overloaded "frame not accepted" code.** A
  genuine oversize payload, an unrecognized traffic type, and a CRC mismatch
  are indistinguishable to the sender. Accepted: the sender's only viable
  reaction to all three is the same (resync + one retry), and the host
  validates length before sending, so an oversize frame never originates from
  a well-behaved host.
- **Bit 0 of byte 1 is now meaningful.** Any future header field must use the
  remaining 3 reserved bits; the reserved-bit validation mask drops from
  `0x0F` to `0x0E` in all three implementations.
- **Follow-up**: if a future protocol revision adds a dedicated
  "integrity error" code, the `LENGTH_TOO_LONG` overloading can be retired
  without a wire break (it is only a 2-bit value already in use).
