#!/usr/bin/env bash
# requires: llvm python3
# 960 -- an ok image of another architecture runs through its execution host
# (SPEC-013 §10; design 2026-10-29).
#
# openkal-platform's runner is `{ cmd = ["okrun", "{}"], host = "same-arch" }`:
# it runs an image on a machine of the image's architecture. On this machine
# an image of the other architecture is refused, naming `run_under`. With an
# execution host named, the runner runs there, so its program is the
# platform's runner for the image's architecture -- the carrier with nothing
# carried, built for this system -- and a user-mode emulator
# (`run_under = ["qemu-aarch64"]`) runs an aarch64 image on x86_64. Measured on
# 2026.10.28.1: qemu was handed the x86_64 okrun ("Invalid ELF image for this
# architecture").
#
# Criteria:
#   A. `mcpp run`, `mcpp test --target <this-arch>-openkal`, `mcpp run`: the
#      third prints no "fingerprint changed": the build of the package's
#      program before its tests is recorded under its target, not as this
#      machine's.
#   B. `mcpp test --target <other>-openkal` exits 2 and prints the refusal in
#      full, its `run_under` line included, exactly once.
#   C. `mcpp self env --target <other>-openkal` says runs are refused here
#      (`runner.refusal` in JSON); with an execution host named, the runner's
#      program is the other architecture's and the execution host's program
#      is reported.
#   D. An execution host that records what it is handed receives, from
#      `mcpp run` and `mcpp test`, the platform's runner of the other
#      architecture (an ELF of that machine); where this machine has that
#      architecture's qemu, the image runs and the test passes through it.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
TMP_HOST="$(host_path "$TMP")"

case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=x86_64;  OTHER=aarch64 ;;
    aarch64|arm64) HOST_ARCH=aarch64; OTHER=x86_64 ;;
    *) echo "SKIP: no ok-image row for $(uname -m)"; exit 0 ;;
esac
HT="$HOST_ARCH-openkal"
OT="$OTHER-openkal"
RUN_UNDER_KEY="MCPP_TARGET_$(printf '%s' "$OT" | tr 'a-z-' 'A-Z_')_RUN_UNDER"

mkdir -p "$TMP/app/src" "$TMP/app/tests"
cd "$TMP/app"
printf '[package]\nname    = "app"\nversion = "0.1.0"\n' > mcpp.toml
cat > src/main.cpp <<'CPP'
import std;
int main(int argc, char** argv) {
    try { throw std::runtime_error("caught"); }
    catch (const std::exception& e) { std::println("image: {} ({} args)", e.what(), argc); }
    return 0;
}
CPP
cat > tests/smoke.cpp <<'CPP'
import std;
int main() { std::vector<int> v{2, 1}; std::ranges::sort(v); return v.front() == 1 ? 0 : 1; }
CPP

# The architecture of an ELF file (e_machine).
elf_arch() {
    python3 - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read(20)
assert d[:4] == b"\x7fELF", "not an ELF file"
m = struct.unpack("<H", d[18:20])[0]
print({62: "x86_64", 183: "aarch64"}.get(m, str(m)))
PY
}

# A
"$MCPP" run > a1.log 2>&1 || fail "A: mcpp run" a1.log
"$MCPP" test --target "$HT" > a2.log 2>&1 || fail "A: mcpp test --target $HT" a2.log
"$MCPP" run > a3.log 2>&1 || fail "A: mcpp run after the test" a3.log
grep -q "fingerprint changed" a3.log && fail "A: the test's build was recorded as this machine's" a3.log
grep -qx "\[target=$HT\]" target/.build_cache || fail "A: no cache entry for $HT" target/.build_cache
echo "ok: the build before a target's tests is recorded under the target"

# B
rc=0
"$MCPP" test --target "$OT" > b.log 2>&1 || rc=$?
[ "$rc" = 2 ] || fail "B: mcpp test --target $OT exited $rc, not 2" b.log
grep -q "smoke ... not run" b.log || fail "B: the test is not reported not run" b.log
[ "$(grep -c "run_under = " b.log)" = 1 ] || fail "B: the run_under line is not printed exactly once" b.log
grep -q "cannot run $OT artifacts on this $HOST_ARCH host" b.log || fail "B: the reason" b.log
echo "ok: the refusal is printed in full, once"

# C
"$MCPP" self env --target "$OT" > c.log 2>&1 || fail "C: self env" c.log
grep -q "refused on this machine: cannot run $OT artifacts" c.log || fail "C: self env does not say runs are refused" c.log
"$MCPP" self env --target "$OT" --format json > c.json 2> c.err || fail "C: self env --format json" c.err c.json
python3 - c.json "$HOST_ARCH" <<'PY' || fail "C: the refusal in JSON" c.json
import json, sys
r = json.load(open(sys.argv[1]))["data"]["runner"]
assert r["refusal"] and "run_under" in r["refusal"], r
assert r["arch"] == sys.argv[2], r
PY
printf '#!/bin/sh\nprintf "%%s\\n" "$@" > "$VIA_LOG"\n[ -n "$VIA_QEMU" ] && exec "$VIA_QEMU" "$@"\nexit 0\n' > "$TMP/via"
chmod +x "$TMP/via"
env "$RUN_UNDER_KEY=$TMP_HOST/via" "$MCPP" self env --target "$OT" --format json > c2.json 2> c2.err \
    || fail "C: self env with an execution host" c2.err c2.json
python3 - c2.json "$OTHER" "$TMP/via" <<'PY' || fail "C: self env with an execution host" c2.json
import json, os, sys
d = json.load(open(sys.argv[1]))["data"]
r = d["runner"]
assert r["refusal"] is None, r
assert r["arch"] == sys.argv[2], r
assert "/carrier-pack/okrun-" in r["program"].replace("\\", "/") and r["program"].replace("\\", "/").endswith(sys.argv[2] + "/target/dist/packed/bin/okrun"), r
assert os.path.samefile(d["runUnderProgram"], sys.argv[3]), d["runUnderProgram"]
PY
echo "ok: self env states the refusal, and the other architecture's runner for an execution host"

# D
printf '\n[target.%s]\nrun_under = ["%s/via"]\n' "$OT" "$TMP_HOST" >> mcpp.toml
VIA_LOG="$TMP/via.run" "$MCPP" run --target "$OT" -- one > d1.log 2>&1 || fail "D: mcpp run through the execution host" d1.log
grep -q "okrun for linux-$OTHER" d1.log || grep -q "okrun for macos-$OTHER" d1.log \
    || fail "D: the other architecture's runner was not built" d1.log
runner=$(head -1 "$TMP/via.run")
[ -f "$runner" ] || fail "D: the execution host was handed no program" "$TMP/via.run"
[ "$(elf_arch "$runner")" = "$OTHER" ] || fail "D: the runner handed over is $(elf_arch "$runner"), not $OTHER" "$TMP/via.run"
grep -q "/bin/app.oke$" "$TMP/via.run" || fail "D: the image is not the runner's argument" "$TMP/via.run"
VIA_LOG="$TMP/via.test" "$MCPP" test --target "$OT" > d2.log 2>&1 || fail "D: mcpp test through the execution host" d2.log
grep -q "smoke ... ok" d2.log || fail "D: the test did not pass through the execution host" d2.log
[ "$(head -1 "$TMP/via.test")" = "$runner" ] || fail "D: the test's runner is not the run's" "$TMP/via.test"
echo "ok: the execution host runs the image with the runner of its architecture"

QEMU=$(command -v "qemu-$OTHER" || command -v "qemu-$OTHER-static" || true)
if [ -z "$QEMU" ]; then
    echo "  (no qemu-$OTHER on this machine: the image is not executed)"
else
    VIA_LOG="$TMP/via.q" VIA_QEMU="$QEMU" "$MCPP" run --target "$OT" -- one > d3.log 2>&1 \
        || fail "D: mcpp run under $QEMU" d3.log
    grep -q "image: caught (2 args)" d3.log || fail "D: the image's output under $QEMU" d3.log
    VIA_LOG="$TMP/via.qt" VIA_QEMU="$QEMU" "$MCPP" test --target "$OT" > d4.log 2>&1 \
        || fail "D: mcpp test under $QEMU" d4.log
    grep -q "smoke ... ok" d4.log || fail "D: the test under $QEMU" d4.log
    echo "ok: $QEMU runs the $OTHER image and its test"
fi

echo "PASS: 960 an image of another architecture runs through its execution host"
