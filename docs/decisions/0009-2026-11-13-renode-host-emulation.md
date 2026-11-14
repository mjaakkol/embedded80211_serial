# ADR 0009: Host emulation on Renode (app-local `renode_cortex_m3` board)

- **Date**: 2026-11-13
- **Status**: accepted

## Context

The host app (`host/`) had been emulated with QEMU (`qemu_cortex_m3`,
`lm3s6965evb` board), with the bus UART bridged to the device port via
`socat` + a PTY pair (`host/run_qemu.sh`). This setup was broken in the
current development environment (macOS arm64): `socat` is not installed, and
the QEMU board's Stellaris ENET needed a permanent `&eth { status =
"disabled"; }` overlay hack to keep the virtual Wi-Fi interface at index 1.

Constraints:

- NCS v3.4.1 (Zephyr 4.4.2) ships **no Renode board**; Zephyr mainline has
  only `cortex_r8_virtual` / `riscv32_virtual` (no Cortex-M3). No out-of-tree
  SDK patching is acceptable for this.
- NCS `west build` enables sysbuild by default; the app must build with
  `--no-sysbuild`, and Kconfig warnings are hard errors.
- The wire protocol is invariant; the bus must remain a plain UART that a
  serial port (the nRF7002 DK's CDC-ACM) can sit on the other end of.

## Decision

Migrate the host app to **Renode** (v1.17.0), with an **app-local board +
SoC pair** in the app tree so the SDK stays untouched:

- `host/boards/renode_cortex_m3/` — `board.yml`, `board.cmake` (sets
  `SUPPORTED_EMU_PLATFORMS renode`), `Kconfig.renode_cortex_m3`,
  `renode_cortex_m3.dts`, `renode_cortex_m3_defconfig`, and
  `support/renode_cortex_m3.repl` (machine: 256 KiB ROM @ 0x0, 32 MiB RAM @
  0x20000000, Cortex-M3 @ 100 MHz, two PL011 UARTs) +
  `support/renode_cortex_m3.resc` (Zephyr-native `west build -t run`
  script).
- `host/soc/renode_cortex_m3/` — Cortex-M3 SoC (NVIC, `cortex_m` linker
  script, 100 MHz system clock).
- `host/CMakeLists.txt` prepends `BOARD_ROOT`/`SOC_ROOT` before
  `find_package(Zephyr)` so `west build -b renode_cortex_m3` resolves the
  app-local board.

Runtime plumbing (replaces `run_qemu.sh`, deleted):

- `host/run_renode.sh` — resolves Renode (`$RENODE` → `PATH` →
  `~/tools/renode/Renode.app/Contents/MacOS/renode`), auto-detects the device
  bus port (`tests/find_bus_port.py`), and launches Renode with the bus UART
  (uart1) attached to a **socket terminal** on 127.0.0.1:25999 and the
  console (uart0) to a log file.
- `host/tests/tunnel_bus.py` — bridges the bus socket to the device serial
  port (two pump threads). No `socat`, no PTY pair in the run script.

The bus stays a UART end-to-end; the socket terminal is pure Renode-side
plumbing. Renode 1.17 specifics baked into the scripts: `.repl` files are
declarations-only, the machine is created with `mach add` (no name), the CPU
needs an explicit `s` after `LoadELF`, a closed monitor stdin exits Renode
(the runner keeps it open via a FIFO), and the old `> pty:`/`> tcp:`
connect syntax is gone.

## Consequences

Easier:

- `west build -t run` works natively for the host app (Zephyr's Renode
  emulation support), and `run_renode.sh` adds the bus bridge.
- No `socat` dependency; the bridge is a small, testable Python script.
- 32 MiB RAM removes the 64 KB QEMU RAM constraint on net_buf/heap sizes.
- No ENET/`&eth` overlay hack: the app-local board has no Ethernet node.
- The emulator's **instant frame delivery** exposed a real liveness bug in
  `embedded80211/zephyr/host/bus.c` (the RX ISR left a full payload in the
  PL011 FIFO, so the level-triggered RX interrupt never deasserted and the
  workqueue that was supposed to drain the FIFO was starved — a hard system
  wedge). The ISR now drains the payload/trailer; the work item only
  validates and dispatches. Serial hardware (byte-at-a-time arrivals) had
  masked this.

Harder / risks:

- The board + SoC are duplicated in the app tree and must be kept in sync
  with the DTS (the `.repl` header comment documents the mapping). If NCS
  ever ships a Renode Cortex-M3 board, migrate and delete the app-local pair.
- Renode's monitor/terminal API churns between releases (this ADR pins the
  1.17 syntax); a Renode upgrade needs a script check.
- The emulation itself is not hardware-in-loop: CDC-ACM, VBUS, and the
  nRF7002's real timing are only exercised by the physical E2E
  (`host/tests/run_e2e.sh`).
