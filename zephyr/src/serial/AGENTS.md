# Zephyr serial-backend instructions

## Scope

These rules apply to serial transport implementations in this directory and
supplement [../../AGENTS.md](../../AGENTS.md) and [../AGENTS.md](../AGENTS.md).

## Backend boundaries

- Keep UART, USB, SPI, and SDIO-specific code behind the shared serial
  interface. Do not duplicate framing or sequence logic in a backend.
- Make RX/TX ownership, interrupt context, buffering, and flow control
  explicit in headers and implementation.
- Do not block or perform lengthy protocol work in interrupt callbacks.
- Validate lengths before copying into buffers and preserve required alignment.
- Handle disconnect, reset, cancellation, and partial transfer completion
  without leaving stale state in the multiplexer.
- Keep Kconfig symbols, device-tree requirements, and buffer defaults aligned
  with the backend actually being built.

## Verification

- Build each changed backend with its intended Kconfig/device-tree settings.
- Test the shared protocol with a fake or loopback transport before relying on
  hardware.
- Exercise initialization, shutdown, short transfers, timeout, overflow,
  disconnect, and reinitialization paths.
