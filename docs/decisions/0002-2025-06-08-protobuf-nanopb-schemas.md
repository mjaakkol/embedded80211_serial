# ADR 0002: Protobuf (nanopb) as the schema language

- **Date**: 2025-06-08
- **Status**: accepted

## Context

The Wi-Fi management and network-interface management traffic types carry
structured, evolving messages between host and device. A schema language was
needed that is well-understood, efficient to (de)serialize on both a Zephyr
MCU and host systems, and stable across independently built firmware and
hosts.

## Decision

- Google Protobuf is the schema language for all structured technology
  payloads; schemas live in `protos/` and are shared by every build.
- Device-side code is generated to nanopb at build time; generated artifacts
  are never committed. The Linux host is expected to use build-time codegen
  producing kernel-safe C (no libc) — decided at its Phase 6.
- Field numbers are never renumbered; additions use new field numbers and
  removed numbers are reserved where possible.
- The nanopb runtime is added by a build only when the consuming build does
  not already provide `modules__nanopb` (no duplicate runtime).

## Consequences

- Hosts and firmware can evolve independently as long as schemas stay
  wire-compatible; a breaking wire change requires a protocol-version update
  and compatibility documentation.
- Protobuf deserialization is a known attack surface: every payload is
  validated (lengths, bounds) before use, per the root AGENTS.md rules.
- Rust `cli_test` generates its bindings from the same `protos/` schemas via
  `protoc`, keeping all three implementations in lockstep.
