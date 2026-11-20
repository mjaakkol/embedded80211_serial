# Wi‑Fi virtual interface — architectural soundness findings

Status: resolved. Implementation applied 2026-11-20 — all five issues
(`issues/01`–`05`) are resolved; see each issue's `## Comments` for the applied
change and verification. One deviation: issue 05 sizes `tx_buf` to
`CONFIG_VIRT_WIFI_MTU + sizeof(struct net_eth_hdr)` (1550) so full-size frames fit.

SDK under audit: **NCS v3.4.2** at `/opt/nordic/ncs/v3.4.2` (the only NCS present on
this machine; v3.4.1 was removed). Primary source of truth: the in‑tree Wi‑Fi drivers
under `/opt/nordic/ncs/v3.4.2/zephyr/drivers/wifi/`.

Scope: `embedded80211/zephyr/host/host_wifi.c` (host `virt_wifi` offload driver) and
`host/src/e2e_test.c`, assessed against the canonical Zephyr Wi‑Fi offload driver
pattern.

## Origin

Original task: validate that the Zephyr host emulator acquires the MAC address and
other link‑specific attributes through `net_if` before starting Wi‑Fi activity.
That investigation surfaced an apparent "driver bug" (`eth_if_type == 0` at runtime)
which this document **corrects** and then expands into a full architectural audit.

## Root cause of the original investigation (correction)

The `eth_if_type == 0` reading was a **test artifact, not a driver bug.**

- The E2E test hard‑codes `VIRT_WIFI_IFACE_INDEX = 1` and the driver's own
  `virt_wifi_netif()` uses `net_if_get_by_index(1)` (`host_wifi.c:38`, `:174`).
- An interface index is an **allocation order**, not a stable identity. It is
  correct only when `virt_wifi` is the *sole* non‑loopback interface.
- On `qemu_cortex_m3` the `stellaris` Ethernet MAC (init priority 60) is allocated
  before `virt_wifi` (init priority 80), so `virt_wifi` lands at **index 2** and
  index 1 is the stellaris Ethernet port. The test then inspected the wrong
  interface and saw `eth_if_type == 0` (plain Ethernet).
- On the real target `renode_cortex_m33` there is **no** Ethernet MAC node, so
  `virt_wifi` is index 1 and the test happens to pass.

Runtime QEMU evidence (`/tmp/e80211-qemu2.log`):

```
idx=1 mac=00:00:94:00:83:00 mtu=1500 l2eth=0 up=1   # stellaris eth — WRONG iface
idx=2 mac=00:00:00:00:00:02 mtu=1536 l2eth=1 up=1   # virt_wifi   — CORRECT
idx=3 mac=00:00:5e:00:53:90 mtu=1500 l2eth=0 up=1   # stellaris eth
```

The driver itself sets `eth_if_type = L2_ETH_IF_TYPE_WIFI` and a link address
correctly (`host_wifi.c:753-766`). See issue 03 for the robust fix.

## Canonical Zephyr Wi‑Fi offload pattern (6‑driver audit, NCS v3.4.2)

All six in‑tree drivers were audited by reading the actual init‑hook bodies.
Citations are `file:line` in NCS v3.4.2.

| # | Driver (v3.4.2 path) | Registration macro | `eth_if_type` in init | MAC source | Init hook: `ethernet_init` / carrier / dormant |
|---|----------------------|--------------------|-----------------------|------------|-----------------------------------------------|
| 1 | **nrf_wifi** `nrf_wifi/src/net_if.c` | `ETH_NET_DEVICE_DT_INST_DEFINE` (`fmac_main.c:989,999`) | yes, `net_if.c:721` (no NULL guard) | **start hook** (OTP) | `ethernet_init` + `net_eth_carrier_on` + `net_if_dormant_on` (`net_if.c:722-724`) |
| 2 | **esp32** `esp32/src/esp_wifi_drv.c` | `NET_DEVICE_DT_INST_DEFINE` (`:1134,1141`) | yes, `:966` (sta) / `:1004` (ap), no guard | init: eFuse `esp_read_mac`, `:975/985` → `set_link_addr :991/1015` | `ethernet_init` + `net_if_carrier_off` (`:993-994` / `:1017-1018`) |
| 3 | **esp_hosted** `esp_hosted/esp_hosted_wifi.c` | `NET_DEVICE_INIT_INSTANCE` (`:678,682`) | yes, `:291`, no guard | init: host‑ctrl msg → `set_link_addr :294` | `ethernet_init` + `net_if_dormant_on` + `net_if_carrier_on` (`:297-299`) |
| 4 | **airoc** `infineon/airoc_wifi.c` *(moved from `airoc/`)* | `NET_DEVICE_DT_INST_DEFINE` (`:1013`) | yes, `:522`, no guard | init: WHD firmware `whd_wifi_get_mac_address :527` → `set_link_addr :535` | `ethernet_init` + `net_if_dormant_on` + `net_if_carrier_on` (`:540-546`) |
| 5 | **nxp** `nxp/nxp_wifi_drv.c` | `NET_DEVICE_INIT_INSTANCE` (`:2427,2471`) | yes, `:1967` (sta) / `:2008` (uap), no guard | **DT only** (never `set_link_addr`) | **none** in init hook (the single outlier) |
| 6 | **siwx91x** `siwx91x/siwx91x_wifi.c` | `ETH_NET_DEVICE_DT_INST_DEFINE` (`:668`) | yes, helper `:365`, no guard | init: chip `sl_wifi_get_mac_address :541` → `set_link_addr :546` | `ethernet_init` via helper `:366` (called `:552`) + `net_if_dormant_on :549` |

Synthesis of the canonical init‑hook contract (`struct net_if_api.init`, embedded as
`wifi_iface.iface_api.init` in `struct net_wifi_mgmt_offload`):

1. Set `net_if_l2_data(iface)->eth_if_type = L2_ETH_IF_TYPE_WIFI` — **all 6** do this;
   **none** guard `eth_ctx` for NULL.
2. Set the link address via `net_if_set_link_addr(..., NET_LINK_ETHERNET)` from
   hardware — **5 of 6** (nxp defers to DT).
3. Call `ethernet_init(iface)` — **5 of 6** (all except nxp).
4. Set the not‑connected link state: `net_if_carrier_off` and/or `net_if_dormant_on`
   — **5 of 6** (nrf_wifi is the outlier that asserts carrier **on** because its FMAC
   firmware is already up; it uses the newer `net_eth_carrier_on()` API).

Registration macro: `ETH_NET_DEVICE_DT_INST_DEFINE` (nrf_wifi, siwx91x) and the
explicit `NET_DEVICE_DT_INST_DEFINE(..., ETHERNET_L2, NET_L2_GET_CTX_TYPE(ETHERNET_L2))`
(esp32, airoc) are **byte‑equivalent** — `ETH_NET_DEVICE_DT_INST_DEFINE` expands
through `Z_ETH_NET_DEVICE_INIT_INSTANCE` to `ETHERNET_L2` +
`NET_L2_GET_CTX_TYPE(ETHERNET_L2)` (`include/zephyr/net/ethernet.h:1131-1156`).
`esp_hosted`/`nxp` use the non‑DT `NET_DEVICE_INIT_INSTANCE` (no device tree node).

## Assessment of `host_wifi.c`

### Correct — no change needed

- **Registration macro.** Explicit `NET_DEVICE_DT_INST_DEFINE(0, ..., ETHERNET_L2,
  NET_L2_GET_CTX_TYPE(ETHERNET_L2), CONFIG_VIRT_WIFI_MTU)` is the esp32/airoc spelling
  and equivalent to `ETH_NET_DEVICE_DT_INST_DEFINE`. Canonical.
- **`eth_if_type` set in the init hook** (`host_wifi.c:757-759`). Matches all 6
  drivers. The `eth_ctx != NULL` guard is *more* defensive than any upstream driver
  (none guard `eth_ctx`) — harmless, not a defect.
- **Static MAC `00:00:00:00:00:02`** set via `net_if_set_link_addr` in the init hook
  (`host_wifi.c:761-765`). A locally‑administered static MAC is the only sensible
  option for a virtual device; the placement matches esp32/esp_hosted/airoc/siwx91x.
- **`net_if_up()` → `ethernet_enable()` is a safe no‑op** under
  `CONFIG_WIFI_USE_NATIVE_NETWORKING=y`: `struct ethernet_api` embeds
  `struct net_if_api` at offset 0, and the driver provides no `start`/`stop`, so
  `eth->start()`/`eth->stop()` resolve to NULL.

### Deviations / defects (see issues)

| Issue | Severity | Summary |
|-------|----------|---------|
| 01 | **High** | `virt_wifi_if_init` does **not** call `ethernet_init(iface)`. 5 of 6 canonical drivers do. Consequence: `net_arp_init()`, multicast flags, and the L2 `carrier_work` are never initialized for the Wi‑Fi iface. |
| 02 | Medium | Init hook sets neither `net_if_carrier_off` nor `net_if_dormant_on`. Canonical not‑connected state is `carrier_off` (+ `dormant_on`). |
| 03 | **High** | Interface resolved by hard‑coded index 1 in **both** the driver (`virt_wifi_netif()`, `host_wifi.c:174`) and the E2E test (`VIRT_WIFI_IFACE_INDEX`). Board‑dependent; breaks whenever any other net iface is allocated first (e.g. QEMU stellaris eth). |
| 04 | Medium | `iface_index = 1` is baked into the **wire** protocol in every mgmt request (`host_wifi.c:426,458,494,517,554,578-579,668`), conflating the host's local `net_if` index with a protocol value. |
| 05 | **High** | Latent **stack over‑read** in `virt_wifi_l2_send` (`host_wifi.c:183-208`): the linearization loop clamps the copy to `sizeof(tx_buf)` (1536) but `bus_send(..., total_len)` reads `total_len` bytes, which for a frame > 1536 bytes (MTU 1536 + L2 headers) reads past the stack buffer. |

### The `ethernet_init()` omission in detail (issue 01)

`ethernet_init()` (`include/zephyr/net/ethernet.h:721`, "Initialize Ethernet L2 stack
for a given interface") is what:

- sets `ctx->ethernet_l2_flags = NET_L2_MULTICAST`,
- registers `ctx->iface` and initializes `ctx->carrier_work` (`k_work_init` →
  `carrier_on_off`),
- registers multicast monitoring,
- calls `net_arp_init()`,
- sets `ctx->is_init = true`.

It is called only by each Ethernet driver's own init hook — there is no stack‑side
boot call. Confirmed for the production build:

- The `renode_cortex_m33` generated DTS (`build/host/zephyr/zephyr.dts`) contains
  **only** the `virt-wifi` and `bt-hci-virtual` nodes — no other Ethernet MAC.
- The built `build/host/zephyr/zephyr.elf` contains **no `ethernet_init` /
  `net_arp_init` symbols** — link‑eliminated because nothing references them.
- `arp_cache_initialized` is set only inside `net_arp_init()` with **no lazy‑init**
  in the ARP request path (`subsys/net/l2/ethernet/arp.c`).

So in the production `renode_cortex_m33` build the ARP cache is **never
initialized**. The E2E gateway ping passing is therefore fragile / build‑dependent
(on QEMU the stellaris eth drivers call `ethernet_init()` and initialize ARP
globally, masking the gap). Adding `ethernet_init(iface)` to `virt_wifi_if_init` is
the canonical fix and removes the dependence on any other Ethernet driver being
present. **Verify the gateway ping on the real board after the change.**

## v3.4.2 vs v3.4.1 (what changed for this audit)

- **airoc moved** from `drivers/wifi/airoc/` to `drivers/wifi/infineon/`.
- **nrf_wifi** and **esp32** sources moved under a `src/` subdirectory.
- **nrf_wifi** now uses the newer `net_eth_carrier_on()` API (`net_if.c:723`) instead
  of `net_if_carrier_on()`.
- The canonical init‑hook contract (eth_if_type / set_link_addr / ethernet_init /
  carrier+dormant) is **unchanged** in substance; all six drivers behave the same as
  in v3.4.1. The findings and recommendations are therefore identical.

## Out of scope

- Device‑side (`embedded80211/zephyr/device/`) driver.
- Link‑protocol / bus framing changes.
- No driver code was modified by this assessment (per prior user instruction to
  leave driver changes to a follow‑up implementation pass).
