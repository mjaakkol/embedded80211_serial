#!/usr/bin/env python3
"""Bridge Renode's bus UART to the board's serial port.

Uses the Renode monitor port (-P) for RX (WriteChar commands) and a
file backend log for TX (polling the file the PL011 writes to).

The monitor port is a telnet-style TCP connection. Setup commands
(mach add, LoadPlatformDescription, CreateFileBackend, LoadELF, s)
are sent once at startup. After that, the RX pump sends WriteChar
commands and the TX pump reads the file backend log.

Usage: tunnel_bus.py MONITOR_PORT BOARD_PORT BUS_TX_LOG CONSOLE_LOG ELF REPL
"""
import os
import select
import socket
import sys
import termios
import threading
import time
import tty

CONNECT_WINDOW_S = 15.0
PROMPT_TIMEOUT_S = 10.0


class Monitor:
    """Renode monitor port connection (telnet-style)."""

    def __init__(self, port: int):
        self.sock = self._connect(port)
        self.buf = b""
        self._drain_banner()

    @staticmethod
    def _connect(port: int) -> socket.socket:
        deadline = time.time() + CONNECT_WINDOW_S
        while time.time() < deadline:
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=3)
                s.settimeout(PROMPT_TIMEOUT_S)
                return s
            except OSError:
                time.sleep(0.5)
        raise SystemExit(f"Renode monitor 127.0.0.1:{port} not reachable")

    def _drain_banner(self) -> None:
        """Read the telnet banner and initial prompt."""
        deadline = time.time() + 5.0
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return
                self.buf += chunk
                if b"(monitor) " in self.buf or b"(host) " in self.buf:
                    return
            except socket.timeout:
                return

    def _read_until_prompt(self, timeout: float = PROMPT_TIMEOUT_S) -> None:
        """Read until the next prompt; keep any trailing data in buf."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for marker in (b"(host) ", b"(monitor) "):
                idx = self.buf.find(marker)
                if idx != -1:
                    self.buf = self.buf[idx + len(marker):]
                    return
            remaining = deadline - time.time()
            if remaining <= 0:
                return
            ready, _, _ = select.select([self.sock], [], [],
                                        min(remaining, 0.1))
            if ready:
                try:
                    chunk = self.sock.recv(4096)
                    if not chunk:
                        return
                    self.buf += chunk
                except socket.timeout:
                    pass

    def cmd(self, command: str, timeout: float = PROMPT_TIMEOUT_S) -> None:
        """Send a monitor command and wait for the prompt."""
        self.sock.send(f"{command}\n".encode())
        self._read_until_prompt(timeout)

    def cmd_batch(self, commands: list[str], timeout: float = PROMPT_TIMEOUT_S) -> None:
        """Send multiple commands; wait for one prompt per command."""
        data = "".join(f"{c}\n" for c in commands)
        self.sock.send(data.encode())
        for _ in commands:
            self._read_until_prompt(timeout)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def open_board_port(port: str) -> int:
    # The port may have just been recreated by a reset; USB re-enumeration
    # can take tens of seconds, and udev applies the dialout group a moment
    # after node creation. Retry for a full minute.
    deadline = time.monotonic() + 60.0
    while True:
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            break
        except OSError as e:
            if time.monotonic() >= deadline:
                raise SystemExit(f"Cannot open board port {port}: {e}")
            time.sleep(0.5)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[3] &= ~0x100  # HUPCL: closing must not toggle DTR
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


BUS_PTY_PATH = "/tmp/renode_bus_pty"


def setup_renode(mon: Monitor, console_log: str,
                 elf: str, repl: str) -> None:
    """Send the one-time setup commands to Renode."""
    mon.cmd('mach add "host"')
    mon.cmd(f"machine LoadPlatformDescription @{repl}")
    mon.cmd(f"sysbus.uart0 CreateFileBackend @{console_log} true")
    # Bus UART: PTY terminal for high-throughput bidirectional data.
    mon.cmd(f'emulation CreateUartPtyTerminal "buspty" "{BUS_PTY_PATH}"')
    mon.cmd("connector Connect sysbus.uart1 buspty")
    mon.cmd(f"sysbus LoadELF @{elf}", timeout=30.0)
    mon.cmd("s")


def pump_tx(pty_fd: int, board_fd: int, stop: threading.Event) -> None:
    """Read host→device bytes from the PTY, write to the board port."""
    tx_debug = open("/tmp/e80211-tx-debug.log", "wb")
    total_tx = 0
    while not stop.is_set():
        r, _, _ = select.select([pty_fd], [], [], 0.05)
        if not r:
            continue
        try:
            data = os.read(pty_fd, 4096)
        except (BlockingIOError, OSError):
            continue
        if not data:
            break
        total_tx += len(data)
        tx_debug.write(data)
        tx_debug.flush()
        off = 0
        while off < len(data):
            try:
                written = os.write(board_fd, data[off:])
                off += written
            except BlockingIOError:
                # Board port TX buffer full; wait until writable.
                _, w, _ = select.select([], [board_fd], [], 1.0)
                if not w:
                    break
    tx_debug.close()
    print(f"[tunnel] TX total: {total_tx} bytes", file=sys.stderr)
    stop.set()


def pump_rx(board_fd: int, pty_fd: int, stop: threading.Event) -> None:
    """Read board port bytes, write to the PTY (→ Renode PL011)."""
    rx_debug = open("/tmp/e80211-rx-debug.log", "wb")
    total_rx = 0
    while not stop.is_set():
        r, _, _ = select.select([board_fd], [], [], 0.05)
        if not r:
            continue
        chunks = []
        while True:
            try:
                data = os.read(board_fd, 4096)
            except BlockingIOError:
                break
            except OSError:
                rx_debug.close()
                stop.set()
                return
            if not data:
                break
            chunks.append(data)
        if not chunks:
            continue
        data = b"".join(chunks)
        total_rx += len(data)
        rx_debug.write(data)
        rx_debug.flush()
        # Write directly to the PTY — no per-byte monitor round-trip.
        off = 0
        while off < len(data):
            written = os.write(pty_fd, data[off:])
            off += written
    rx_debug.close()
    print(f"[tunnel] RX total: {total_rx} bytes", file=sys.stderr)
    stop.set()


def main() -> None:
    monitor_port = int(sys.argv[1])
    board_port = sys.argv[2]
    console_log = sys.argv[3]
    elf = sys.argv[4]
    repl = sys.argv[5]

    board_fd = open_board_port(board_port)
    print(f"[tunnel] board port {board_port} fd={board_fd}", flush=True)
    mon = Monitor(monitor_port)
    print(f"[tunnel] monitor connected port={monitor_port}", flush=True)
    setup_renode(mon, console_log, elf, repl)
    print("[tunnel] Renode setup done, opening PTY", flush=True)

    # Open the PTY that Renode created for the bus UART.
    pty_fd = os.open(BUS_PTY_PATH, os.O_RDWR | os.O_NONBLOCK)
    print(f"[tunnel] bus PTY fd={pty_fd}", flush=True)

    stop = threading.Event()
    t_tx = threading.Thread(target=pump_tx, args=(pty_fd, board_fd, stop),
                            daemon=True)
    t_rx = threading.Thread(target=pump_rx, args=(board_fd, pty_fd, stop),
                            daemon=True)
    t_tx.start()
    t_rx.start()

    try:
        while not stop.is_set():
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        t_tx.join(timeout=2)
        t_rx.join(timeout=2)
        mon.close()
        os.close(pty_fd)
        os.close(board_fd)


if __name__ == "__main__":
    main()
