# 03 — Resolve the Wi‑Fi interface robustly (drop hard‑coded index 1)

Status: ready-for-agent
Type: task
Severity: High

## Problem

The Wi‑Fi interface is resolved by a **hard‑coded index 1** in two places:

- Driver: `virt_wifi_netif()` uses `net_if_get_by_index(1)` with a
  `net_if_get_default()` fallback (`embedded80211/zephyr/host/host_wifi.c:174`).
- E2E test: `VIRT_WIFI_IFACE_INDEX = 1` (`host/src/e2e_test.c`).

An interface index is an **allocation order**, not a stable identity. It is correct
only when `virt_wifi` is the *sole* non‑loopback interface. On `qemu_cortex_m3` the
`stellaris` Ethernet MAC (init priority 60) is allocated before `virt_wifi`
(priority 80), so `virt_wifi` lands at **index 2** and index 1 is the stellaris
Ethernet port. The test then inspects the wrong interface and sees
`eth_if_type == 0` (plain Ethernet) — this is the root cause of the original
"driver bug" report. On `renode_cortex_m33` there is no Ethernet MAC, so `virt_wifi`
is index 1 and it happens to pass.

Runtime QEMU evidence (`/tmp/e80211-qemu2.log`):

```
idx=1 mac=00:00:94:00:83:00 mtu=1500 l2eth=0 up=1   # stellaris eth — WRONG iface
idx=2 mac=00:00:00:00:00:02 mtu=1536 l2eth=1 up=1   # virt_wifi   — CORRECT
idx=3 mac=00:00:5e:00:53:90 mtu=1500 l2eth=0 up=1   # stellaris eth
```

## Change

Resolve the Wi‑Fi interface by **type**, not index. Add a helper that walks
`net_if_foreach()` and returns the first interface whose
`net_if_l2_data(iface)->eth_if_type == L2_ETH_IF_TYPE_WIFI` (and whose device is the
`virt_wifi` device, to be safe). Use it in `virt_wifi_netif()` and in the E2E test.

Sketch:

```c
static struct net_if *virt_wifi_netif(void)
{
	struct net_if *found = NULL;

	net_if_foreach((net_if_iter_func_t)match_wifi, &found);
	return found;
}
```

where `match_wifi` checks `eth_if_type == L2_ETH_IF_TYPE_WIFI` (and, if needed,
`net_if_get_device(iface) == virt_wifi_dev`).

## Acceptance

- E2E passes on **both** `renode_cortex_m33` and `qemu_cortex_m3` (the QEMU case is
  the regression that proves index‑independence).
- The test asserts `eth_if_type == L2_ETH_IF_TYPE_WIFI` on the resolved interface
  (not on a fixed index).

## Notes

- This is the fix that makes the original "MAC / link attributes acquired through
  net_if" validation meaningful on every board.
- Keep the `net_if_get_default()` fallback only as a last resort; prefer the
  type‑based match.
