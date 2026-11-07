# host

QEMU host application for the embedded 802.11 serial link protocol. It is a
Zephyr app for `qemu_cortex_m3` that instantiates the host-side drivers from
the `embedded80211` module — virtual Wi-Fi, virtual Bluetooth HCI, and the
link protocol bus client — over a UART, and optionally runs an end-to-end
test against a device image.

## Layout

| Path | Contents |
|------|----------|
| `CMakeLists.txt` | Adds `embedded80211` to `ZEPHYR_EXTRA_MODULES` |
| `Kconfig` | App Kconfig root |
| `prj.conf` | Default configuration (host role, networking, Bluetooth) |
| `app.overlay` | Device tree: `virt-wifi`, `bt-hci-virtual` nodes, `embedded80211,bus` chosen |
| `src/main.c` | Application entry point |
| `src/e2e_test.{c,h}` | End-to-end test (`CONFIG_HOST_TEST_ENABLED=y`) |
| `run_qemu.sh` | Build + launch under QEMU with a PTY pair |
| `tests/run_e2e.sh` | Run the end-to-end test and check the result |

## Build

NCS `west build` enables sysbuild by default, so build with `--no-sysbuild`:

```bash
source /path/to/ncs/env.sh
west build -d build/host -s host -b qemu_cortex_m3 --no-sysbuild
```

## End-to-end test

The test is disabled by default. Enable it and provide credentials via build
args or environment (never commit real SSIDs/passphrases):

```bash
west build -d build/host -s host -b qemu_cortex_m3 --no-sysbuild \
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
- The `qemu_cortex_m3` (`lm3s6965evb`) board dts enables a built-in Stellaris
  ENET, which registers an Ethernet interface at index 1 and shifts
  `virt_wifi` to index 2. The E2E test and `host_wifi` address the interface
  by index 1 (`VIRT_WIFI_IFACE_INDEX`), so `app.overlay` disables it
  (`&eth { status = "disabled"; };`). Keep it disabled — re-enabling it makes
  `get_wifi_api()` cast the ENET's API to a WiFi API and wild-jump on scan.
