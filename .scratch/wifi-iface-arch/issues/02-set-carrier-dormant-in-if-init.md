# 02 — Set `carrier_off` + `dormant_on` in the init hook

Status: ready-for-agent
Type: task
Severity: Medium

SDK reference: NCS v3.4.2 (`/opt/nordic/ncs/v3.4.2`).

## Problem

`virt_wifi_if_init()` (`embedded80211/zephyr/host/host_wifi.c:753-766`) sets neither
`net_if_carrier_off` nor `net_if_dormant_on`. The canonical not‑connected Wi‑Fi state
is `carrier_off` and/or `dormant_on`.

Reference (init hooks, v3.4.2):

- esp32: `ethernet_init` + `net_if_carrier_off` (`esp32/src/esp_wifi_drv.c:993-994`)
- esp_hosted: `ethernet_init` + `net_if_dormant_on` + `net_if_carrier_on`
  (`esp_hosted/esp_hosted_wifi.c:297-299`)
- airoc: `ethernet_init` + `net_if_dormant_on` + `net_if_carrier_on`
  (`infineon/airoc_wifi.c:540-546`)
- siwx91x: `ethernet_init` + `net_if_dormant_on` (`siwx91x/siwx91x_wifi.c:549,552`)
- nrf_wifi: `ethernet_init` + `net_eth_carrier_on` + `net_if_dormant_on`
  (`nrf_wifi/src/net_if.c:722-724`) — outlier, asserts carrier because its FMAC
  firmware is already up.

For a virtual Wi‑Fi device that starts **disconnected**, the correct initial state is
`carrier_off` + `dormant_on` (no link, not ready for traffic). The driver already
toggles `net_if_carrier_off`/`net_if_carrier_on` on connect/disconnect events
(`host_wifi.c:295-302`), so the init hook should establish the disconnected baseline.

## Change

In `virt_wifi_if_init()`, after `ethernet_init(iface)` (issue 01), add:

```c
net_if_carrier_off(iface);
net_if_dormant_on(iface);
```

## Acceptance

- After `net_if_up()`, the Wi‑Fi iface reports carrier **off** and dormant **on**
  until a successful connect event flips them on.
- E2E connect flow still brings the interface up and passes.

## Notes

- Order matters: set these **after** `ethernet_init(iface)` so the L2 context is
  initialized first.
- Do not assert `carrier_on` in init (that is the nrf_wifi FMAC‑specific outlier).
