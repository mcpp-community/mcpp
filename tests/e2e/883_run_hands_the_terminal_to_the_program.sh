#!/usr/bin/env bash
# requires: python3 unix-shell
# 883 -- `mcpp run` hands the terminal to the program: it reads the terminal's
# input, receives the terminal's Ctrl-C, and its exit status reaches the caller
# unchanged. Before 2026.10.5.1 the program ran in a background process group,
# where its first read stopped it with SIGTTIN and Ctrl-C killed it instead of
# reaching its handler.
set -euo pipefail
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
MCPP="${MCPP:-mcpp}"
REGISTRY_HOST=$(host_path "${MCPP_HOME:-$HOME/.mcpp}/registry")
export MCPP_HOME="$TMP/mcpp-home"
mkdir -p "$MCPP_HOME"
cat > "$MCPP_HOME/config.toml" <<EOF
[xlings]
home = "$REGISTRY_HOST"
EOF
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/app/src"
cd "$TMP/app"
cat > mcpp.toml <<'EOF'
[package]
name = "t883"
version = "0.1.0"
EOF
cat > src/main.cpp <<'EOF'
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

volatile std::sig_atomic_t interrupted = 0;

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "echo";
    if (mode == "echo") {
        std::string line;
        while (std::cout << "> " << std::flush, std::getline(std::cin, line)) {
            if (line == "quit") break;
            std::cout << "read: " << line << std::endl;
        }
        return 0;
    }
    if (mode == "interrupt") {
        std::signal(SIGINT, [](int) { interrupted = 1; });
        std::puts("ready");
        std::fflush(stdout);
        while (!interrupted) ::usleep(20000);
        std::puts("handled");
        std::fflush(stdout);
        return 3;
    }
    if (mode == "abort") std::abort();
    if (mode == "forever") {
        std::printf("pid %d\n", static_cast<int>(::getpid()));
        std::fflush(stdout);
        for (;;) ::pause();
    }
    return 2;
}
EOF

"$MCPP" build > build.log 2>&1 || fail "the fixture did not build" build.log

# One driver: a pseudo-terminal in front of mcpp, as a user's terminal is.
cat > "$TMP/drive.py" <<'EOF'
import os, pty, select, signal, sys, time

def run(argv, keys, settle=1.0, deadline=120.0):
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(argv[0], argv)
    out = b""
    def drain(seconds):
        nonlocal out
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    chunk = os.read(fd, 4096)
                except OSError:
                    return False
                if not chunk:
                    return False
                out += chunk
        return True
    # Wait for the program's first output before typing.
    end = time.time() + deadline
    while time.time() < end and keys and keys[0][0] not in out:
        if not drain(0.2):
            break
    for wait_for, data in keys:
        end = time.time() + deadline
        while time.time() < end and wait_for not in out:
            if not drain(0.2):
                break
        os.write(fd, data)
    end = time.time() + deadline
    status = None
    while time.time() < end:
        drain(0.2)
        p, st = os.waitpid(pid, os.WNOHANG)
        if p:
            status = st
            break
    if status is None:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        print(out.decode(errors="replace"))
        print("STATUS hung")
        return
    drain(settle)
    print(out.decode(errors="replace"))
    print("STATUS", os.waitstatus_to_exitcode(status))

mode = sys.argv[1]
argv = sys.argv[2:]
if mode == "echo":
    run(argv, [(b"> ", b"hello\r"), (b"read: hello", b"quit\r")])
elif mode == "interrupt":
    run(argv, [(b"ready", b"\x03")])
EOF

# 1. Line input on a terminal, through the cached fast path and through the
#    prepared path (a profile bypasses the fast path).
python3 "$TMP/drive.py" echo "$MCPP" run > fast.log 2>&1 || true
grep -q "read: hello" fast.log && grep -q "STATUS 0" fast.log \
    || fail "the program did not read the terminal through the fast path" fast.log
python3 "$TMP/drive.py" echo "$MCPP" run -q --release > prepared.log 2>&1 || true
grep -q "read: hello" prepared.log && grep -q "STATUS 0" prepared.log \
    || fail "the program did not read the terminal through the prepared path" prepared.log

# 2. Ctrl-C reaches the program's handler, and its status is the caller's.
python3 "$TMP/drive.py" interrupt "$MCPP" run -q -- interrupt > interrupt.log 2>&1 || true
grep -q "handled" interrupt.log && grep -q "STATUS 3" interrupt.log \
    || fail "Ctrl-C did not reach the program's handler" interrupt.log

# 3. Piped input, no terminal: unchanged.
printf 'piped\nquit\n' | "$MCPP" run -q > piped.log 2>&1 || fail "the piped run failed" piped.log
grep -q "read: piped" piped.log || fail "the program did not read piped input" piped.log

# 4. A death by signal reaches the caller as that signal, as a direct run does.
direct=$(python3 -c 'import subprocess,sys; print(subprocess.run(sys.argv[1:]).returncode)' \
    "$(find target -path '*/bin/t883' -type f | head -1)" abort 2>/dev/null)
viarun=$(python3 -c 'import subprocess,sys; print(subprocess.run(sys.argv[1:]).returncode)' \
    "$MCPP" run -q -- abort 2>/dev/null)
[[ "$direct" == "$viarun" ]] || fail "status of a signal death: direct $direct, through mcpp run $viarun"

# 5. Nothing outlives a terminated `mcpp run`: the run IS the program.
python3 - "$MCPP" > forever.log 2>&1 <<'EOF' || fail "a terminated run left a process behind" forever.log
import os, signal, subprocess, sys, time
p = subprocess.Popen([sys.argv[1], "run", "-q", "--", "forever"], stdout=subprocess.PIPE)
line = p.stdout.readline().decode()
assert line.startswith("pid "), line
child = int(line.split()[1])
assert child == p.pid, f"the program runs as pid {child}, not as the run's own pid {p.pid}"
p.send_signal(signal.SIGTERM)
p.wait(timeout=30)
time.sleep(0.2)
try:
    os.kill(child, 0)
    print("still running:", child)
    sys.exit(1)
except ProcessLookupError:
    print("terminated with the run")
EOF

echo "PASS: 883_run_hands_the_terminal_to_the_program"
