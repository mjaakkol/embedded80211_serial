# ADR 0007: Device console on vserial; RTT for development logs

- **Date**: 2026-11-08
- **Status**: accepted

## Context

The DK exposes two USB CDC-ACM interfaces. The protocol bus needs one
(`CDC_ACM_0`). The device's own console/log path needed a home that does not
contend with the bus and does not depend on the J-Link being attached.

## Decision

- The Zephyr console is the **vserial** virtual UART
  (`chosen zephyr,console = &vserial`). vserial is a virtual null-modem port
  in the link protocol (traffic type 8); console writes are dropped by its
  poll stub, so the console is effectively **silent by design** on the DK.
- Development logs flow over **SEGGER RTT** (`CONFIG_RTT_CONSOLE=y` /
  `CONFIG_USE_SEGGER_RTT=y`), readable over SWD without the target USB.
  On the Mac (no RTT client CLI) the RTT buffer is dumped with
  `JLinkExe mem8` (control block `0x20020040`, up-buffer `0x20000040`, 128
  KB).
- `CDC_ACM_1` is the spare interface, exposed but unbound by default.

## Consequences

- The protocol bus and the log path never share a physical interface; the
  bus stays deterministic for E2E testing.
- Anyone debugging must know the console is silent and read RTT; a host that
  opens the vserial port gets a best-effort console, never on the Wi-Fi
  critical path (see [linux/README.md](../../linux/README.md) §3.6).
- Baud/stop/parity on vserial are shadowed for readback compatibility only
  and do not control transport behavior (protocol rule).
