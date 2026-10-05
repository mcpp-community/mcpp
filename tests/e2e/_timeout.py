#!/usr/bin/env python3
"""The per-test bound of run_all.sh where neither GNU `timeout` nor `gtimeout`
exists (the macOS runners).

Usage: _timeout.py SECONDS COMMAND [ARG...]

The command runs in a session of its own, so the bound ends the whole tree a
test started (mcpp, ninja, compilers), not only its shell. At the deadline the
group receives SIGTERM, and SIGKILL ten seconds later. The exit status follows
GNU timeout: 124 when the deadline was reached, the command's own status
otherwise.
"""
import os
import signal
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: _timeout.py SECONDS COMMAND [ARG...]", file=sys.stderr)
        return 125
    seconds = float(sys.argv[1])
    child = subprocess.Popen(sys.argv[2:], start_new_session=True)
    try:
        return child.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        pass
    for sig, grace in ((signal.SIGTERM, 10), (signal.SIGKILL, None)):
        try:
            os.killpg(child.pid, sig)
        except ProcessLookupError:
            break
        try:
            child.wait(timeout=grace)
            break
        except subprocess.TimeoutExpired:
            continue
    return 124


if __name__ == "__main__":
    sys.exit(main())
