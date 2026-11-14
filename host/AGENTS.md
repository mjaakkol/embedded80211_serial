# Renode host app

## Scope

These rules apply to the Renode host application in this directory and
supplement [../AGENTS.md](../AGENTS.md). The host app is a Zephyr application
for the app-local board `renode_cortex_m3` (an app-local board + SoC pair
under `host/boards/` and `host/soc/`, since NCS v3.4.1 ships no Renode board)
that instantiates the host-side drivers from the `embedded80211` module
(virtual Wi-Fi, virtual Bluetooth HCI, and the link protocol bus client) over
a UART, and optionally runs the end-to-end test.

## Layout

- `CMakeLists.txt` — adds `embedded80211` to `ZEPHYR_EXTRA_MODULES` before
  `find_package(Zephyr)`.
- `Kconfig` — app Kconfig root; sources `Kconfig.zephyr`.
- `prj.conf` — default configuration (host role, networking, Bluetooth).
- `app.overlay` — device tree: the `embedded80211,virt-wifi` and
  `embedded80211,bt-hci-virtual` nodes, the `embedded80211,bus` chosen node
  bound to `uart1`, and the `zephyr,bt-hci` chosen node.
- `src/main.c` — application entry point.
- `src/e2e_test.{c,h}` — end-to-end test, enabled only with
  `CONFIG_HOST_TEST_ENABLED=y`.
- `boards/renode_cortex_m3/` — app-local board: `board.yml`, `board.cmake`,
  `Kconfig.renode_cortex_m3`, `renode_cortex_m3.dts`, `renode_cortex_m3_defconfig`,
  and `support/renode_cortex_m3.{repl,resc}` (the Renode machine + runner).
- `soc/renode_cortex_m3/` — app-local SoC (Cortex-M3, 100 MHz, 256 KiB ROM,
  32 MiB RAM) wired into the app via `BOARD_ROOT`/`SOC_ROOT` in `CMakeLists.txt`.
- `run_renode.sh` — resolves Renode, launches the machine with the bus UART
  on a socket terminal, and bridges it to the device port with
  `tests/tunnel_bus.py`.
- `tests/run_e2e.sh` — runs the end-to-end test and checks the result.

## Build

The app is a Zephyr module consumer, so it must be built with `--no-sysbuild`
(NCS `west build` enables sysbuild by default):

```
source /path/to/ncs/env.sh
west build -d build/host -s host -b renode_cortex_m3 --no-sysbuild
```

## Rules

- Keep host-specific configuration in this app; the shared driver behavior
  belongs in the `embedded80211` module.
- The end-to-end test is disabled by default; enable it with
  `-DCONFIG_HOST_TEST_ENABLED=y`.
- Wi-Fi credentials for the end-to-end test must be supplied via build args
  (`-DCONFIG_HOST_TEST_WIFI_SSID=...` / `..._PSK=...`) or the environment
  (`E2E_SSID` / `E2E_PSK`); never commit real SSIDs or passphrases.

## Verification

- Build the app after any change (compile + link must succeed).
- When Renode is available, run `run_renode.sh` and, with the test enabled,
  `tests/run_e2e.sh` to exercise the full host/device loop.
