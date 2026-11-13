# ADR 0005: Linux host as a full-MAC cfg80211 driver (no mac80211)

- **Date**: 2026-11-10
- **Status**: accepted

## Context

The Linux host integration ("cfg80211_serial") must expose the device's
Wi-Fi to userspace. The firmware (nRF70 radio core, NCS `nrf_wifi`) runs a
complete 802.11 Zephyr stack: scanning, authentication/association, 4-way
handshake, and all crypto happen in firmware. The host sees Ethernet L2
frames plus management commands, not an 802.11 frame interface. There is no
in-tree `drivers/net/wireless/nordic` to base the driver on (verified
2026-11-09).

## Decision

The driver is a **cfg80211 full-MAC driver with its own Ethernet netdev** —
the same class as `ipw2200` (full-MAC, firmware auth) and `brcmfmac`
(full-MAC SDIO/USB). mac80211 is not used: it would duplicate in the kernel
what the radio core already does.

Supporting decisions (full plan in
[linux/README.md](../../linux/README.md)):

- **D3/D4/D6**: the multiplexed link is exposed as in-kernel ports, one per
  traffic type, plus a tty per port; exclusive ownership per port; the USB
  driver claims the composite CDC-ACM device (ftdi_sio-style), not per
  interface via `cdc_acm`.
- **D8/D9**: STA only, single virtual interface, netdev MTU 1500 with the
  MAC taken from the device.
- **D10**: management RX (assoc response, deauth) is synthesized from
  `connect_result`/`disconnect_result` events — the firmware provides no
  management frames.
- **D12**: regulatory domain comes from `WifiRegDomain`; DFS flags pass
  through, CAC only if firmware gains it.

## Consequences

- The cfg80211 surface is large relative to the firmware's: many ops map to
  `-EOPNOTSUPP` (AP mode, ROC, TX power, station stats, TWT) — see the
  capability matrix in [linux/README.md](../../linux/README.md) §2.5.
- Scan results carry no IEs, so the driver fabricates minimal
  `cfg80211_bss` entries (R2); a beacon-IE schema extension is on the
  roadmap for full `iw scan` output.
- Keeping serial framing out of cfg80211 callbacks (layering rule in
  [linux/AGENTS.md](../../linux/AGENTS.md)) makes the codec and transport
  swappable — e.g. the Bluetooth path can move from `hciattach` (option A)
  to an in-kernel HCI driver (option B) without a rewrite.
