# Linux integration instructions

## Scope

These rules apply to Linux host integration and the future cfg80211 driver.
Repository-wide protocol requirements are defined in
[../AGENTS.md](../AGENTS.md).

## Driver architecture

- Map the device's full-MAC Wi-Fi management operations to cfg80211 without
  leaking serial framing into cfg80211 callbacks.
- Keep serial transport, common-link framing, protobuf messages, and cfg80211
  registration in separate layers with explicit ownership and lifetime rules.
- Use kernel-managed allocation and synchronization primitives appropriate for
  the target kernel; do not assume user-space APIs or libc are available.
- Keep probe, disconnect, suspend/resume, and teardown paths safe when requests
  or responses are in flight.
- Make worker, interrupt, and callback contexts explicit. Do not sleep in
  atomic or interrupt context.

## Protocol and data handling

- Validate every frame before passing data to cfg80211 or the network stack.
- Bound allocations by the protocol maximum and handle partial reads, short
  writes, sequence errors, acknowledgements, and device resets.
- Avoid copying Wi-Fi data unnecessarily, but prioritize ownership, lifetime,
  and DMA/cache correctness over micro-optimizations.
- Keep protocol-version and capability negotiation explicit so unsupported
  traffic types fail clearly.

## Verification

- Keep code buildable with the supported kernel configuration and toolchain.
- Test framing and state machines independently of hardware where possible.
- Exercise probe/remove, timeout, reset, malformed-frame, sequence-recovery,
  and concurrent request paths.
- Document the target kernel API assumptions and any hardware-only validation
  that was not run.
