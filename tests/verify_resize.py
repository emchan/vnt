#!/usr/bin/env python3
"""Verify that the client catches a terminal resize and re-renders at the
new (smaller) size."""
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time
import fcntl

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIENT = os.path.join(ROOT, "vnt")
SERVER = os.path.join(ROOT, "vnctestserver")


def main():
    import subprocess
    server = subprocess.Popen([SERVER, "-p", "5918", "-w", "640", "-h", "480",
                               "-f", "10"],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    try:
        pid, fd = pty.fork()
        if pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.execv(CLIENT, [CLIENT, "127.0.0.1:5918"])
            os._exit(1)

        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 384, 640))

        out = bytearray()
        # Wait for the first complete frame.
        t0 = time.time()
        while time.time() - t0 < 5 and out.count(b"\x1bPq") < 1:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                out.extend(data)

        # Shrink the terminal.
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 12, 40, 192, 320))

        # Wait for at least one more frame.
        t0 = time.time()
        while time.time() - t0 < 5 and out.count(b"\x1bPq") < 2:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                out.extend(data)

        os.kill(pid, signal.SIGKILL)
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass

        buf = bytes(out)
        sizes = re.findall(rb"\x1bPq\"1;1;(\d+);(\d+)", buf)
        print("frames:", buf.count(b"\x1bPq"), "sizes:", sorted(set(sizes)))

        small = [s for s in sizes if int(s[0]) <= 320 and int(s[1]) <= 192]
        if not small:
            print("FAIL: no frame at the smaller size")
            return 1
        print("PASS: resize detected, re-rendered at", small[0])
        return 0
    finally:
        server.terminate()
        server.wait()


if __name__ == "__main__":
    sys.exit(main())
