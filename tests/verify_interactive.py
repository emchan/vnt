#!/usr/bin/env python3
"""
Check the interactive side of vncsixel:

  * consecutive frames differ (the remote desktop is animated, so the client
    has to keep redrawing)
  * keyboard input reaches the VNC server
  * mouse input reaches the VNC server, mapped into remote framebuffer
    coordinates
"""
import os
import pty
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

REMOTE_W, REMOTE_H = 640, 480


def main():
    import subprocess
    server_log = open(os.path.join(os.path.dirname(__file__), "..",
                                  "it_server.log"), "w+b")
    server = subprocess.Popen([SERVER, "-p", "5912", "-w", str(REMOTE_W),
                               "-h", str(REMOTE_H), "-f", "10"],
                              stdout=server_log, stderr=server_log)
    time.sleep(1.0)
    try:
        pid, fd = pty.fork()
        if pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.execv(CLIENT, [CLIENT, "127.0.0.1:5912"])
            os._exit(1)

        winsize = struct.pack("HHHH", 24, 80, 640, 384)
        fcntl.ioctl(fd, termios.TIOCSWINSZ, winsize)

        out = bytearray()
        frames = []
        sent_mouse = sent_key = sent_backspace = False
        deadline = time.time() + 12
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
                # Split the stream into sixel frames.
                while True:
                    start = out.find(b"\x1bPq")
                    if start < 0:
                        break
                    end = out.find(b"\x1b\\", start)
                    if end < 0:
                        break
                    frames.append(bytes(out[start:end + 2]))
                    del out[:end + 2]

            if quitting:
                if time.time() > quit_deadline:
                    break
                continue

            if not sent_mouse and len(frames) >= 1:
                # SGR mouse press at cell (10, 5) -> remote (39, 19)
                os.write(fd, b"\x1b[<0;10;5M")
                sent_mouse = True
            elif sent_mouse and not sent_key and len(frames) >= 2:
                os.write(fd, b"a")
                sent_key = True
            elif sent_key and not sent_backspace and len(frames) >= 3:
                # Terminals send DEL for Backspace; it must reach the server
                # as the X11 keysym XK_BackSpace.
                os.write(fd, b"\x7f")
                sent_backspace = True
            elif sent_backspace and len(frames) >= 4:
                os.write(fd, b"\x03")
                quitting = True
                quit_deadline = time.time() + 2.0

        time.sleep(0.3)
        server_log.flush()
        server_log.seek(0)
        slog = server_log.read().decode("utf-8", "replace")

        print("sixel frames received: %d" % len(frames))

        failed = 0

        if len(frames) < 3:
            print("  FAIL: expected several frames")
            failed += 1
        else:
            # The remote desktop animates, so the frames must differ.
            distinct = len(set(frames))
            print("  %-34s %d/%d distinct" % ("frames differ", distinct,
                                               len(frames)))
            if distinct < 2:
                print("  FAIL: every frame is identical - no live updates")
                failed += 1

        if "key down keysym=0x61" in slog:
            print("  %-34s ok" % "keyboard event forwarded")
        else:
            print("  %-34s MISSING" % "keyboard event forwarded")
            print("    server log: %r" % slog[-400:])
            failed += 1

        if "key down keysym=0xff08" in slog:
            print("  %-34s ok" % "backspace mapped to XK_BackSpace")
        else:
            print("  %-34s MISSING" % "backspace mapped to XK_BackSpace")
            print("    server log: %r" % slog[-400:])
            failed += 1

        if "pointer mask=" in slog:
            line = [l for l in slog.splitlines() if "pointer mask=" in l][-1]
            print("  %-34s ok (%s)" % ("mouse event forwarded", line.strip()))
            # The terminal is 80x24 cells of 8x16 pixels, so the drawable
            # area is 640x384. The remote 640x480 desktop is scaled down to
            # 512x384. Cell (10,5) maps to the centre of the cell:
            # image pixel (72+4, 64+8) = (76,72), which maps back to
            # remote pixel (76*640/512, 72*480/384) = (95, 90).
            m = line.split("at (")[1].rstrip(")")
            x, y = (int(v) for v in m.split(","))
            if (x, y) != (95, 90):
                print("  FAIL: pointer at (%d,%d), expected (95,90)" % (x, y))
                failed += 1
        else:
            print("  %-34s MISSING" % "mouse event forwarded")
            print("    server log: %r" % slog[-400:])
            failed += 1

        if failed:
            print("\nFAIL: %d check(s) failed" % failed)
            return 1
        print("\nPASS")
        return 0
    finally:
        server.terminate()
        server.wait()
        server_log.close()


if __name__ == "__main__":
    sys.exit(main())
