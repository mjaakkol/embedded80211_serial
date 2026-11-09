#!/usr/bin/env python3
"""Find the port carrying the common link protocol bus.

On the nRF7002 DK the bus is the target USB connector's CDC-ACM protocol
port (CDC_ACM_0). Probe each /dev/cu.usbmodem* port with a PROTOCOL_INFO
query and print the one that answers (the J-Link virtual ports and the
unbound CDC-ACM console port do not answer).

PROTOCOL_INFO query (no_ack=1, host=0, type=7, seq=0, len=0): 17 00 00 00
Expected answer header: 27 10 00 04 (type=7, host=1, len=4).

Usage: find_bus_port.py [timeout_s]
Prints the port path on success, exits 1 if none responds.
"""
import glob
import os
import sys
import termios
import tty

QUERY = bytes([0x17, 0x00, 0x00, 0x00])


def probe(port: str, timeout: float) -> bool:
    try:
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    except OSError:
        return False
    try:
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[3] &= ~0x100  # HUPCL: closing must not toggle DTR
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        os.set_blocking(fd, False)

        # Drain pending data.
        import time
        while True:
            try:
                if not os.read(fd, 4096):
                    break
            except BlockingIOError:
                break

        os.write(fd, QUERY)

        buf = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                d = os.read(fd, 256)
            except BlockingIOError:
                time.sleep(0.02)
                continue
            if not d:
                break
            buf += d
            if len(buf) >= 8:
                break
        # type=7 (0x0F), host bit set (0x20), length bytes at [2:4]
        return len(buf) >= 4 and buf[0] == 0x27 and buf[2] == 0x00 and buf[3] == 0x04
    finally:
        os.close(fd)


def main() -> None:
    timeout = float(sys.argv[1]) if len(sys.argv) > 1 else 0.5
    for port in sorted(glob.glob("/dev/cu.usbmodem*")):
        if probe(port, timeout):
            print(port)
            return
    sys.exit(1)


if __name__ == "__main__":
    main()
