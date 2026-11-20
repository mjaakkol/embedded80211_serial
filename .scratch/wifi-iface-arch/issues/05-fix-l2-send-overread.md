# 05 — Fix latent stack over‑read in `virt_wifi_l2_send`

Status: ready-for-agent
Type: task
Severity: High

## Problem

`virt_wifi_l2_send()` (`embedded80211/zephyr/host/host_wifi.c:183-208`) can read past
the end of its stack buffer:

```c
size_t total_len = net_buf_frags_len(pkt->frags);   // :185  total bytes across all frags
uint8_t tx_buf[1536];                                // :186  fixed 1536-byte stack buffer
...
while (frag != NULL && off < total_len) {            // :194
    size_t flen = frag->len;
    if (off + flen > sizeof(tx_buf)) {               // :196  clamp copy to 1536
        flen = sizeof(tx_buf) - off;
    }
    memcpy(tx_buf + off, frag->data, flen);          // :199
    off += flen;
    frag = frag->frags;
}
int rc = bus_send(SERIAL_TYPE_WIFI_DATA, tx_buf, total_len);  // :204  reads total_len bytes
```

The loop clamps the **copy** to `sizeof(tx_buf)` (1536), so at most 1536 bytes are
written into `tx_buf`. But `bus_send(..., tx_buf, total_len)` reads **`total_len`**
bytes. When `total_len > 1536`, this reads `total_len - 1536` bytes past the end of
the stack buffer — a **stack over‑read**.

This is triggerable, not theoretical: `CONFIG_VIRT_WIFI_MTU` is 1536, and an
Ethernet frame is payload (up to MTU) + 14‑byte L2 header + 4‑byte FCS, i.e. up to
~1554 bytes. A full‑size frame exceeds 1536 and triggers the over‑read.

## Change

Send the number of bytes actually copied (`off`), and **explicitly reject** frames
larger than the buffer (per repo rule: return/report failures explicitly; never
silently downgrade to success):

```c
if (total_len > sizeof(tx_buf)) {
    net_pkt_unref(pkt);
    return -EMSGSIZE;          /* frame too large for the bus buffer */
}
/* ... linearization loop (no clamp needed now) ... */
int rc = bus_send(SERIAL_TYPE_WIFI_DATA, tx_buf, off);   /* off == total_len here */
```

## Acceptance

- A frame ≤ 1536 bytes is sent in full (`off == total_len`).
- A frame > 1536 bytes is **rejected** with an explicit error (no over‑read, no
  silent truncation).
- No change to the wire protocol for in‑size frames.

## Notes

- The clamp at `:196-198` becomes dead code once the size check is added; remove it.
- Verify the bus layer's max payload (`SERIAL_TYPE_WIFI_DATA`) is ≥ 1536 so the
  buffer size matches the protocol limit; if the protocol limit is smaller, size
  `tx_buf` to that and reject accordingly.
