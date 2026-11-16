#!/usr/bin/env python3
"""
Check the sixel capability probe.

The probe uses the primary device attributes query (CSI c). A terminal with
sixel graphics answers with "4" among its parameters. There is no terminfo
capability for sixel, so this is the only portable way.
"""
import os
import pty
import select
import signal
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIENT = os.path.join(ROOT, "vnt")


def probe(reply, expect_rc):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.execv(CLIENT, [CLIENT, "--check"])
        os._exit(1)

    out = b""
    answered = False
    deadline = time.time() + 5
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                data = os.read(fd, 4096)
            except OSError:
                break
            if not data:
                break
            out += data
            # Answer the device attributes query like a sixel terminal.
            if not answered and b"\x1b[c" in out:
                os.write(fd, reply)
                answered = True
        if b"sixel:" in out:
            break

    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass

    text = out.decode("latin1")
    # The escape sequences that set up the terminal are printed first, so
    # the result is not at the start of a line.
    idx = text.find("sixel:")
    result = text[idx:].splitlines()[0] if idx >= 0 else "(no answer)"
    print("  reply %-28r -> %s" % (reply, result))
    return result


def main():
    failed = 0

    print("sixel probe:")
    r = probe(b"\x1b[?62;1;4;6;8;9;15;18;21;22c", 0)
    if "supported" not in r:
        print("  FAIL: a terminal reporting '4' must be detected as supported")
        failed += 1

    r = probe(b"\x1b[?62;1;6;8c", 1)
    if "not supported" not in r:
        print("  FAIL: a terminal without '4' must be detected as unsupported")
        failed += 1

    r = probe(b"", 2)
    if "unknown" not in r:
        print("  FAIL: a silent terminal must be reported as unknown")
        failed += 1

    if failed:
        print("\nFAIL: %d check(s) failed" % failed)
        return 1
    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
