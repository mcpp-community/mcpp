#!/usr/bin/env bash
# requires: python3 unix-shell
# 845_play_game_restores_the_terminal.sh -- build output design 2026-09-30
# (revision 3), §5.14: `--play-game`, through a pseudo-terminal.
#
# A package whose prepare action takes four seconds, built with
# `--play-game=snake`:
#
#   G1  while the game plays, the terminal reads keys without echo and
#       without a line end, and the status row carries the game's score;
#   G2  after the build the terminal's mode is the mode it had, and a line
#       after `Finished` states the best round;
#   G3  a Ctrl-C during the game ends mcpp by SIGINT and still restores the
#       terminal's mode, and the prepare action does not outlive mcpp;
#   G4  with standard input not a terminal the game is off, a line says why,
#       and the build succeeds.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

mkdir -p app/src
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > app/mcpp.toml
printf 'int main() { return 0; }\n' > app/src/main.cpp
cat > app/build.mcpp <<'EOF'
import mcpp;
#include <string>
int main() {
    const std::string prefix = std::string(mcpp::out_dir()) + "/tool";
    mcpp::action a;
    a.id   = "app:install";
    a.role = mcpp::roles::prepare;
    a.arg("python3").arg("-c")
     .arg("import os, sys, time; time.sleep(4); os.makedirs(sys.argv[1], exist_ok=True); open(os.path.join(sys.argv[1], \"ready\"), \"w\").close()")
     .arg(prefix.c_str())
     .output((prefix + ".stamp").c_str())
     .output_dir(prefix.c_str())
     .submit();
    return 0;
}
EOF
cd app

cat > play.py <<'PY'
import fcntl, os, re, select, struct, subprocess, sys, termios, time
mcpp, mode = sys.argv[1], sys.argv[2]
master, slave = os.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
env = dict(os.environ, TERM="xterm-256color", LANG="C.UTF-8")
env.pop("LC_ALL", None)
env.pop("NO_COLOR", None)
# The terminal's mode before and after is recorded by a shell in the same
# session, while the terminal is still its controlling terminal: on macOS the
# slave side is revoked for everyone else once the session leader exits. The
# shell catches SIGINT (a trap, not an ignore, so mcpp starts with the default
# action) and records the mode after mcpp has gone.
for f in ("before.txt", "after.txt"):
    if os.path.exists(f):
        os.remove(f)
pid = os.fork()
if pid == 0:
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    for fd in (0, 1, 2):
        os.dup2(slave, fd)
    script = ('trap : INT; stty -g > before.txt; "$0" build --play-game=snake; '
              'code=$?; stty -g > after.txt; exit $code')
    os.execvpe("sh", ["sh", "-c", script, mcpp], env)
raw, t0, during, sent, status = b"", time.time(), None, False, 0
while True:
    r, _, _ = select.select([master], [], [], 0.1)
    if r:
        try:
            chunk = os.read(master, 65536)
        except OSError:
            chunk = b""
        raw += chunk
    if time.time() - t0 > 1.5 and not sent:
        during = termios.tcgetattr(slave)
        for key in (b"\x1b[B", b"\x1b[C", b"\x1b[A"):
            os.write(master, key)
            time.sleep(0.15)
        if mode == "ctrlc":
            os.write(master, b"\x03")
        sent = True
    done, status = os.waitpid(pid, os.WNOHANG)
    if done:
        while select.select([master], [], [], 0.2)[0]:
            try:
                chunk = os.read(master, 65536)
            except OSError:
                break
            if not chunk:
                break
            raw += chunk
        break
before = open("before.txt").read().strip() if os.path.exists("before.txt") else None
after = open("after.txt").read().strip() if os.path.exists("after.txt") else None
plain = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", raw.decode("utf-8", "replace")).replace("\r\n", "\n")
print(plain)
assert during is not None, "the build ended before the game could be observed"
# G1
assert not (during[3] & termios.ECHO), "G1: echo stayed on during the game"
assert not (during[3] & termios.ICANON), "G1: the terminal still waited for a line end"
assert re.search(r"Building .* · snake \d+", plain), "G1: no status row carries the score"
# G2 / G3
assert before and after == before, f"G2/G3: the terminal's mode was not restored: {before!r} -> {after!r}"
code = os.WEXITSTATUS(status) if os.WIFEXITED(status) else -1
if mode == "ctrlc":
    assert code == 130, f"G3: mcpp did not end by SIGINT (the shell reports {code})"
    time.sleep(0.5)
    left = subprocess.run(["pgrep", "-f", "app:install|time.sleep\\(4\\)"], capture_output=True, text=True).stdout
    assert os.path.basename(os.getcwd()) not in left or not left.strip(), f"G3: the prepare action outlived mcpp: {left}"
else:
    assert code == 0, f"G2: the build failed ({code})"
    assert re.search(r"^ +Played snake · best \d+$", plain, re.M), "G2: no line states the best round"
    fin = [i for i, l in enumerate(plain.splitlines()) if "Finished" in l]
    pla = [i for i, l in enumerate(plain.splitlines()) if "Played snake" in l]
    assert fin and pla and pla[0] > fin[0], "G2: the best round is not stated after Finished"
PY

python3 play.py "$MCPP" normal > g2.log 2>&1 || fail "G1/G2" g2.log
rm -rf target
python3 play.py "$MCPP" ctrlc > g3.log 2>&1 || fail "G3" g3.log

# G4
rm -rf target
"$MCPP" build --play-game < /dev/null > g4.log 2>&1 || fail "G4: the build failed" g4.log
grep -q "play-game" g4.log || true   # not a terminal on stdout either: the log medium says nothing
python3 - "$MCPP" <<'PY' > g4b.log 2>&1 || fail "G4" g4b.log
import os, pty, sys, re
# stdout a terminal, stdin not: the game is off and one line says why.
pid, fd = pty.fork()
if pid == 0:
    devnull = os.open("/dev/null", os.O_RDONLY)
    os.dup2(devnull, 0)
    os.environ["LANG"] = "C.UTF-8"
    os.environ.pop("LC_ALL", None)
    os.execvp(sys.argv[1], [sys.argv[1], "build", "--play-game"])
raw = b""
while True:
    try:
        chunk = os.read(fd, 65536)
    except OSError:
        break
    if not chunk:
        break
    raw += chunk
_, status = os.waitpid(pid, 0)
plain = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", raw.decode("utf-8", "replace"))
print(plain)
assert os.WEXITSTATUS(status) == 0, "G4: the build failed"
assert "standard input is not a terminal" in plain, "G4: no line says why the game is off"
assert "Played" not in plain, "G4: a game was played without keys"
PY

echo "PASS: 845_play_game_restores_the_terminal"
