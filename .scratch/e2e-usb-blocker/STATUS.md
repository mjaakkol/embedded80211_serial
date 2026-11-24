# E2E USB Blocker — Continuation Notes

**Last updated:** 2026-11-24
**Status:** BLOCKED on nRF5340 target USB (2fe3:0001) not enumerating on host.

## Goal
Make the E2E network integration test (Wi-Fi IPv4/DHCP/ping + BT scan) PASS:
real nRF7002 DK board as **device** (runs appcore, presents USB CDC bus port) +
Renode as **host** (runs host app, drives Wi-Fi/BT over the bus).

## What is DONE (committed + pushed to origin/master)

All 5 session commits are pushed. Working tree clean.

| Commit | Description |
|--------|-------------|
| `8d02750` | fix(nrf7002dk): no-BL2 flash layout so the board boots |
| `2d2acfa` | fix(wifi): drive nRF70 connect via wifi_nm (no mgmt_ops.connect) |
| `c5fcf86` | chore(nrf7002dk): tune RAM/heap for wpa_supplicant; trim boot log |
| `711556f` | refactor(serial): replace work-queue dispatch with a dedicated RX thread |
| `7a3c520` | feat(host): PL011 DR-register poll RX + PTY tunnel for Renode E2E |

### Verified working
- Board boots cleanly (RTT: "Wi-Fi ready — proxy mode", no errors).
- Wi-Fi connect succeeds on device (RTT: `CONNECT_RESULT: status=0`, EAPOL 4-way completes) — **when a frame is dispatched**.
- Dedicated RX thread (`serial.c`) is in place: `uart_irq_callback` only does `k_sem_give`; `serial_rx_thread_fn` owns all FIFO reads. Stack = 3072 B.
- Host bus RX: PL011 DR-register poll (`bus_rx_poll_fn`, 2 ms cycle) + PTY tunnel (`tunnel_bus.py`).
- RTT observability: 4 KB ring at `_acUpBuffer` @0x20040040, CB @0x20041040.

## The Blocker: nRF5340 target USB not enumerating

### Symptoms
- `2fe3:0001` absent from `lsusb` and sysfs.
- `/dev/ttyACM0` and `/dev/ttyACM1` bound to SEGGER J-Link (1366:1061), NOT the nRF5340.
- Board is alive (RTT-confirmed, boots to "Wi-Fi ready — proxy mode").
- USB CDC ACM initialized on device side (RTT: "USB CDC ACM initialized successfully").

### Diagnostics performed (all exonerate the SW build)

1. **prj.conf revert test:** Restored pre-session `prj.conf` → build FAILED with
   FLASH overflow 5 KB + **RAM overflow 175 KB**. The old config (128 KB RTT buffer,
   32/24 net bufs, 60 KB Wi-Fi heap) cannot link. The RAM trims are **mandatory**,
   not a regression. `prj.conf` exonerated.

2. **USBD register probe (J-Link SWD, base 0x40036000):**
   - `INTENSET` = 0x01DFFFFD (interrupts armed)
   - `LOWPOWER` = 0 (normal mode)
   - `EVENTCAUSE` = 0 (no USB error)
   - `EVENTS_USBRESET` = 0, `EVENTS_SOF` = 0, `EVENTS_EP0SETUP` = 0 (zero USB traffic)
   - **`USBADDR` = 0x14 (20)** — device WAS enumerated (host assigned address).
     This proves firmware + descriptors + USB stack are correct.
   - Conclusion: USBD is powered and ready, but D+/D- are not reaching a live host port.

3. **Host-side:** No `2fe3` in `lsusb` (fast-polled, 30×50 ms). Stable 18-device count.
   `dmesg` not accessible (sudo password required).

### Root cause assessment
The physical USB path between the nRF5340's D+/D- pins and the host port is not
conducting. SW is exonerated. Remaining suspects (hardware):
- Board's application-USB connector (bent pin, poor contact, corrosion).
- nRF5340 USB PHY (marginal — can latch enough signal to assign an address, then drop).
- Host port not providing VBUS (data-only port).

### What was tried (no success)
- Moving cable to different host ports (multiple).
- Full board power cycle (multiple).
- Reattaching the "previous" (known-good) cable.
- **NOT tried:** multimeter VBUS/D+ probe on the board connector; `TASKS_DPDMDRIVE`
  to force D+ state for probing.

## Where to continue

### Immediate next steps (in order)

1. **Bench hardware check** (requires user at bench):
   - With cable on board's application-USB port + live host port, measure:
     - VBUS (pin 1) = 5 V?
     - D+ pull-up (~2.8 V with device attached)?
   - If VBUS absent → host port issue (try powered hub).
   - If VBUS present but no `2fe3` → board-side USB path fault (connector or PHY).
   - Alternative: use J-Link `TASKS_DPDMDRIVE` (write 0x40036058) to force D+/D-
     state, then probe with multimeter/oscilloscope.

2. **Once `2fe3:0001` enumerates and STAYS in `lsusb`:**
   - Identify the correct `/dev/ttyACM*` node (VID 2fe3, NOT the J-Link's 1366):
     ```bash
     for f in /dev/ttyACM*; do
       DEV=$(readlink -f "/sys/class/tty/$(basename $f)/device" 2>/dev/null)
       VID=$(cat "$DEV/idVendor" 2>/dev/null)
       [ "$VID" = "2fe3" ] && echo "USE: $f"
     done
     ```
   - Run the E2E tunnel bridge:
     ```bash
     pkill -f renode 2>/dev/null; sleep 1
     rm -f /tmp/renode_bus_pty /tmp/e80211-host-console.log
     /home/mikko/local/renode/renode -p -P 25997 --disable-gui < /dev/null > /tmp/renode_stdout.log 2>&1 &
     sleep 3
     cd /home/mikko/projects/embedded80211_serial
     timeout 120 python3 host/tests/tunnel_bus.py 25997 /dev/ttyACM0 \
         /tmp/e80211-host-console.log \
         build/host/zephyr/zephyr.elf \
         host/boards/renode_cortex_m33/support/renode_cortex_m33.repl
     ```
     **IMPORTANT:** `/dev/ttyACM0` must be the 2fe3 node, NOT the J-Link's.

3. **Check E2E results:**
   - Host console: `cat /tmp/e80211-host-console.log` — look for
     `E2E WIFI: CONNECTED`, `E2E WIFI: DHCP BOUND`, `E2E WIFI: PING SUMMARY`, `E2E WIFI: OK`.
   - Device RTT: J-Link decode pipeline (see below) — look for
     `Received complete packet of length 42` and `Message complete, length: 42`.

4. **If Wi-Fi E2E passes:** investigate BT failure
   (`bt_hci_driver: Endpoint binding failed with -11` in `zephyr/drivers/bluetooth/hci/ipc.c`).

5. **Once E2E fully passes — cleanup:**
   - Remove TEMP logging in `embedded80211/zephyr/host/bus.c`:
     - `debug_rx_count` counter
     - first-8-RX-bytes `LOG_INF` (lines ~169, 179-183)
     - ISR debug counters
   - **KEEP:** `bus_rx_poll_fn` poll work item, `rx_lock` spinlock,
     `wifi_mgmt_proxy.c` `WIFI EVT` logs (production state-transition logs).
   - Commit cleanup via eval `commit` namespace.

## Build / flash / debug reference

### Environment
```bash
source /home/mikko/ncs/env.sh   # NCS v3.4.2, Zephyr 4.4.2
```

### Device build
```bash
west build -d nrf7002dk/build -s nrf7002dk -b nrf7002dk/nrf5340/cpuapp/ns --sysbuild
```
- Artifacts: `nrf7002dk/build/nrf7002dk/zephyr/zephyr.elf`, `tfm_merged.hex`
- **Always use non-secure variant** (`cpuapp/ns`). Do NOT switch to secure.

### Host build
```bash
west build -d build/host -s host -b renode_cortex_m33 --no-sysbuild
```
- Artifact: `build/host/zephyr/zephyr.elf`

### Flash (J-Link, two sessions: erase then program)
- JLinkExe: `/home/mikko/local/jlink/JLink_Linux_V982_x86_64/JLinkExe`
- Probe SN: `852000646`, interface SWD
- **MUST pipe empty stdin:** `echo "" | JLinkExe ...`
- Command files use `connect 4000` syntax.
- Stop APP-domain watchdog before erase: `w4 0x4003C000 1`
- `mem8` reads capped at ~15 KB per command.

J-Link scripts (recreate in `/tmp` if missing):
```
# /tmp/flash_erase.jlink
device NRF5340_XXAA_APP
connect 4000
w4 0x4003C000 1
r
erase
exit

# /tmp/flash_prog.jlink
device NRF5340_XXAA_APP
connect 4000
w4 0x4003C000 1
r
loadfile /home/mikko/projects/embedded80211_serial/nrf7002dk/build/nrf7002dk/zephyr/tfm_merged.hex
exit

# /tmp/rtt_read_buf.jlink
device NRF5340_XXAA_APP
connect 4000
halt
mem32 0x20041064 1
mem32 0x20041068 1
mem8 0x20040040 0x1000
go
exit
```

### RTT decode pipeline
```bash
echo "" | /home/mikko/local/jlink/JLink_Linux_V982_x86_64/JLinkExe \
  -SelectEmuBySN 852000646 -if SWD -CommandFile /tmp/rtt_read_buf.jlink 2>&1 > /tmp/rtt_raw.txt
grep '^2004' /tmp/rtt_raw.txt | awk '{
  n=split($0,p," = "); if(n<2)next; h=p[2]; if(h !~ / /)next; gsub(/ /,"",h); print h
}' | python3 -c "
import sys,re
data=bytes.fromhex(sys.stdin.read().strip())
text=data.decode('utf-8',errors='replace')
text=re.sub(r'\x1b\[[0-9;]*m','',text)
print(text)
"
```

### USBD register probe (nRF5340, base 0x40036000)
```
# /tmp/usbd_check.jlink
device NRF5340_XXAA_APP
connect 4000
halt
mem32 0x40036100 1   # EVENTS_USBRESET
mem32 0x40036154 1   # EVENTS_SOF
mem32 0x4003615C 1   # EVENTS_EP0SETUP
mem32 0x40036470 1   # USBADDR
go
exit
```

### Key constraints
- **NEVER suggest the USB cable is faulty** (user explicitly forbade).
- RTT buffer max 4096 bytes (overlaps CB if larger).
- `run_e2e.sh` harness is unreliable — use manual tunnel bridge procedure.
- Build cache gotcha: `touch` source or `find build -name "serial.c.obj" -delete` if RTT shows stale logs.
- Upstream TF-M headers MUST stay pristine.
- Commit via eval `commit` namespace. Separate commit per concern.
