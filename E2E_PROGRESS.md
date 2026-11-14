# e80211-refactor — E2E validation progress & handoff

Status as of 2026-11-08 (Mac session). The refactor (Phase 0–4) is
structurally complete. Remaining work is **Phase 4/6 E2E validation**
against the physical nRF7002 DK.

**Transport reverted to USB CDC-ACM (2026-11-08).** The J-Link-UART
steering (bus on SoC UART1, UART0 disabled, RTS pin hogs,
`SERIAL_MUX_IFACE_USB=n`) is reverted: the common link protocol bus is
back on the target USB connector's CDC-ACM port (CDC_ACM_0), the
pre-steering design. The vserial console
(`zephyr,console = &vserial`) is kept by design. Temporary diagnostics
removed (uart.c UARTE register read-back, host bus.c BUSTX/BUSRX/BUSYNC
printks, serial.c E80211 DIAG printk). Both required NCS local patches
verified present in the Mac tree (CDC ACM TX-drain in usbd_cdc_acm.c +
net_if.c start-zep step).

WiFi E2E creds are build-time only (`E2E_SSID`/`E2E_PSK`); never in the repo.
## Emulation migrated QEMU → Renode (2026-11-13)

The host app now runs under **Renode** (v1.17.0) on the app-local board
`renode_cortex_m3` (board + SoC under `host/boards/` + `host/soc/`; NCS
v3.4.1 ships no Renode board). `host/run_qemu.sh` is deleted;
`host/run_renode.sh` + `host/tests/tunnel_bus.py` replace it (socat-free:
the bus UART is a Renode socket terminal on 127.0.0.1:25999 bridged to the
device port in Python). See ADR 0009.

The migration exposed a real liveness bug in `embedded80211/zephyr/host/bus.c`:
the RX ISR parsed only the 4-byte header and left the payload in the PL011
FIFO. With a whole frame in the FIFO at once (the emulator delivers frames
instantly; serial hardware does not), the level-triggered RX interrupt stayed
asserted, the CPU re-entered the ISR forever, and the system workqueue that
was supposed to drain the FIFO was starved — a hard system wedge. Fix: the
ISR now drains payload + trailer out of the FIFO; the work item only
validates and dispatches. Verified with a fake device over the Renode bus
bridge (`Bus synced: version 1, types 0x000f, next seq 0`).


## Environment change: Linux dev host → macOS (arm64)

The dev host changed. Current environment on the Mac:

- NCS: `/opt/nordic/ncs/v3.4.1` (west topdir; `zephyr-env.sh` sources it).
- Toolchain: `/opt/nordic/ncs/toolchains/97226bad62` — exported via
  `~/.zephyrrc` (PATH, `ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu`,
  `ZEPHYR_SDK_INSTALL_DIR`, `NRFUTIL_HOME`). `zephyr-env.sh` loads it
  automatically.
- J-Link: `/Applications/SEGGER/JLink_V876` (JLinkExe; the Mac
  `JLinkRTTClient` is GUI-only and `JLinkExe` V8.76 has no `rtt` command).
- Flashing: nrfutil (toolchain `nrfutil`, needs `NRFUTIL_HOME`):
  `nrfutil device program --core network --firmware build/nrf7002dk/ipc_radio/zephyr/zephyr.hex`
  then `nrfutil device program --firmware build/nrf7002dk/nrf7002dk/zephyr/zephyr.hex`
  (sysbuild produces per-domain images; flash radio core first).
- USB ports (change on replug): J-Link OB exposes
  `/dev/cu.usbmodem0010507935211` + `0010507935213` (probe S/N 001050793521).
  The **target** USB (Zephyr CDC-ACM, VID 0x2FE3) carries the protocol
  bus (CDC_ACM_0) plus a spare unbound port (CDC_ACM_1). **2026-11-08:
  not attached to the Mac** (ioreg shows only the J-Link) — and with the
  board powered only from the J-Link OB the 16 MHz HSE does not start,
  so the device also cannot enumerate until the target USB provides
  VBUS. Auto-detect in `host/run_qemu.sh`/`host/tests/run_e2e.sh`
  probes each `/dev/cu.usbmodem*` with a PROTOCOL_INFO query and uses
  the one that answers (`host/tests/find_bus_port.py`).

## Done

- [x] **Host-side wild-jump crash root-caused and fixed** (QEMU `&eth`
  disable; see git log `8aa4eec`/`2b91e6a`).
- [x] **Device-side Kconfig gated to `SERIAL_DEVICE_ROLE`.**
- [x] **Mac environment confirmed & adjusted** (this session):
  - `~/.zephyrrc` created; device + host apps build on the Mac.
  - NCS local patch `nrf_wifi_start_zep_last_step` **re-applied** to the
    fresh Mac NCS tree
    (`/opt/nordic/ncs/v3.4.1/zephyr/drivers/wifi/nrf_wifi/src/net_if.c`):
    global `int nrf_wifi_start_zep_last_step` + `E80211_START_ZEP_STEP(n)`
    markers 1..10 through `nrf_wifi_if_start_zep()` (was only in the old
    Linux NCS tree; missing here broke the device link).
  - **Device reflashed and verified alive** via J-Link: CPU running,
    boot log read from the RTT buffer.
- [x] **E2E extended: gateway ping** (user requirement: acquire IP **and**
  ping the default gateway):
  - `host/src/e2e_test.c`: open-network support (empty PSK →
    `WIFI_SECURITY_TYPE_NONE`) and ICMP echo to the DHCP gateway via
    `net_icmp_init_ctx()`/`net_icmp_send_echo_request()`
    (`zephyr/net/icmp.h`; `icmp.c` builds unconditionally). New markers:
    `E2E WIFI: GATEWAY PING RECV/OK/TIMEOUT`. `E2E WIFI: OK` now only
    prints after a gateway echo reply.
  - `host/tests/run_e2e.sh`: env auto-detect (Linux `env.sh` or Mac
    `zephyr-env.sh`), port auto-detect, `GATEWAY PING OK` check added.
  - `host/run_qemu.sh`: port auto-detect (excludes J-Link ports).
- [x] Current E2E network: **`MedallionNet`, no password** (open).
  Host app is built with `E2E_SSID=MedallionNet E2E_PSK=""`.

## Device console access on the Mac (no RTT client CLI)

The device console is the **vserial virtual UART** (chosen
`zephyr,console = &vserial`, by design) — console writes are dropped by
its poll stub, so the console is effectively silent. The second
CDC-ACM port (CDC_ACM_1) is exposed but unbound. Logs flow over SEGGER
RTT (`CONFIG_RTT_CONSOLE=y`/`CONFIG_USE_SEGGER_RTT=y`), readable over
SWD without the target USB:

- RTT control block `_SEGGER_RTT` = `0x20020040`; up-buffer
  `_acUpBuffer` = `0x20000040` (128 KB, `.rtt_buff_data` section at
  `0x20000000..0x200200e8`).
- Dump with JLinkExe:
  `JLinkExe -CommandFile` with `si SWD / speed 4000 / device nRF5340 /
  connect / halt / mem8 0x20000040 <len> / go`.
- `WrOff` (CB+0x64, 32-bit) is the read offset; the J-Link side must not
  advance it (read-only dumps are safe).

## Transport: USB CDC-ACM (reverted 2026-11-08)

The J-Link-UART transport steer (introduced while the target USB was not
connected) is **reverted**: the protocol bus is back on the target USB
CDC-ACM port (CDC_ACM_0), as in the pre-steering design. Both USB cables
are in use: J-Link USB (SWD flash/debug + RTT) and target USB
(protocol bus). The CDC ACM TX-drain patch is verified applied in the
Mac NCS tree.

**Verified on hardware (2026-11-08):**
- Both cores flashed via nrfutil 8.1.1 (logs: `$NRFUTIL_HOME/logs/`),
  board reset via JLinkExe. Boot log (RTT) shows the USB-ACM transport
  up: `usbd_init: bNumInterfaces 2`, `serial_mux: Registered serial
  interface 1 (CDC_ACM_0)`, `serial_usb: USB CDC ACM initialized
  successfully`, `Wi-Fi ready — proxy mode`.
- **The Mac does NOT show the target USB device** (no VID 0x2FE3 in the
  ioreg USB tree; only the J-Link). Only the J-Link USB is attached to
  the Mac.
- **The app core is running at ~1/43 speed** (a 14 s boot takes ~10 min
  wall time; DWT CYCCNT advances at ~1.5 MHz vs the 64 MHz the firmware
  assumes). The 16 MHz HSE is not oscillating (HSE status reg = 0, all
  FLLs unlocked — read back as 0 via SWD; app-core clock regs are not
  SWD-readable on this part). Consistent with the board being powered
  only from the J-Link OB (no target-USB VBUS). This also explains the
  J-Link-UART steering session's failures: at ~1.5 MHz PCLK the UARTE
  "115200" setting actually produces ~2.7 kbaud, so the J-Link virtual
  port could never exchange frames, and USB can't enumerate without the
  HSE-locked clock.

**Action:** plug the DK **target** USB connector into a working Mac USB
port (powers the board from VBUS + carries the CDC-ACM bus), verify the
device enumerates (VID 0x2FE3, two CDC-ACM ports), then re-run the E2E:
`E2E_SSID=MedallionNet E2E_PSK= host/tests/run_e2e.sh`

## Pending items

1. **Connect the DK target USB to a working Mac USB port** (physical) —
   this also fixes the ~1/43-speed clock issue (HSE needs proper VBUS
   power; see the finding above).
2. **Verify enumeration**: VID 0x2FE3 with two CDC-ACM ports
   (`ioreg -p IOUSB | grep -i 2fe3`); the bus port is found automatically
   by the harness (PROTOCOL_INFO probe).
3. **Re-run the full E2E** against `MedallionNet` (open):
   `E2E_SSID=MedallionNet E2E_PSK= host/tests/run_e2e.sh` — expect
   `E2E WIFI: DHCP BOUND <ip> gw <gw>`, `E2E WIFI: GATEWAY PING OK`,
   `E2E WIFI: OK`, `E2E BT: SCAN DONE`, `E2E PASS`.

## Key commands (Mac, all after `source /opt/nordic/ncs/v3.4.1/zephyr/zephyr-env.sh`)

Host build (creds are build args only; empty PSK = open network):

```
west build -d build/host -s host -b renode_cortex_m3 --no-sysbuild \
  -- -DCONFIG_HOST_TEST_ENABLED=y \
  -DCONFIG_HOST_TEST_WIFI_SSID="\"MedallionNet\"" \
  -DCONFIG_HOST_TEST_WIFI_PSK="\"\""
```

Device build: `west build -d build/nrf7002dk -s nrf7002dk -b nrf7002dk/nrf5340/cpuapp/nrf7001`

Device flash (J-Link, nrfutil):

```
TC=/opt/nordic/ncs/toolchains/97226bad62
export NRFUTIL_HOME=$TC/nrfutil/home PATH="$TC/nrfutil/bin:$PATH"
nrfutil device program --core network --firmware build/nrf7002dk/ipc_radio/zephyr/zephyr.hex
nrfutil device program --firmware build/nrf7002dk/nrf7002dk/zephyr/zephyr.hex
```

nrfutil 8.x prints **nothing on stdout** (logs go to
`$NRFUTIL_HOME/logs/nrfutil-device.log`); check "No errors executing
operation program" there. It does not reset the board afterwards —
reset with JLinkExe (`r` + `g`) if the app needs to start.

Full E2E harness: `E2E_SSID=MedallionNet E2E_PSK= host/tests/run_e2e.sh`
(board port auto-detected; pass it as `$1` to override).

### Constraints

- Wire protocol is invariant (4-byte header, types/seq/ACK, PROTOCOL_INFO
  layout) — `embedded80211/zephyr/include/embedded80211/serial.h`. Do not
  touch it.
- No PII/credentials in the repo. E2E WiFi creds only via build args/env.
- NCS tree local patches MUST NOT be reverted:
  `zephyr/subsys/usb/device_next/class/usbd_cdc_acm.c` (TX-drain patch) and
  `zephyr/drivers/wifi/nrf_wifi/src/net_if.c` (start-zep step diagnostic).
  Both are verified applied in the Mac NCS v3.4.1 tree (2026-11-08);
  if a fresh NCS install appears, re-apply both.
- `source /opt/nordic/ncs/v3.4.1/zephyr/zephyr-env.sh` before any `west`
  command (loads `~/.zephyrrc`). The NCS host app MUST build with
  `--no-sysbuild`. Kconfig warnings are hard errors.
- The `renode_cortex_m3` board has 32 MiB RAM (the old `qemu_cortex_m3`
  64 KB limit no longer applies); net_buf/heap sizes are no longer
  constrained by RAM size.

## Files changed (2026-11-08 revert to USB CDC-ACM)

1. `nrf7002dk/app.overlay` — USB CDC-ACM transport restored (usbd +
   CDC_ACM_0/CDC_ACM_1, uart0); J-Link UART1/pinctrl/RTS-hog removed;
   `zephyr,console = &vserial` kept by design.
2. `nrf7002dk/prj.conf` — USB device stack + CDC ACM config restored;
   the `SERIAL_MUX_IFACE_USB=n` / `UART_CONSOLE=n` steer removed.
3. `embedded80211/zephyr/device/serial/uart.c` — restored to committed
   state (UART0 backend; UARTE register diag removed).
4. `embedded80211/zephyr/device/serial/serial.c` — temporary E80211 DIAG
   printk removed (diagnosis complete).
5. `embedded80211/zephyr/host/bus.c` — BUSTX/BUSRX/BUSYNC trace printks
   removed; the bus_open retry-on-failed-handshake fix kept.
6. `host/run_qemu.sh`, `host/tests/run_e2e.sh`,
   `host/tests/find_bus_port.py` — bus port = target USB CDC-ACM
   (functional auto-detect kept; wording updated).
7. `host/src/e2e_test.c` — open-network + gateway ping kept from
   2026-11-07; stray indent fixed.
8. `E2E_PROGRESS.md` — this file.

## Files changed (2026-11-07 session)

1. `host/src/e2e_test.c` — open-network security selection; gateway ICMP
   ping (new success criterion).
2. `host/tests/run_e2e.sh` — env + port auto-detect; gateway-ping check.
3. `host/run_qemu.sh` — port auto-detect.
4. `~/.zephyrrc` (outside repo) — Mac NCS toolchain environment.
5. `/opt/nordic/ncs/v3.4.1/zephyr/drivers/wifi/nrf_wifi/src/net_if.c`
   (outside repo) — `nrf_wifi_start_zep_last_step` diagnostic re-applied.
6. `E2E_PROGRESS.md` — this file.
