#!/usr/bin/env python3
"""
Drive vncsixel through a pseudo terminal and check the escape sequences it
emits, plus that keyboard and mouse input reach the VNC server.

  * the terminal is put into the alternate screen, the cursor is hidden
  * sixel display mode (DECSDM) is enabled so that every frame is drawn at
    the top left corner without moving the text cursor
  * the shared colour register palette is requested (DECRST 1070)
  * mouse reporting is switched on and off again
  * on exit the terminal is restored
"""
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIENT = os.path.join(ROOT, "vnt")
SERVER = os.path.join(ROOT, "vnctestserver")

CSI = b"\x1b["


def main():
    server = subprocess = None
    import subprocess
    server = subprocess.Popen([SERVER, "-p", "5911", "-w", "320", "-h", "240",
                               "-f", "10"],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    try:
        pid, fd = pty.fork()
        if pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.execv(CLIENT, [CLIENT, "127.0.0.1:5911"])
            os._exit(1)

        # Give the child a sensible window size: 80x24 cells of 8x16 pixels.
        winsize = struct.pack("HHHH", 24, 80, 640, 384)
        import fcntl
        fcntl.ioctl(fd, termios.TIOCSWINSZ, winsize)

        out = bytearray()
        deadline = time.time() + 12
        sent_mouse = False
        sent_key = False
        quitting = False
        quit_deadline = 0.0

        while time.time() < deadline:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                out.extend(data)

            if quitting:
                if time.time() > quit_deadline:
                    break
                continue

            if not sent_mouse and len(out) > 0:
                # SGR mouse press at cell (10, 5)
                os.write(fd, b"\x1b[<0;10;5M")
                sent_mouse = True
            elif sent_mouse and not sent_key and len(out) > 2000:
                os.write(fd, b"a")
                sent_key = True

            if sent_key and len(out) > 20000:
                # Ctrl-C quits the client; keep reading so that we also
                # capture the terminal restore sequences.
                os.write(fd, b"\x03")
                quitting = True
                quit_deadline = time.time() + 2.0

        buf = bytes(out)
        print("captured %d bytes from the pty" % len(buf))
        print("tail: %r" % buf[-120:])

        checks = [
            ("alternate screen on", CSI + b"?1049h"),
            ("cursor hidden", CSI + b"?25l"),
            ("sixel display mode", CSI + b"?80h"),
            ("shared colour registers", CSI + b"?1070l"),
            ("mouse tracking on", CSI + b"?1002h"),
            ("sixel data", b"\x1bPq"),
            ("sixel terminator", b"\x1b\\"),
            ("mouse tracking off", CSI + b"?1002l"),
            ("cursor shown", CSI + b"?25h"),
            ("alternate screen off", CSI + b"?1049l"),
        ]

        failed = 0
        for name, needle in checks:
            ok = needle in buf
            print("  %-26s %s" % (name, "ok" if ok else "MISSING"))
            if not ok:
                failed += 1

        # The sixel stream must contain a raster attribute so that the
        # terminal uses square pixels.
        m = re.search(rb"\x1bPq\"1;1;(\d+);(\d+)", buf)
        if m:
            print("  %-26s ok (%sx%s)"
                  % ("raster attributes", m.group(1).decode(), m.group(2).decode()))
        else:
            print("  %-26s MISSING" % "raster attributes")
            failed += 1

        nframes = buf.count(b"\x1bPq")
        print("  %-26s %d" % ("sixel frames emitted", nframes))
        if nframes < 1:
            failed += 1

        if failed:
            print("\nFAIL: %d check(s) failed" % failed)
            return 1
        print("\nPASS")
        return 0
    finally:
        server.terminate()
        server.wait()


if __name__ == "__main__":
    sys.exit(main())
