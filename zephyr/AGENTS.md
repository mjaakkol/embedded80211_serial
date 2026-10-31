# Zephyr integration instructions

## Scope

These rules apply to device-side and Zephyr host integration. Shared product
and wire-protocol requirements are in [../AGENTS.md](../AGENTS.md).

## Architecture

- Keep the common link protocol independent of UART, USB, SPI, and SDIO
  drivers. Transport adapters belong below the multiplexer.
- Keep Wi-Fi management, Wi-Fi data, Bluetooth HCI, OpenThread Spinel, virtual
  serial, and network-interface management paths separated by domain.
- Wi-Fi management requests decode protobuf messages, call Zephyr management
  APIs, and encode responses or asynchronous events.
- Wi-Fi data should use the Zephyr networking path directly rather than the
  management proxy.
- Keep blocking work out of interrupt context; use a thread or workqueue when
  an operation can block.

## Zephyr and configuration

- Follow Zephyr C style, API ownership rules, logging conventions, and
  `__ASSERT`/error-code conventions.
- Use nanopb for protobuf-generated device code and keep generated artifacts
  reproducible from the source schemas.
- Gate optional host/device behavior with Kconfig. Every new option needs a
  useful default, help text, dependency conditions, and build coverage.
- Preserve correct buffer ownership and alignment across net_buf, DMA, and
  transport boundaries.
- Do not change protobuf schemas casually; coordinate schema changes with all
  generated host and device bindings.

## Verification

- Build the active board configuration after C or Kconfig changes.
- Run the protocol tests under [tests/](tests/) for framing, sequencing, and
  error-handling changes.
- For transport changes, test the affected backend and at least one
  transport-independent protocol path.
- Check length validation, buffer exhaustion, reset/reinitialization, and
  concurrent RX/TX behavior.
- Record the board, toolchain, and tests used when hardware validation is
  unavailable.
