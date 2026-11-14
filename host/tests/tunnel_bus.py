#!/usr/bin/env python3
"""Bridge Renode's bus UART socket to the board's serial port.

Connects to the Renode socket terminal (127.0.0.1:PORT) and pumps bytes
bidirectionally to the board bus port (the nRF7002 DK's USB CDC-ACM
protocol port). Replaces the socat pty bridge used by the old QEMU setup.

Exits non-zero if the Renode socket does not come up within the connect
window, or if the board port cannot be opened.

Usage: tunnel_bus.py PORT BOARD_PORT
"""
import os
import socket
import sys
import termios
import threading
import time
import tty

CONNECT_WINDOW_S = 15.0


def open_board_port(port: str) -> int:
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[3] &= ~0x100  # HUPCL: closing must not toggle DTR
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def connect_renode(port: int) -> socket.socket:
    deadline = time.time() + CONNECT_WINDOW_S
    while time.time() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=2)
        except OSError:
            time.sleep(0.5)
    raise SystemExit(f"Renode bus socket 127.0.0.1:{port} not reachable")


def pump(rfd: int, wfd: int, stop: threading.Event) -> None:
    """Read rfd, write to wfd, until EOF or error; set stop on exit."""
    try:
        while not stop.is_set():
            try:
                data = os.read(rfd, 4096)
            except BlockingIOError:
                continue
            except OSError:
                break
            if not data:
                break
            try:
                os.write(wfd, data)
            except OSError:
                break
    finally:
        stop.set()


def main() -> None:
    port = int(sys.argv[1])
    board_port = sys.argv[2]

    fd = open_board_port(board_port)
    sock = connect_renode(port)
    sfd = sock.fileno()

    stop = threading.Event()
    t1 = threading.Thread(target=pump, args=(sfd, fd, stop), daemon=True)
    t2 = threading.Thread(target=pump, args=(fd, sfd, stop), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()

    os.close(fd)
    sock.close()


if __name__ == "__main__":
    main()
