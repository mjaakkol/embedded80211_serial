# e80211-refactor — E2E validation progress & handoff

Status as of 2026-11-07. The refactor (Phase 0–4) is structurally complete:
the device stack is split into the `embedded80211/` Zephyr library, the
`nrf7002dk/` board project, and the `host/` QEMU app. The remaining work is
**Phase 4/6 E2E validation** against the physical nRF7002 DK.

WiFi E2E creds are build-time only (`E2E_SSID`/`E2E_PSK`); never in the repo.

## Done

- [x] **Host-side wild-jump crash root-caused and fixed.**
  - Symptom: QEMU host app produced NO console output (crash before the log
    backend flushed — not a logging bug).
  - Cause: the `qemu_cortex_m3` (`lm3s6965evb`) board dts enables a built-in
    Stellaris ENET (`&eth { status = "okay"; }`), which registers an Ethernet
    interface at **index 1**, shifting `virt_wifi` to index 2. `e2e_test.c` /
    `host_wifi.c` use `VIRT_WIFI_IFACE_INDEX 1`, so `net_if_get_by_index(1)`
    returned the ENET; `get_wifi_api()` cast its `dev->api` (an eth API) to
    `net_wifi_mgmt_offload *` → wild `scan` pointer → jump to garbage
    (GDB: PC=`0x708xxxxx`, LR=`0x817`, CFSR/HFSR/BFAR all 0 = no HW fault).
  - Fix: `host/app.overlay` now sets `&eth { status = "disabled"; };`.
    virt_wifi is now the only non-loopback interface at index 1.
  - Verified: scan/connect now time out cleanly (`-116`/`-ETIMEDOUT`), no
    wild jump; interface count dropped 3→2.
- [x] **Device-side Kconfig gated to `SERIAL_DEVICE_ROLE`.**
  - `embedded80211/zephyr/Kconfig`: the "Serial multiplexer" menu
    (`SERIAL_PROXY_*`, `SERIAL_MUX_*`) and `rsource "device/serial/Kconfig"`
    were unconditional, so device proxies (which create their own net_if)
    leaked into the host build. Wrapped in `if SERIAL_DEVICE_ROLE ... endif`.
    Host `.config` now has only `SERIAL_HOST_ROLE=y`; device `.config` still
    has `SERIAL_DEVICE_ROLE=y` + proxies.
- [x] `e2e_test.c` temporary diagnostic added then removed (file is clean).
- [x] `host/README.md` documents the `&eth` disable requirement (Stellaris
  ENET shifts virt_wifi off index 1).

## Blocker: device reports 0 supported types

The device answers PROTOCOL_INFO with `supported types 0x0000`, so WiFi
MGMT / BT traffic times out (`-116`). Ruled out as the cause:

- Device config is correct (`SERIAL_PROXY_WIFI_MGMT/BT/NET_IF_MGMT/OT/
  VSERIAL = y`, all routed to the USB interface).
- `serial_mux_supported_types_mask()` (`device/multiplexer.c:174`) **always
  sets bit 7** (`SERIAL_TYPE_PROTOCOL_INFO`) → it cannot return 0.
- All proxy callback symbols are present in the device ELF; `mux_callbacks[]`
  is populated (verified via GDB read of the live symbols).
- Host `bus_sync()` / `bus_rx_work()` parse the 4-byte response correctly
  (`p[1] | (p[2]<<8)`).
- `handle_protocol_info.isra.0` (device ELF) **does** call
  `serial_mux_supported_types_mask` (`bl 0x5fd3e` at `0x3e1de`), so the code
  path is confirmed correct.

So the live device is computing 0 even though the code is sound. A temporary
`printk` diagnostic was added to `handle_protocol_info`
(`embedded80211/zephyr/device/serial/serial.c`) to capture the live value on
the device console. **This is temporary — remove it before finalizing.**

## Device hardware state (needs attention on resume)

After repeated J-Link flash attempts the nRF7002 DK entered a bad state:
console empty, SWD unresponsive (`InitTarget()` hangs). It needs a **physical
RESET** (board reset button, or unplug/replug USB) before the next flash.
The dev host has no passwordless sudo, so a USB power toggle is not possible
from the shell.

## Pending items

1. **Physically reset the nRF7002 DK.**
2. **Reflash** the current device build (J-Link recipe below) and read the
   device console for the
   `E80211 DIAG: handle_protocol_info supported_types=0x....` line to see the
   live value the device computes.
3. **Fix the device 0-supported-types** once the live value is known.
4. **Re-run the full E2E**: confirm `E2E WIFI: OK` (scan → connect → DHCP)
   and `E2E BT: SCAN DONE`.
5. **Remove the temporary `printk`** from `serial.c`.

## Key commands (all after `source /home/mikko/ncs/env.sh`)

Host build (creds are build args only):

```
west build -d build/host -s host -b qemu_cortex_m3 --no-sysbuild \
  -- -DCONFIG_HOST_TEST_ENABLED=y \
  -DCONFIG_HOST_TEST_WIFI_SSID="\"HappyHouse\"" \
  -DCONFIG_HOST_TEST_WIFI_PSK="\"dontenter\""
```

Device build: `west build -d build/nrf7002dk`

QEMU one-shot WITH the physical device:

```
rm -f /tmp/e80211-host-bus /tmp/e80211-host-console.log && : > /tmp/e80211-host-console.log && \
(socat pty,raw,echo=0,link=/tmp/e80211-host-bus \
   /dev/serial/by-id/usb-Test_Vendor_My_Zephyr_CDC-ACM_Device_2994E6BE83CEFC3F-if00 & \
 spid=$!; \
 qemu-system-arm -machine lm3s6965evb -cpu cortex-m3 -nographic \
   -kernel build/host/zephyr/zephyr.elf \
   -serial "file:/tmp/e80211-host-console.log" -serial "file:/tmp/e80211-host-bus" \
   -monitor none & qpid=$!; sleep 35; kill $qpid $spid 2>/dev/null; wait 2>/dev/null); \
cat /tmp/e80211-host-console.log
```

Full E2E harness: `E2E_SSID=HappyHouse E2E_PSK=dontenter host/tests/run_e2e.sh`

### J-Link flash recipe (nRF7002 DK, on-board J-Link OB)

`/home/mikko/local/jlink/JLink_Linux_V982_x86_64/JLinkExe` with a command file
containing:

```
si SWD
speed 1000
device nRF5340
coresel 0
connect
halt
load build/nrf7002dk/zephyr/zephyr.hex
reset
quit
```

Notes: `connect` MUST precede `halt`; the device name is `nRF5340` (NOT
`nRF5340_app`). The GDB server (`JLinkGDBServer -selectIF SWD -Device nRF5340
-port 5678`) holds the J-Link and must be killed before a flash.

### Constraints

- Wire protocol is invariant (4-byte header, types/seq/ACK, PROTOCOL_INFO
  layout) — `embedded80211/zephyr/include/embedded80211/serial.h`. Do not
  touch it.
- No PII/credentials in the repo. E2E WiFi creds only via build args/env.
- NCS tree local patches MUST NOT be reverted:
  `zephyr/subsys/usb/device_next/class/usbd_cdc_acm.c` (TX-drain patch) and
  `zephyr/drivers/wifi/nrf_wifi/src/net_if.c` (diagnostic counter).
- `source /home/mikko/ncs/env.sh` before any `west` command. The NCS host app
  MUST build with `--no-sysbuild`. Kconfig warnings are hard errors.
- `qemu_cortex_m3` has 64 KB RAM → keep net_buf/heap sizes small.

## Files changed this session

1. `embedded80211/zephyr/Kconfig` — wrapped the "Serial multiplexer" menu and
   `rsource "device/serial/Kconfig"` in `if SERIAL_DEVICE_ROLE ... endif`.
2. `host/app.overlay` — added `&eth { status = "disabled"; };` (Stellaris ENET
   disable).
3. `embedded80211/zephyr/device/serial/serial.c` — **temporary** `printk`
   diagnostic in `handle_protocol_info` (remove before final).
4. `E2E_PROGRESS.md` — this file.
