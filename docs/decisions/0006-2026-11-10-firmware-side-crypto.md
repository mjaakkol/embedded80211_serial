# ADR 0006: All Wi-Fi crypto in firmware (no host key installation)

- **Date**: 2026-11-10
- **Status**: accepted

## Context

The radio core runs a full 802.11 stack including the 4-way handshake and
frame encryption (CCMP, SAE). The link protocol carries management commands
and Ethernet L2 frames; it has no key-management message surface in the MVP
proto.

## Decision

**All key management terminates in the firmware.** The Linux cfg80211 driver
never installs keys: `add_key`/`del_key`/`get_key`/`set_key` are not
implemented (they are cfg80211 params that terminate in the driver), and
connect requests send `WpaKeys`/`pmk`/IEs only as capability signaling — the
`pmk` has no proto field and `wpa_keys` are never placed on the wire. The
driver advertises cipher suites as capability only.

Enterprise/EAP remains out of scope: the proto enumerates EAP fields but the
firmware has no working credential path (R5); `eap_*` connect fields
terminate in the host and are never sent.

## Consequences

- Host-side security code is minimal: cipher selection reduces to
  advertising capability.
- The device is the trust boundary for Wi-Fi credentials; the host never
  holds long-term keys.
- Enterprise/WPA3-Enterprise support requires a firmware credential path
  first (EAPOL forwarding on the roadmap, [../linux/README.md](../../linux/README.md)
  §2.7 item 4).
