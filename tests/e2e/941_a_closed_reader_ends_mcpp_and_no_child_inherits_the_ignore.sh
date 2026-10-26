#!/usr/bin/env bash
# requires: gcc unix-shell python3
# 941 -- a closed reader ends mcpp in order, and no program mcpp starts
# inherits SIGPIPE ignored (design 2026-10-26 part 2, §6.4 and §10.1).
#
# mcpp reaches the operating system through mcpplibs.os over openkal, and
# openkal-linux and openkal-macos ignore SIGPIPE so that their own writes
# report a closed reader instead of ending the program. An ignored disposition
# survives exec: before the guard, every program mcpp started -- a build
# script's `yes | head` included -- began with SIGPIPE ignored, and mcpp's own
# `std::print` to a closed standard output threw instead of ending mcpp.
# mcpp installs a handler for SIGPIPE before it writes anything: a handler is
# reset to the default by exec, and it ends mcpp as an interrupt does and then
# by the signal itself.
#
# Both processes are started from Python with SIGPIPE at its default
# (`restore_signals`), whatever the shell running this file was given.
#
# Criteria:
#   A. A program `mcpp run` starts begins with SIGPIPE at its default.
#   B. `mcpp --help` whose standard output has no reader ends by SIGPIPE --
#      the status a shell has always seen for `mcpp … | head` -- and not by an
#      uncaught exception, an abort, or a normal exit.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p app/src
cat > app/mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
TOML
cat > app/src/main.cpp <<'CPP'
#include <signal.h>
#include <stdio.h>
int main() {
    struct sigaction now {};
    sigaction(SIGPIPE, nullptr, &now);
    puts(now.sa_handler == SIG_IGN ? "pipe=ignored" : "pipe=default");
    return 0;
}
CPP

cd app
"$MCPP" build > build.log 2>&1 || fail "the program does not build" build.log

# A
python3 - "$MCPP" > run.log 2>&1 <<'PY' || fail "A: the program mcpp ran began with SIGPIPE ignored" run.log
import subprocess, sys
r = subprocess.run([sys.argv[1], "run"], capture_output=True, text=True, restore_signals=True)
print(r.stdout); print(r.stderr, file=sys.stderr)
sys.exit(0 if r.returncode == 0 and "pipe=default" in r.stdout else 1)
PY

# B
python3 - "$MCPP" > pipe.log 2>&1 <<'PY' || fail "B: mcpp did not end by SIGPIPE when its reader was gone" pipe.log
import os, signal, subprocess, sys
r, w = os.pipe()
os.close(r)
p = subprocess.run([sys.argv[1], "--help"], stdout=w, stderr=subprocess.PIPE, text=True,
                   restore_signals=True)
os.close(w)
print("status", p.returncode); print(p.stderr)
sys.exit(0 if p.returncode == -signal.SIGPIPE and "terminate" not in p.stderr else 1)
PY

echo "OK"
