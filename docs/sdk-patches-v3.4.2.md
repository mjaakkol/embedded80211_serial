# NCS v3.4.2 SDK Patches — embedded80211_serial

Temporary diagnostic patches applied to the NCS v3.4.2 SDK for E2E
bring-up debugging. These are **not** part of the project source tree
and must be re-applied (or confirmed unnecessary) after an SDK update.

**Status:** All patches are diagnostic-only. No functional behaviour is
changed. Revert all before a clean release build.

---

## 1. `zephyr/drivers/wifi/nrf_wifi/src/net_if.c`

The Zephyr `nrf_wifi` driver (compiled in this build; the nrf71 FMAC
driver at `nrf/drivers/wifi/nrf71/` is **not** in the build).

### 1a. Static diagnostic buffers (after line 37, `#endif CONFIG_NRF70_STA_MODE`)

```c
/* Temporary MAC diagnostics (embedded80211) */
struct macdbg_info {
	volatile uint8_t initial[6];
	volatile uint8_t valid;
	volatile uint8_t after[6];
	volatile uint8_t config_called;
	volatile uint8_t config[6];
};
static struct macdbg_info macdbg = {
	.initial = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF },
	.valid = 0xEE,
};
/* Temporary TX stats diagnostics (embedded80211) */
static volatile void *txstats_fmac_ctx;
```

**Purpose:** J-Link-readable RAM buffer capturing MAC address state
through VIF creation and config op. Sentinel `0xAA..0xFF` in `initial`
confirms the buffer was not yet written.

### 1b. Sentinel in `nrf_wifi_if_start_zep()` (top of function, ~line 819)

```c
	macdbg.initial[0] = 0x42; /* E80211: start_zep entered */
```

**Purpose:** Confirms `nrf_wifi_if_start_zep` was called.

### 1c. `txstats_fmac_ctx` capture + offset log (~line 887, after `fmac_dev_ctx = rpu_ctx_zep->rpu_ctx`)

```c
	txstats_fmac_ctx = fmac_dev_ctx;
	LOG_INF("TXSTATS: fmac_ctx=%p fw_stats_off=%lu",
		(void *)fmac_dev_ctx,
		(unsigned long)offsetof(struct nrf_wifi_fmac_dev_ctx, fw_stats));
```

**Purpose:** Captures the `fmac_dev_ctx` pointer and prints the
`fw_stats` offset so `rpu_host_stats` can be read via J-Link at
`fmac_ctx + fw_stats_off`.

### 1d. MAC capture in VIF creation (~line 923, after `mac_addr = net_if_get_link_addr(...)`)

```c
	macdbg.initial[0] = mac_addr[0];
	macdbg.initial[1] = mac_addr[1];
	/* ... bytes 2-5 ... */
	macdbg.valid = nrf_wifi_utils_is_mac_addr_valid(mac_addr);
```

**Purpose:** Records the MAC as read from the net_if link address
before any FMAC override.

### 1e. MAC capture after FMAC override (~line 944, inside the `if (!valid)` branch)

```c
	macdbg.after[0] = vif_ctx_zep->mac_addr.addr[0];
	macdbg.after[1] = vif_ctx_zep->mac_addr.addr[1];
	/* ... bytes 2-5 ... */
```

**Purpose:** Records the MAC after the driver applies
`CONFIG_WIFI_FIXED_MAC_ADDRESS` (or OTP).

### 1f. TX-path carrier/authorization diagnostic (~line 502, in `nrf_wifi_if_send`, before the carrier-state drop check)

```c
		/* Temporary diagnostic (embedded80211) */
		LOG_INF("TXCHK: carr=%d auth=%d peer=%d eapol=%d",
			vif_ctx_zep->if_carr_state, authorized, peer_id,
			is_eapol(pkt));
```

**Purpose:** Confirms whether frames are dropped due to
`if_carr_state != ON` or `authorized == false`.

### 1g. Config-op MAC capture (~line 1234, in the config op handler)

```c
		macdbg.config_called = 1;
		macdbg.config[0] = vif_ctx_zep->mac_addr.addr[0];
		macdbg.config[1] = vif_ctx_zep->mac_addr.addr[1];
		/* ... bytes 2-5 ... */
```

**Purpose:** Records whether and what MAC the config op sets.

### Revert checklist
- Remove lines 38–51 (macdbg struct + txstats_fmac_ctx)
- Remove line 819 (sentinel)
- Remove lines 887–890 (txstats capture + LOG_INF)
- Remove lines 923–929 (macdbg.initial fill)
- Remove lines 944–949 (macdbg.after fill)
- Remove lines 502–505 (TXCHK LOG_INF)
- Remove lines 1234–1240 (macdbg.config fill)

---

## 2. `nrf/drivers/wifi/nrf71/src/net_if.c`

The nrf71 FMAC driver. **Not compiled in this build** (the build uses
the Zephyr `nrf_wifi` driver). Patches are inert but present for
cleanliness.

### 2a. Static diagnostic buffer (after line 37)

```c
/* Temporary MAC diagnostics (embedded80211) */
struct macdbg_info {
	volatile uint8_t initial[6];
	volatile uint8_t valid;
	volatile uint8_t after[6];
	volatile uint8_t config_called;
	volatile uint8_t config[6];
};
static struct macdbg_info macdbg;
```

### 2b. MAC captures (lines 873, 889, 1171–1172)

Same pattern as §1d/1e/1g but using `memcpy` (the nrf71 driver does
not have the volatile-optimization issue).

### Revert checklist
- Remove lines 38–48 (macdbg struct + static)
- Remove lines 873–874 (macdbg.initial memcpy)
- Remove line 889 (macdbg.after memcpy)
- Remove lines 1171–1172 (macdbg.config memcpy)

---

## 3. `zephyr/subsys/usb/device_next/class/usbd_cdc_acm.c`

### 3a. Removed log line (original ~line 828)

The following `LOG_INF` was **removed** to reduce RTT ring spam:

```c
	LOG_INF("UART dev %p, len %d, remaining space %u",
		(void *)dev, len, remaining_space);
```

This log fires on every CDC-ACM bulk-OUT transfer and floods the 128 KB
RTT ring, overwriting useful diagnostics.

### Revert checklist
- Re-add the `LOG_INF` if USB-level debug tracing is needed.
  The log is at the top of the `usbd_cdc_acm_data_rx()` function,
  after the `remaining_space` calculation.

---

## 4. Project-side changes (in repo, already committed)

These are in the project source tree (not SDK) and are committed in
`810f42a`:

| File | Change |
|------|--------|
| `nrf7002dk/prj.conf` | `CONFIG_WIFI_FIXED_MAC_ADDRESS="62:34:56:78:9a:bc"` (replaced `CONFIG_WIFI_OTP_MAC_ADDRESS=y`) |
| `nrf7002dk/prj.conf` | `CONFIG_USBD_LOG_LEVEL_DBG=y`, `CONFIG_UDC_DRIVER_LOG_LEVEL_DBG=y`, `CONFIG_USBD_CDC_ACM_LOG_LEVEL_DBG=y` |
| `embedded80211/zephyr/device/wifi_data.c` | `tx_macdbg_*` statics + `DHCP42` diagnostic + src-MAC rewrite |
| `embedded80211/zephyr/CMakeLists.txt` | `usbd_init.c` source |
| `embedded80211/zephyr/device/serial/usbd_init.c` | New: explicit USBD init |

---

## 5. Key findings (context for SDK update validation)

1. **The Zephyr `nrf_wifi` driver** (`zephyr/drivers/wifi/nrf_wifi/`)
   is the compiled driver, **not** the nrf71 FMAC driver
   (`nrf/drivers/wifi/nrf71/`). All driver analysis must target the
   Zephyr `nrf_wifi` driver.

2. **`CONFIG_WIFI_FIXED_MAC_ADDRESS` works correctly**: the VIF
   creation path sets the link address to the configured unicast MAC
   (`62:34:56:78:9a:bc`). The `nrf_wifi_sys_fmac_set_vif_macaddr`
   command is sent with the correct MAC.

3. **The FMAC receives frames but does not transmit them**:
   `carr=1` (associated), `auth=1` (authorized), `peer=0` (valid).
   Frames enter `nrf_wifi_fmac_start_xmit` successfully. The FMAC
   firmware is not transmitting queued frames. Root cause is at the
   FMAC/RPU firmware level.

4. **`offsetof(struct nrf_wifi_fmac_dev_ctx, fw_stats)` = 16** in
   v3.4.2. The `rpu_host_stats` struct (5 × `uint64_t` = 40 bytes)
   is at `fmac_dev_ctx + 16`.

5. **`memcpy` to `volatile` arrays is optimised out** by the compiler
   in the Zephyr `nrf_wifi` driver. Use direct byte assignments for
   diagnostic buffers.
