# Zephyr protocol-test instructions

## Scope

These rules apply to [tests/](.) and supplement [../AGENTS.md](../AGENTS.md).

## Test design

- Keep tests deterministic and independent of physical serial hardware.
- Test public protocol constants and behavior rather than private
  implementation details.
- Cover header layout, traffic IDs, payload limits, alignment, endianness,
  acknowledgement/error encoding, independent TX/RX sequence counters, and
  sequence wraparound.
- Include malformed, truncated, oversized, unsupported, out-of-order, and
  buffer-exhaustion cases.
- Add regression tests for every discovered protocol bug and retain them when
  implementation details change.
- Keep test configuration minimal; document any board, simulator, SDK, or
  environment requirement in the test README.

## Verification

- Prefer the repository's `run_tests.sh` or focused CTest targets.
- Run formatting/build checks appropriate to the changed C and CMake files.
- Report the exact target, toolchain, and test command when a test cannot run.
