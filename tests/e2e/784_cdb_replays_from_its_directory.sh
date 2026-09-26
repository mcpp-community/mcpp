#!/usr/bin/env bash
# requires: gcc llvm
# 784_cdb_replays_from_its_directory.sh — C3 (design 2026-09-26
# .agents/docs/2026-09-26-compile-database-and-issue-699-design.md §3.3):
# `directory` is the OUTPUT directory the compiler actually runs in, for
# every unit and every toolchain -- the JSON Compilation Database format and
# S1-8-2 both define the field this way. Before the fix, `directory` named
# the project root: GCC's importers then wrote `gcm.cache/` into the PROJECT
# ROOT when replayed from there, and two of three GCC entries failed outright
# (report §3.2). Must FAIL for gcc on 2026.9.26.1.
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
mkdir -p app/src
cd app
cat > mcpp.toml <<'EOF'
[package]
name = "replaytest"
version = "0.1.0"
standard = "c++23"
EOF
cat > src/greet.cppm <<'EOF'
export module replaytest.greet;
export int answer() { return 42; }
EOF
cat > src/main.cpp <<'EOF'
import replaytest.greet;
int main() { return answer() == 42 ? 0 : 1; }
EOF

replay_all() {  # $1 = toolchain label, for messages
    local label="$1" fail=0
    python3 - "$label" <<'PY'
import json, os, shutil, subprocess, sys

label = sys.argv[1]
root = os.getcwd()
entries = json.load(open("compile_commands.json"))
fail = False
for e in entries:
    # This project's own units only: the standard-library units (D5a) share
    # one cache across every project on the machine, and replaying THAT
    # replay is 786's criterion, not this one's (C3 is about the project's
    # own directory).
    if os.path.commonpath([os.path.abspath(e["file"]), root]) != root:
        continue
    obj = e["output"]
    if os.path.exists(obj):
        os.remove(obj)
    rc = subprocess.run(e["arguments"], cwd=e["directory"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    ok = rc.returncode == 0 and os.path.exists(obj)
    print(f"  [{label}] {'ok' if ok else 'FAIL'}: {os.path.basename(e['file'])} "
          f"from directory={e['directory']}")
    if not ok:
        sys.stderr.write(rc.stderr.decode(errors="replace"))
        fail = True
sys.exit(1 if fail else 0)
PY
}

for tc in gcc@16.1.0 llvm@22.1.8; do
    "$MCPP" build --toolchain "$tc" --no-cache > "build-$tc.log" 2>&1 || {
        cat "build-$tc.log"; echo "FAIL: build with $tc failed"; exit 1; }
    replay_all "$tc" || { echo "FAIL: replay failed for $tc"; exit 1; }
    # The project root must never gain a BMI cache directory: every unit's
    # `directory` already IS the output directory, so a compiler that writes
    # its cache relative to cwd (GCC's gcm.cache) writes it there, not here.
    if [[ -d gcm.cache || -d pcm.cache ]]; then
        echo "FAIL: a BMI cache directory leaked into the project root under $tc"
        find . -maxdepth 1 -iname 'gcm.cache' -o -iname 'pcm.cache'
        exit 1
    fi
done

echo "ok: gcc row replays every entry from its directory, no gcm.cache/ in the project root"
echo "ok: llvm row replays every entry from its directory"
echo "PASS: 784 the compile database replays from its own directory"
