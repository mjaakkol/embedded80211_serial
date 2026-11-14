# embedded80211

Zephyr module library for the Radio Co-Processor. It provides the common link
protocol, the device-side (co-processor) transport and technology proxies, and
the Zephyr host-side drivers, all sharing one wire protocol.

## Layout

| Path | Contents |
|------|----------|
| `zephyr/` | Module build files and sources (`module.yml`, `CMakeLists.txt`, `Kconfig`) |
| `zephyr/include/embedded80211/` | Public headers (`serial.h`, `bus.h`, `multiplexer.h`, `proxy.h`, `host_wifi.h`) |
| `zephyr/device/` | Device-side transport + technology proxies (`CONFIG_SERIAL_DEVICE_ROLE`) |
| `zephyr/host/` | Host-side drivers: `bus.c`, `host_wifi.c`, `host_bt.c` (`CONFIG_EMBEDDED80211_HOST_*`) |
| `zephyr/dts/bindings/` | Device-tree bindings for the host driver nodes |
| `tests/` | ztest protocol suite |

Protobuf schemas live in `../protos/` and are generated to nanopb at build
time (generated artifacts are not committed).

## Usage

Add this directory to `ZEPHYR_EXTRA_MODULES` from a consuming app's
`CMakeLists.txt` before `find_package(Zephyr)`:

```cmake
set(ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../embedded80211)
```

The device app (`nrf7002dk/`) builds the device role; the host app (`host/`)
builds the host drivers.

## Verification

- Board app: `west build -d build -s nrf7002dk -b nrf7002dk/nrf5340/cpuapp`
- Host app: `west build -d build/host -s host -b renode_cortex_m3 --no-sysbuild`
- Protocol tests: `embedded80211/tests/run_tests.sh`
