# host

Renode host application for the embedded 802.11 serial link protocol. It is a
Zephyr app for the app-local `renode_cortex_m3` board that instantiates the
host-side drivers from the `embedded80211` module — virtual Wi-Fi, virtual
Bluetooth HCI, and the link protocol bus client — over a UART, and optionally
runs an end-to-end test against a device image.

## Layout

| Path | Contents |
|------|----------|
| `CMakeLists.txt` | Adds `embedded80211` to `ZEPHYR_EXTRA_MODULES` |
| `Kconfig` | App Kconfig root |
| `prj.conf` | Default configuration (host role, networking, Bluetooth) |
| `app.overlay` | Device tree: `virt-wifi`, `bt-hci-virtual` nodes, `embedded80211,bus` chosen |
| `src/main.c` | Application entry point |
| `src/e2e_test.{c,h}` | End-to-end test (`CONFIG_HOST_TEST_ENABLED=y`) |
| `boards/renode_cortex_m3/` | App-local board (DTS, defconfig, Renode `.repl`/`.resc`) |
| `soc/renode_cortex_m3/` | App-local Cortex-M3 SoC (100 MHz, 256 KiB ROM, 32 MiB RAM) |
| `run_renode.sh` | Launch under Renode; bridges the bus UART to the device port |
| `tests/tunnel_bus.py` | Bridges Renode's bus socket to the device serial port |
| `tests/run_e2e.sh` | Run the end-to-end test and check the result |

## Build

NCS `west build` enables sysbuild by default, so build with `--no-sysbuild`:

```bash
source /path/to/ncs/env.sh
west build -d build/host -s host -b renode_cortex_m3 --no-sysbuild
```

## End-to-end test

The test is disabled by default. Enable it and provide credentials via build
args or environment (never commit real SSIDs/passphrases):

```bash
west build -d build/host -s host -b renode_cortex_m3 --no-sysbuild \
    -- -DCONFIG_HOST_TEST_ENABLED=y
E2E_SSID=test E2E_PSK=testpass ./tests/run_e2e.sh
```

Notes:

- The bus client's sync handshake needs `CONFIG_MAIN_STACK_SIZE=4096`
  (set in `prj.conf`); the 1024 B default overflows and faults at boot.
- A full WiFi E2E requires a flashed device on the board port (default
  `/dev/serial/by-id/usb-Test_Vendor_My_Zephyr_CDC-ACM_Device_*-if00`)
  whose serial mux reports the WiFi/BT traffic types. With no device,
  `bus_open()` times out and the BT scan reports `-EAGAIN`; an empty
  `E2E_SSID` still yields a BT-only pass once the device is up.
- `run_renode.sh` finds Renode via `$RENODE`, `PATH`, or
  `~/tools/renode/Renode.app/Contents/MacOS/renode`. The bus UART (uart1) is
  exposed as a socket terminal on 127.0.0.1:25999, which `tests/tunnel_bus.py`
  bridges to the device serial port; the console (uart0) is streamed to a log
  file under `/tmp`.
