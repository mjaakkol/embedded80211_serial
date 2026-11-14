# embedded80211 Zephyr module

## Scope

These rules apply to the `embedded80211` Zephyr module and supplement
[../AGENTS.md](../AGENTS.md). The module is the shared Radio Co-Processor
library: the common link protocol, the device-side transport and technology
proxies, and the Zephyr host-side drivers. It is consumed as a Zephyr module
(via `ZEPHYR_EXTRA_MODULES`) by the board app (`nrf7002dk/`) and by the
Renode host app (`host/`).

## Layout

- `zephyr/` — module build files (`CMakeLists.txt`, `Kconfig`, `module.yml`)
  and sources:
  - `include/embedded80211/` — public headers (`serial.h`, `bus.h`,
    `multiplexer.h`, `proxy.h`, `host_wifi.h`).
  - `device/` — device-side (co-processor) transport and technology proxies,
    compiled only under `CONFIG_SERIAL_DEVICE_ROLE`. See
    [zephyr/device/AGENTS.md](zephyr/device/AGENTS.md).
  - `host/` — Zephyr host-side drivers (`bus.c`, `host_wifi.c`, `host_bt.c`),
    compiled under `CONFIG_EMBEDDED80211_HOST_*`. See [host/AGENTS.md](../host/AGENTS.md).
  - `dts/bindings/` — device-tree bindings for the host driver nodes.
- `tests/` — ztest suite for the link protocol. See
  [tests/AGENTS.md](tests/AGENTS.md).
- `../../protos/` — protobuf schemas shared by device and host (generated via
  nanopb at build time; never committed).

## Module rules

- Keep the module build self-contained: a consumer only needs to add
  `embedded80211` to `ZEPHYR_EXTRA_MODULES`.
- Gate device sources on `CONFIG_SERIAL_DEVICE_ROLE` and host drivers on
  `CONFIG_EMBEDDED80211_HOST_*` so a single build links exactly one role.
- The device and host builds must not duplicate the nanopb runtime; the
  runtime is added only when the consuming build does not already provide the
  `modules__nanopb` target.
- Do not add Zephyr subsystem `select`s to the host Kconfig; subsystem
  selection from a module can create dependency cycles. Leaf config
  `select`s (e.g. `WIFI_USE_NATIVE_NETWORKING`) are acceptable.
- Host driver nodes require a DTS binding and a `#define DT_DRV_COMPAT` in
  the driver source.
- The wire protocol is invariant; see the root AGENTS.md for the framing,
  sequencing, and schema rules.

## Verification

- Build the board app (`nrf7002dk/`) and the host app (`host/`) after module
  C, Kconfig, or CMake changes.
- Run the protocol tests under [tests/](tests/) for framing, sequencing, and
  error-handling changes.
