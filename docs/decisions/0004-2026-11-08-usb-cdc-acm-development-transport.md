# ADR 0004: USB CDC-ACM as the development transport

- **Date**: 2026-11-08
- **Status**: accepted

## Context

The DK's target USB connector carries the protocol bus. When the target USB
was not attached to the dev host, the bus was steered onto the J-Link's
UART (UART1, UART0 disabled, RTS pin hogs, `SERIAL_MUX_IFACE_USB=n`). That
steering proved unreliable: powered only from the J-Link OB the 16 MHz HSE
does not start, the app core runs at ~1/43 speed (~1.5 MHz), UART "115200"
is actually ~2.7 kbaud, and USB cannot enumerate without the HSE-locked
clock.

## Decision

The J-Link-UART steering was **reverted**; the common link protocol bus is
on the target USB connector's CDC-ACM interface:

- `CDC_ACM_0` — the multiplexed protocol bus.
- `CDC_ACM_1` — spare interface (unbound by default).
- The device console stays on the **vserial** virtual UART
  (`zephyr,console = &vserial`), by design.
- Both USB cables are in use: J-Link USB (SWD flash/debug + RTT) and target
  USB (protocol bus + VBUS power).
- The bus port is auto-detected by the harnesses
  (`host/tests/find_bus_port.py` probes each port with a `PROTOCOL_INFO`
  query).

The transport layer stays agnostic: SPI and SDIO are the production targets
and must sit behind the same serial interface (see root AGENTS.md and
  [linux/README.md](../../linux/README.md) §3.7).

## Consequences

- The **target USB port must be plugged in**: without VBUS the HSE does not
  start and all communication fails. This is the single most common
  "device is dead" cause.
- The CDC-ACM TX-drain patch in NCS `usbd_cdc_acm.c` is required (see
  [nrf7002dk/README.md](../../nrf7002dk/README.md)); without it the
  device enumerates but never answers the link protocol.
- The strict ACK scheme makes a quiet line a valid idle signal, which
  supports USB autosuspend design in the Linux transport.
