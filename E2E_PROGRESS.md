# E2E Progress — 2026-11-15

## Current Status
**Blocked: USB bulk-OUT endpoint does not receive payload frames (>4 bytes).**

Length-0 PROTOCOL_INFO frames work repeatedly. The FIRST frame with a payload
kills USB RX permanently (subsequent PI queries also fail). JLink `j.reset()`
clears the dead state.

## Root Cause Analysis
`parsing_state=0` in `config_serial` after a payload frame proves the
`uart_irq_callback` never ran — the USB bulk-OUT endpoint never received the
bytes. This is NOT a `serial.c` issue; it's in the CDC-ACM USB driver RX
re-arm path or the nRF5340 USBD hardware.

### Fixes Applied (NCS local patches in `usbd_cdc_acm.c`)
1. **TX single-transfer fix** (`serial.c`): `serial_send_frame()` now builds
   the whole frame in one `k_malloc` buffer and sends with ONE
   `serial_data_tx()` call. Verified: PI round-trip works.
2. **RX busy-bit clear** (`usbd_cdc_acm.c` `cdc_acm_rx_fifo_handler`):
   `CDC_ACM_RX_FIFO_BUSY` is now cleared on `cdc_acm_buf_alloc()` failure
   and `usbd_ep_enqueue()` failure, matching the existing TX handler pattern.
   Also resubmits `rx_fifo_work` on alloc failure to avoid deadlock.
3. These fixes are correct but do NOT resolve the payload-frame RX death.

### Ruled Out
- System workqueue starvation (work list empty after payload frame)
- RX ring buffer full (8192 bytes, 10-byte frame can't fill it)
- `acquire_buffer` failure (drain loop added, RX still dies)
- `CDC_ACM_RX_FIFO_BUSY` stuck bit (fix applied, still fails)
- USB buffer pool exhaustion (pool free list is NULL after death, but this
  is a symptom, not the cause — buffers aren't freed because completions
  don't fire)
- Transition from len=0 to len>0 (len>0 as FIRST frame after reset also fails)
- Split header/payload with delay (still fails)

### Critical Finding
**Hard boundary at `len=0`:** ANY frame with `len>0` kills USB RX
permanently, even as the very first frame after a JLink reset.
4-byte frames (len=0) work repeatedly. 5-byte frames (len=1) fail.
The nRF5340 USB bulk-OUT endpoint only accepts exactly 4-byte packets.

### Remaining Blocker
The nRF5340 USB bulk-OUT endpoint does not accept packets >4 bytes.
This is a USB hardware/driver issue, not a `serial.c` or CDC-ACM
re-arm issue. Requires `dmesg`/`usbmon` access (sudo) to inspect
host-side USB transactions, or a different USB port/cable/hub.
## E2E Harness Command
```bash
env -u PYTHONHOME -u PYTHONPATH -u LD_LIBRARY_PATH bash -c '
  source ~/ncs/env.sh &&
  cd ~/projects/embedded80211_serial/host &&
  export RENODE=/home/mikko/local/renode/renode &&
  E2E_SSID=Koti E2E_PSK="fam!lyJaakkola5" ./tests/run_e2e.sh /dev/ttyACM2
'
```

Kill stale processes before running:
```bash
pkill -f run_renode.sh; pkill -f "renode.*-p"; pkill -f tunnel_bus
```

## Build Commands
- **Device**: `west build -d nrf7002dk/build/nrf7002dk -s nrf7002dk -b nrf7002dk/nrf5340/cpuapp/nrf7001 --force`
- **Host**: `west build -d build/host -s host -b renode_cortex_m3 --no-sysbuild --force -- -DCONFIG_HOST_TEST_ENABLED=y -DCONFIG_HOST_TEST_WIFI_SSID='"Koti"' -DCONFIG_HOST_TEST_WIFI_PSK='"fam!lyJaakkola5"'`
- Both require `source ~/ncs/env.sh` first
- `--force` needed after NCS source changes; `touch` the file first

## Flash Commands
```bash
# JLinkExe (nrfutil can't find JLink DLL)
HEX=$PWD/nrf7002dk/build/nrf7002dk/nrf7002dk/zephyr/zephyr.hex
cat > /tmp/flash_app.jlink <<EOF
si SWD
speed 4000
connect
loadfile $HEX
r
g
exit
EOF
JLinkExe -device nRF5340 -SelectEmuBySN 1050793521 -CommandFile /tmp/flash_app.jlink
```
JLinkExe is in PATH via `source ~/ncs/env.sh`. Connect ~20s, flash ~18s.

## Test Commands
- **Protocol**: `cd embedded80211/tests && bash run_tests.sh` (17 tests, 100%)
- **CLI**: `cd cli_test && cargo test` (5 tests)

## Verified Passing (this session)
- [x] Device build
- [x] Host build
- [x] Protocol tests (17/17)
- [x] Rust CLI tests (5/5)
- [x] PROTOCOL_INFO round-trip over USB CDC-ACM (length-0 frames)
- [x] TX single-transfer fix (no more payload drops on send)
- [ ] E2E harness (Wi-Fi DHCP + gateway ping + BT scan) — BLOCKED on USB RX

## USB Issue Details
- `/dev/ttyACM2` = Zephyr TARGET port (VID 2fe3, "My Zephyr CDC-ACM Device")
- `/dev/ttyACM0` + `/dev/ttyACM1` = J-Link ONBOARD port (VID 1366) — DO NOT USE
- Device firmware runs, USB enumerates, CDC-ACM driver binds
- Length-0 PI frames work repeatedly (4+ in a row, 0.7s spacing)
- First payload frame (>4 bytes) kills RX permanently; JLink `j.reset()` clears it
- `parsing_state=0` after payload frame → `uart_irq_callback` never ran
- USB buffer pool free list is NULL after death (symptom, not cause)
- CDC-ACM RX busy-bit clear + resubmit applied (correct, insufficient)
- **Blocker**: needs sudo for `dmesg`/`usbmon` to diagnose host USB state,
  or physical cable/port change (remote session can't do this)

## Repo Git State (uncommitted changes)
- `embedded80211/zephyr/host/host_wifi.c` — `virt_wifi_if_init()` added, `iface_api.init` set, `virt_wifi_init()` simplified, `bus_open()` removed
- `embedded80211/zephyr/host/host_bt.c` — `bus_open()` removed from `virt_bt_init()`
- `host/src/main.c` — printf diagnostic removed
- `embedded80211/zephyr/host/bus.c` — `BUS_SYNC_RETRIES` 4→30
- All prior session changes remain (see git diff)

## NCS Patches (9, all applied, outside repo)
1. `usbd_cdc_acm.c`: `K_NO_WAIT` in `cdc_acm_fifo_fill()` (TX fix)
2. `net_if.c` (nrf_wifi): `nrf_wifi_start_zep_last_step` markers
3. `net_if.c` (nrf_wifi): `__weak nrf_wifi_if_rx_frm_hook()` + call site
4. `dhcpv4.c`: 3 changes (callback registration, IF_UP handler, logs removed)
5. `usbd_cdc_acm.c`: LOG_INF trace removed
6. `net_if.c` (nrf_wifi): LOG_INF trace removed
7. `ipv4.c`: 2 changes (accept 255.255.255.255 as broadcast pre-DHCP)
8. `usbd_cdc_acm.c`: RX `CDC_ACM_RX_FIFO_BUSY` clear on alloc/enqueue failure
9. `usbd_cdc_acm.c`: RX `rx_fifo_work` resubmit on alloc failure

## Key Constants
- Device MAC: `00:00:00:00:00:01`, Host MAC: `00:00:00:00:00:02`
- RTT: `_SEGGER_RTT` at `0x20020040`, UP buffer[0] at `0x20000040` size 0x20000
- J-Link: S/N 1050793521, use `speed 1000` (4000 is flaky)
- `mem32` max 4000 words per call
- Renode PL011: 1-byte RX FIFO (hardwareFifoSize=1, not changeable)
- `tunnel_bus.py`: host→dev chunk=256/delay=0, dev→host chunk=1/delay=50μs

## Compiler Warnings (non-blocking)
- `host_bt.c:55`: const qualifier on `bus_set_handler_user`
- `host_wifi.c:166`: unused `notify_buf`
- `host_wifi.c:717`: array bounds
