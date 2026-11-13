# ADR 0003: Split device stack into library, board app, and QEMU host app

- **Date**: 2026-11-07
- **Status**: accepted

## Context

The original codebase mixed the link protocol, device-side transport,
technology proxies, and host-side drivers in one application. This made it
impossible to build a host against the same protocol library, to run the
protocol tests against a fake transport, or to reuse the stack on other
boards.

## Decision

The "e80211-refactor" split the stack into three consumers of one shared
library (commit `2b91e6a`):

- `embedded80211/` — Zephyr **module** (via `ZEPHYR_EXTRA_MODULES`) holding
  the common link protocol, device-side transport + technology proxies
  (gated on `CONFIG_SERIAL_DEVICE_ROLE`), and Zephyr host-side drivers
  (gated on `CONFIG_EMBEDDED80211_HOST_*`), plus the ztest protocol suite in
  `tests/`.
- `nrf7002dk/` — thin **board application** that instantiates the module in
  the device role on the nRF7002 DK.
- `host/` — **QEMU host application** (`qemu_cortex_m3`) instantiating the
  host-side drivers over a UART, plus the end-to-end test.

A single build links exactly one role.

## Consequences

- Each layer is independently testable: the ztest suite runs the protocol
  without hardware; the host app pairs against the device over a real bus.
- The board app stays small and board-agnostic changes land in the module.
- Consumers must add the module to `ZEPHYR_EXTRA_MODULES` and the host app
  must build with `--no-sysbuild` (NCS quirk).
- The Linux driver is planned as a fourth consumer of the same wire protocol
  (see [linux/README.md](../../linux/README.md)).
