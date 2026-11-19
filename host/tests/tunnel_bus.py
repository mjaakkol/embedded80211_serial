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


def setup_renode(mon: Monitor, console_log: str, bus_tx_log: str,
                 elf: str, repl: str) -> None:
    """Send the one-time setup commands to Renode."""
    mon.cmd('mach add "host"')
    mon.cmd(f"machine LoadPlatformDescription @{repl}")
    # Widen the PL011 RX FIFO so batched WriteChar commands do not
    # overflow the 1-byte default.
    mon.cmd("sysbus.uart1 hardwareFifoSize = 16")
    mon.cmd(f"sysbus.uart0 CreateFileBackend @{console_log} true")
    mon.cmd(f"sysbus.uart1 CreateFileBackend @{bus_tx_log} true")
    mon.cmd(f"sysbus LoadELF @{elf}", timeout=30.0)
    mon.cmd("s")


def pump_tx(tx_log: str, board_fd: int, stop: threading.Event) -> None:
    """Poll the file backend log, write new bytes to the board port."""
    last_pos = 0
    tx_debug = open("/tmp/e80211-tx-debug.log", "wb")
    total_tx = 0
    while not stop.is_set():
        try:
            with open(tx_log, "rb") as f:
                f.seek(last_pos)
                data = f.read()
                last_pos = f.tell()
            if data:
                total_tx += len(data)
                tx_debug.write(data)
                tx_debug.flush()
                burst = 0
                off = 0
                while off < len(data):
                    chunk_end = min(off + 64, len(data))
                    written = os.write(board_fd, data[off:chunk_end])
                    off += written
                    burst += 1
                    if off < len(data):
                        if burst >= 5:
                            time.sleep(0.200)
                            burst = 0
                        else:
                            time.sleep(0.050)
        except (FileNotFoundError, OSError):
            pass
        time.sleep(0.005)
    tx_debug.close()
    print(f"[tunnel] TX total: {total_tx} bytes", file=sys.stderr)


def pump_rx(board_fd: int, mon: Monitor, stop: threading.Event) -> None:
    """Read board port bytes, send as WriteChar commands to the monitor."""
    rx_debug = open("/tmp/e80211-rx-debug.log", "wb")
    total_rx = 0
    while not stop.is_set():
        try:
            data = os.read(board_fd, 256)
        except BlockingIOError:
            time.sleep(0.001)
            continue
        except OSError:
            break
        if not data:
            break
        total_rx += len(data)
        rx_debug.write(data)
        rx_debug.flush()
        # Halt CPU, batch 16 WriteChars (FIFO size), resume.
        # While halted the emulator processes commands without the
        # Zephyr scheduler starving the monitor thread.
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            mon.cmd("h", timeout=5.0)
            cmds = [f"sysbus.uart1 WriteChar {b}" for b in chunk]
            mon.cmd_batch(cmds, timeout=5.0)
            mon.cmd("c", timeout=5.0)
            time.sleep(0.05)
    rx_debug.close()
    print(f"[tunnel] RX total: {total_rx} bytes", file=sys.stderr)
    stop.set()


def main() -> None:
    monitor_port = int(sys.argv[1])
    board_port = sys.argv[2]
    bus_tx_log = sys.argv[3]
    console_log = sys.argv[4]
    elf = sys.argv[5]
    repl = sys.argv[6]

    board_fd = open_board_port(board_port)
    print(f"[tunnel] board port {board_port} fd={board_fd}", flush=True)
    mon = Monitor(monitor_port)
    print(f"[tunnel] monitor connected port={monitor_port}", flush=True)
    setup_renode(mon, console_log, bus_tx_log, elf, repl)
    print("[tunnel] Renode setup done, starting pumps", flush=True)

    stop = threading.Event()
    t_tx = threading.Thread(target=pump_tx, args=(bus_tx_log, board_fd, stop),
                            daemon=True)
    t_rx = threading.Thread(target=pump_rx, args=(board_fd, mon, stop),
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
        os.close(board_fd)


if __name__ == "__main__":
    main()
