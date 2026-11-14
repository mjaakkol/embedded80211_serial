# Embedded 802.11 serial

A Radio Co-Processor solution for [Zephyr](https://www.zephyrproject.org/)
based microcontrollers. A Zephyr device app exposes its Wi-Fi, Bluetooth
(BLE), and (reserved) OpenThread stacks to a host over **one multiplexed
serial link**, reducing the need for separate peripherals and PCB traces.
Hosts: Zephyr (reference implementation) and Linux (cfg80211, in planning).

This is a hobby project with no commercial aims — the goal is to see if this
can be done efficiently using the most straightforward implementations.

For the system design, the wire protocol specification, module boundaries,
and data flows, see [docs/architecture.md](docs/architecture.md). Historical
design decisions are recorded as ADRs in
[docs/decisions/](docs/decisions/).

## Repository layout

| Path | Contents |
|------|----------|
| `embedded80211/` | Zephyr module: common link protocol, device-side transport + technology proxies, Zephyr host drivers, protocol test suite |
| `nrf7002dk/` | Board project: device-side app for the nRF7002 DK (nRF5340 + nRF70) |
| `host/` | Renode host application (`renode_cortex_m3`) + end-to-end test |
| `linux/` | Linux cfg80211 driver plan (see [linux/README.md](linux/README.md)) |
| `cli_test/` | Rust CLI test harness over a serial port |
| `protos/` | Protobuf schemas (generated to nanopb at build time) |
| `docs/` | Architecture blueprint, diagrams, ADRs |

## Prerequisites

- **NCS v3.4.1** (west workspace) with `zephyr-env.sh` / `env.sh` sourced
  before any `west` command.
- **Hardware (optional, for E2E)**: nRF7002 DK. The DK's **target USB
  connector must be plugged in** — it provides VBUS (without it the 16 MHz
  HSE does not start and all communication fails) and carries the protocol
  bus on CDC-ACM interface `CDC_ACM_0`.
- **Renode** (for the host app): `run_renode.sh` finds it via `$RENODE`,
  `PATH`, or `~/tools/renode/Renode.app/Contents/MacOS/renode`.
- **For `cli_test`**: Rust, `protoc`, and on Linux the `libudev` dev files.
- **NCS local patches** (CDC-ACM TX-drain + nrf_wifi markers) must be
  present; see [nrf7002dk/README.md](nrf7002dk/README.md). Re-apply after a
  fresh NCS install.

## Quickstart

### Build the device app

```bash
source /path/to/ncs/zephyr-env.sh
west build -d build/nrf7002dk -s nrf7002dk -b nrf7002dk/nrf5340/cpuapp/nrf7001
```

### Build the Renode host app

The NCS host app must build with `--no-sysbuild`:

```bash
west build -d build/host -s host -b renode_cortex_m3 --no-sysbuild
```

### Run the protocol tests

```bash
embedded80211/tests/run_tests.sh
```

### Flash the device (J-Link + nrfutil)

Sysbuild produces per-domain images; flash the radio core first:

```bash
nrfutil device program --core network --firmware build/nrf7002dk/ipc_radio/zephyr/zephyr.hex
nrfutil device program --firmware build/nrf7002dk/nrf7002dk/zephyr/zephyr.hex
```

nrfutil does not reset the board afterwards — reset with JLinkExe (`r` + `g`)
if the app needs to start.

### Run the end-to-end test

Wi-Fi credentials are build-time arguments / environment variables only —
never commit real SSIDs or passphrases:

```bash
west build -d build/host -s host -b renode_cortex_m3 --no-sysbuild \
  -- -DCONFIG_HOST_TEST_ENABLED=y
E2E_SSID=MySSID E2E_PSK=mypassword host/tests/run_e2e.sh
```

The board's bus port is auto-detected (pass it as `$1` to override). Expect
`E2E WIFI: OK`, `E2E BT: SCAN DONE`, `E2E PASS`.

### Use the Rust CLI

```bash
cd cli_test
cargo build
cargo run -- --port /dev/ttyACM0 version
cargo run -- --port /dev/ttyACM0 scan
```

Full command reference: [cli_test/README.md](cli_test/README.md).

## Further reading

- [docs/architecture.md](docs/architecture.md) — system blueprint: tech
  stack, module boundaries, link protocol spec, data flows, constraints
- [docs/decisions/](docs/decisions/) — architecture decision records
- [linux/README.md](linux/README.md) — Linux cfg80211 driver plan
  (requirements, mapping tables, capability matrix, roadmap)
- Per-directory `README.md` and `AGENTS.md` files for module-specific
  details
