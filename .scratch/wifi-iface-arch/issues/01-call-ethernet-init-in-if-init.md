# 01 — Call `ethernet_init(iface)` in `virt_wifi_if_init`

Status: resolved
Type: task
Severity: High

SDK reference: NCS v3.4.2 (`/opt/nordic/ncs/v3.4.2`).

## Problem

`virt_wifi_if_init()` (`embedded80211/zephyr/host/host_wifi.c:753-766`) does **not**
call `ethernet_init(iface)`. Five of the six canonical in-tree Wi‑Fi drivers do
(nrf_wifi, esp32, esp_hosted, airoc, siwx91x); only nxp omits it.

`ethernet_init()` (`include/zephyr/net/ethernet.h:721`, "Initialize Ethernet L2 stack
for a given interface") is the only place that:

- sets `ctx->ethernet_l2_flags = NET_L2_MULTICAST`,
- registers `ctx->iface` and initializes `ctx->carrier_work` (`k_work_init` →
  `carrier_on_off`),
- registers multicast monitoring,
- calls `net_arp_init()`,
- sets `ctx->is_init = true`.

It is invoked only by each Ethernet driver's own init hook — there is no stack‑side
boot call. `net_arp_init()` (`subsys/net/l2/ethernet/arp.c:1021-1043`) is the **only**
initializer of `arp_free_entries` / `arp_pending_entries` / `arp_table`, the per‑entry
`pending_queue` fifos, `arp_request_timer`, and `arp_mutex`; the `:1025`
`if (arp_cache_initialized) return;` is an idempotency guard, **not** a lazy‑init.

## Evidence (production build)

- `renode_cortex_m33` generated DTS (`build/host/zephyr/zephyr.dts`) contains **only**
  the `virt-wifi` and `bt-hci-virtual` nodes — no other Ethernet MAC.
- `build/host/zephyr/zephyr.elf` contains **no `ethernet_init` / `net_arp_init`
  symbols** — link‑eliminated because nothing references them.

Therefore the ARP cache is **never initialized** in the production build. The E2E
gateway ping passing is fragile / build‑dependent: on QEMU the stellaris Ethernet
drivers call `ethernet_init()` and initialize ARP globally, masking the gap.

## Change

In `virt_wifi_if_init()`, after setting `eth_if_type` and the link address, add:

```c
ethernet_init(iface);
```

Requires `#include <zephyr/net/ethernet.h>` (verify it is already pulled in via the
existing includes; add if not).

## Acceptance

- `build/host/zephyr/zephyr.elf` now contains `ethernet_init` and `net_arp_init`
  symbols (confirm with `nm`/`strings`).
- E2E gateway ping (`E2E_SSID=MedallionNet host/tests/run_e2e.sh`) passes on the
  **real board** after the change — this is the regression that proves ARP is now
  initialized independent of any other Ethernet driver.
- No change to the wire protocol.

## Notes

- This is the canonical fix; it removes dependence on any other Ethernet driver
  being present to initialize ARP.
- Do not also add `carrier_on` here (see issue 02 for the correct not‑connected
  state).

## Comments

- 2026-11-20: Implemented. `ethernet_init(iface)` added in `virt_wifi_if_init()`
  (`embedded80211/zephyr/host/host_wifi.c`) after the link address, per the
  canonical offload contract. `<zephyr/net/ethernet.h>` was already included.
  Verified: `nm build/host/zephyr/zephyr.elf` now contains `ethernet_init` and
  `net_arp_init` (previously link-eliminated).
