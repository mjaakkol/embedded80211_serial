# 04 — Decouple the wire `iface_index` from the host local `net_if` index

Status: resolved
Type: task
Severity: Medium

## Problem

`iface_index = 1` (the `VIRT_WIFI_IFACE_INDEX` macro) is baked into the **wire**
protocol in every management request:

- `scan_req.iface_index` (`host_wifi.c:426`)
- `connect_req.iface_index` (`host_wifi.c:458`)
- `disconnect_req.iface_index` (`host_wifi.c:494`)
- `ap_enable_req.iface_index` (`host_wifi.c:517`)
- `ap_disable_req.iface_index` (`host_wifi.c:554`)
- `get_iface_status_req.iface_index` (`host_wifi.c:578-579`)
- `get_ps_config_req.iface_index` (`host_wifi.c:668`)

This conflates the **host's local `net_if` index** (a board‑dependent allocation
order, see issue 03) with a **protocol value** the device expects. If the host's
local index ever changes (e.g. another interface is allocated first on QEMU), the
wire value sent to the device is wrong even though the host‑side logic is fine.

## Change

Define the protocol's `iface_index` as a **stable, device‑side constant** (the
device has exactly one virtual Wi‑Fi interface, so `0` is the natural value) and stop
deriving it from the host's local `net_if` index. Introduce a dedicated macro, e.g.:

```c
#define WIFI_PROTO_IFACE_INDEX 0   /* device-side VIF id, stable on the wire */
```

and use it for every `*_req.iface_index` field. Keep the host‑local index resolution
(issue 03) separate from this protocol constant.

## Acceptance

- The wire `iface_index` is a fixed protocol constant, independent of the host's
  local `net_if` index.
- Device‑side parsing is unchanged (it already expects a single VIF id).
- E2E passes on both `renode_cortex_m33` and `qemu_cortex_m3`.

## Notes

- Coordinate the chosen constant with the device‑side parser
  (`embedded80211/zephyr/device/`) so both sides agree on the VIF id.
- This is a wire‑protocol clarification, not a breaking change, provided the device
  already treats the single VIF as id `0` (verify against the device parser before
  changing the value).

## Comments

- 2026-11-20: Implemented. `WIFI_PROTO_IFACE_INDEX 0` is now the wire constant for
  every `*_req.iface_index` field in `host_wifi.c` (scan, connect, disconnect,
  ap_enable, ap_disable, get_iface_status, get_ps_config, get_reg_domain — eight
  sites, one more than the original list: `get_reg_domain_req`). Verified against
  the device parser: `resolve_wifi_iface()` in
  `embedded80211/zephyr/device/wifi_mgmt_proxy.c` treats `ifindex == 0` as
  "unspecified" and maps it to the first registered Wi-Fi interface, so `0` is
  stable and no device-side change was needed. The host-local index is no longer
  part of the wire path; `VIRT_WIFI_IFACE_INDEX` was removed.
