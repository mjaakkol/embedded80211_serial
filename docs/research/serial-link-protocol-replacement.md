# Research: replacing the common link protocol with an off-the-shelf library

Status: research note (pre-decision). Not an ADR — no decision has been made.
Date: 2026-11-13.

## Question

Is there a good open-source library that can replace the project's common
serial link protocol (v1, `embedded80211/zephyr/include/embedded80211/serial.h`)?
Hard requirements:

1. Port for **Zephyr** (device + reference host, C).
2. **Linux kernel-mode** (the Linux host is a cfg80211 kernel driver).
3. **Rust** (the `cli_test` harness is Rust).
4. **Low overhead** (current: 4-byte header, one in-flight frame per direction).
5. **Detect message corruption**.
6. **Detect out-of-order transmission**.

## What the current protocol actually provides

- 4-byte big-endian header: 4-bit traffic type, 2-bit error, 4-bit sequence
  (0..15, wraps), 16-bit length (0..4096, 32-bit aligned).
- **Ordering / out-of-order detection**: the 4-bit sequence + per-direction
  ACK + `INCORRECT_SEQUENCE` resync already do this.
- **Corruption detection**: **structural only** (length bounds, 32-bit
  alignment, reserved-bit and traffic-type checks). There is **no
  checksum/CRC** on the payload. This is the real gap.
- Backpressure via `OUT_OF_BUFFERS`; explicit resend on error.

So a "replacement" must keep multiplexing + ordering + ACK/backpressure and
would need to *add* real corruption detection.

## Key structural finding

**No single off-the-shelf library satisfies all six criteria**, because two of
them pull in opposite directions:

- **"Linux kernel-mode" excludes userspace libraries.** You cannot link a
  userspace C library (nng, ZeroMQ, LwIP, any Rust crate) into the kernel. A
  "kernel-mode port" effectively means *a libc-free C implementation compiled
  into the kernel*, i.e. you use the kernel's own primitives (kernel crypto
  for CRC, kernel net for PPP/HDLC) rather than an external library.
- **"Out-of-order detection" (sequence numbers) is not a link-layer feature of
  any standard protocol.** Ethernet, CAN, PPP and HDLC all rely on the physical
  layer for ordering; sequence numbers are a *transport* (TCP) feature. So the
  requirement points at a small custom framing, not a standard link protocol.

Rust-in-the-kernel is now official (the "Rust experiment" was concluded in
Dec 2025), but the ecosystem of reusable framing/messaging *libraries* for
kernel Rust is effectively empty. So "Rust" here realistically means
**userspace Rust** (the CLI harness), not a shared kernel-Rust library.

The realistic shape of any solution is therefore: **a small libc-free C core
(used by both Zephyr and the Linux kernel) + a userspace Rust binding**, with
the kernel's built-in CRC for the integrity part. That is exactly the shape the
project already has (custom C protocol + Rust CLI).

## Candidate evaluation

Legend: ✔ yes, ✘ no, ◐ partial / not fit.

| Candidate | Zephyr | Linux kernel | Rust | Low overhead | Corruption (CRC) | Out-of-order (seq) | Verdict |
|-----------|:------:|:------------:|:----:|:------------:|:----------------:|:------------------:|---------|
| **PPP / HDLC** | ✔ | ✔ | ✘ | ◐ | ✔ (FCS CRC-16) | ✘ | Closest "real protocol", but no seq, no Rust, IP-oriented, two separate codebases |
| **nng (nanomsg)** | ✘ | ✘ (userspace) | ✔ | ✔ | ✘ | ✘ | Messaging lib, not a link protocol |
| **ZeroMQ** | ✘ | ✘ (userspace) | ✔ | ◐ | ✘ | ✘ | Heavier, same problem |
| **LwIP** | ✔ | ✘ | ✘ | ✘ (~36 KB+ TCP/IP) | ✔ (IP/TCP csum) | ✔ (TCP, transport) | Full network stack — wrong abstraction, too heavy |
| **CAN** | ✔ | ✔ | ✔ | ✔ | ✔ (15-bit CRC) | ✘ (bus, 64 B cap) | Bus protocol, no seq, payload limit |
| **`secure-serial`** (Rust) | ✘ (Embassy, no C core) | ✘ | ✔ | ✔ | ✔ (CRC-32) | ◐ (chunk id/index, no monotonic seq) | Closest in spirit, but Rust-only, Embassy, v0.1 |
| **`rm-frame`** (DJI RoboMaster, Rust) | ✘ | ✘ | ✔ | ✔ | ✔ (CRC) | ✔ (8-bit SEQ) | Rust-only, vendor-specific, no C/kernel core |
| **CRC32C primitive** | ✔ `crc32_c()` | ✔ `crc32c()` | ✔ `crc32c` crate | ✔ (4 B trailer) | ✔ (its purpose) | n/a (pair with existing seq) | **The missing building block** |

### Notes on the near-misses

- **PPP/HDLC** is the only "real" link protocol present in *both* the Linux
  kernel (`drivers/net/ppp/*`, `drivers/net/hdlc.c`) and Zephyr (Zephyr PPP),
  and it does carry an FCS. But it has **no sequence numbers** (ordering is
  delegated to IP), **no Rust**, is **designed to carry IP** (not to multiplex
  arbitrary radio traffic types), and the Linux and Zephyr implementations are
  **separate codebases** — so it is not a shared "library" and does not meet
  the ordering requirement.
- **nng / ZeroMQ** are userspace messaging libraries: no kernel-mode, no Zephyr
  port, no CRC, no link-layer sequence. They solve a different problem (IPC /
  pub-sub), not a raw-link framing problem.
- **LwIP** gives you sequence numbers (TCP) and checksums, but it is a full
  TCP/IP stack (~36 KB flash + ~10 KB RAM), has no kernel-mode and no
  first-class Rust, and is the wrong abstraction level for a link protocol.
- **`secure-serial`** (Rust, `no_std` + Embassy) is the closest in *spirit*:
  magic + length + type, CRC-32 trailer, chunked reassembly, per-chunk ACK and
  retransmission, bounded buffers. It is **Rust-only** (no C/kernel/Zephyr
  core), targets **Embassy** (not Zephyr), is **v0.1**, and its ordering is
  chunk-id/index based rather than a monotonic per-packet sequence number.
  Useful as a design reference, not a drop-in.

## Recommendation

**Do not adopt a full off-the-shelf protocol library. Keep the existing
thin framing (4-byte header + 4-bit sequence + ACK) and add CRC32C for
corruption detection.**

Rationale:

- The ordering / out-of-order / ACK / backpressure machinery already exists and
  is correct; no library provides it at the link layer anyway.
- The one genuine gap — corruption detection — is solved by a single,
  widely-audited primitive, **CRC32C (Castagnoli)**, which is natively
  available in **all three** required environments:
  - **Linux kernel**: `crc32c()` — `lib/crc32c.c`, `include/linux/crc32c.h`
    (hardware-accelerated on x86 SSE4.2 and ARM).
  - **Zephyr**: `crc32_c()` — `zephyr/sys/crc.h` (`CRC32_C`, poly
    `0x1EDC6F41`), available since Zephyr 1.8; software fallback, HW-accelerated
    where the CPU has the CRC instruction (Cortex-M33/M55 ARMv8-M CRC).
  - **Rust**: the `crc32c` crate (with SSE4.2 / ARM acceleration).
- Overhead is a **4-byte trailer per frame** (same order as Ethernet FCS / USB
  CRC), no heap allocation, no per-frame bookkeeping beyond the CRC register.
  This meets the low-overhead requirement and is *less* overhead than any of
  the full-stack candidates.

### Concrete change (if pursued)

1. Append a 4-byte little- or big-endian **CRC32C** over header + payload to
   each frame (define endianness once; the rest of the header is big-endian).
2. On RX, compute and compare before decoding; on mismatch, answer with the
   existing error path (a new `BAD_CRC`/reuse `LENGTH_TOO_LONG`-style code) and
   let the sender's existing explicit-resend logic retransmit.
3. Use `crc32c()` (kernel), `crc32_c()` (Zephyr), `crc32c` crate (Rust) so all
   three sides compute the identical value.
4. This is a **wire-protocol change** → requires a **protocol-version bump** and
   compatibility documentation per the repo's non-negotiable constraints.

### If a "standard" framing is ever wanted

HDLC (flag byte `0x7E` + byte-stuffing + FCS) is the standard used by PPP and
Spinel, but it is less efficient than length-based framing for the binary
payloads this link carries (Ethernet frames, protobuf, raw HCI) and adds no
sequence numbers. The current length-based framing is the better fit; adopting
HDLC would not improve the corruption or ordering story.

## Sources

- Zephyr CRC API (`crc32_c`, `CRC32_C`, `CRC32C_POLY`):
  https://docs.zephyrproject.org/latest/doxygen/html/group__crc.html
- Zephyr PPP support:
  https://docs.zephyrproject.org/latest/services/connectivity/networking/api/ppp.html
- Linux kernel PPP channel interface:
  https://docs.kernel.org/networking/ppp_generic.html
- Rust-in-kernel experiment concluded (Dec 2025):
  https://lwn.net/Articles/1049831
- google/crc32c (reference CRC32C, CPU acceleration):
  https://github.com/google/crc32c
- nng (userspace messaging, no Zephyr/kernel port):
  https://github.com/nanomsg/nng
- `secure-serial` crate (Rust, no_std + Embassy, CRC-32 + ACK):
  https://docs.rs/crate/secure-serial/0.1.0/source/README.md
- `rm-frame` crate (DJI RoboMaster framing, Rust):
  https://crates.io/crates/rm-frame
- Embedded TCP/IP stack footprints (LwIP et al.):
  https://mongoose.ws/embedded-tcpip-stack-comparison/
