# QEMU host app

## Scope

These rules apply to the QEMU host application in this directory and
supplement [../AGENTS.md](../AGENTS.md). The host app is a Zephyr application
for `qemu_cortex_m3` that instantiates the host-side drivers from the
`embedded80211` module (virtual Wi-Fi, virtual Bluetooth HCI, and the link
protocol bus client) over a UART, and optionally runs the end-to-end test.

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
- `run_qemu.sh` — builds and launches the app under QEMU with a PTY pair.
- `tests/run_e2e.sh` — runs the end-to-end test and checks the result.

## Build

The app is a Zephyr module consumer, so it must be built with `--no-sysbuild`
(NCS `west build` enables sysbuild by default):

```
source /path/to/ncs/env.sh
west build -d build/host -s host -b qemu_cortex_m3 --no-sysbuild
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
- When QEMU is available, run `run_qemu.sh` and, with the test enabled,
  `tests/run_e2e.sh` to exercise the full host/device loop.
