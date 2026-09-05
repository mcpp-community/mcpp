#!/usr/bin/env bash
# requires: gcc unix-shell python3
# The three axes a project is asked about before it is adopted: how the
# artefact reaches a device, whether the build is reproducible, and what went
# into it.
#
# NONE OF THESE NEEDS A DEVICE, AND THAT IS DELIBERATE. `flash`, `monitor`
# and `debug` perform an argv the board supplied; what this script asserts is
# that mcpp resolves the right slot, reports an override, and refuses clearly
# when nothing is declared. Standing in a shell script for the tool means the
# assertions are about mcpp rather than about openocd being installed.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/p/src"
cd "$work/p"

cat > src/main.cpp <<'CPP'
int main() { return 0; }
CPP

# ── C: the four device slots ───────────────────────────────────────────────
cat > mcpp.toml <<'TOML'
[package]
name    = "p"
version = "0.1.0"

[target.x86_64-linux-gnu.runners]
flash   = ["/bin/sh", "-c", "echo FLASHED $0"]
monitor = ["/bin/sh", "-c", "echo MONITORED $0"]
TOML

"$MCPP" build >/dev/null 2>&1 || { echo "FAIL: build"; exit 1; }

out=$("$MCPP" run --runner flash 2>&1) || { echo "FAIL: run --runner flash exited non-zero"; echo "$out"; exit 1; }
case "$out" in *FLASHED*) ;; *) echo "FAIL: flash did not perform the declared argv"; echo "$out"; exit 1 ;; esac
echo "  ok  run --runner flash performs [target.*.runners].flash"

out=$("$MCPP" run --runner monitor 2>&1) || { echo "FAIL: run --runner monitor exited non-zero"; exit 1; }
case "$out" in *MONITORED*) ;; *) echo "FAIL: monitor did not perform its own slot"; exit 1 ;; esac
echo "  ok  run --runner monitor performs its own entry, not flash's"

# THE SLOT THAT IS NOT DECLARED MUST BE REFUSED BY NAME. An engine that fell
# back to executing the artefact would "succeed" at flashing by running the
# program on the build host, which is the failure the slot exists to prevent.
if out=$("$MCPP" run --runner debug 2>&1); then
    echo "FAIL: an undeclared runner name succeeded"; exit 1
fi
case "$out" in
    *"no runner named"*) ;;
    *) echo "FAIL: the refusal does not name the slot"; echo "$out"; exit 1 ;;
esac
case "$out" in
    *"Available:"*) ;;
    *) echo "FAIL: the refusal does not list what this project has"; exit 1 ;;
esac
echo "  ok  an undeclared name is refused, listing what this project does have"

# ── D: --locked asserts the recorded resolution ────────────────────────────
mkdir -p "$work/q/src"
cd "$work/q"
cat > mcpp.toml <<'TOML'
[package]
name    = "q"
version = "0.1.0"

[dependencies]
cmdline = "0.0.1"
TOML
cat > src/main.cpp <<'CPP'
int main() { return 0; }
CPP

"$MCPP" build >/dev/null 2>&1 || { echo "SKIP: cmdline@0.0.1 unavailable"; exit 0; }
test -f mcpp.lock || { echo "FAIL: no mcpp.lock after a build with a dependency"; exit 1; }

"$MCPP" build --locked >/dev/null 2>&1 \
    || { echo "FAIL: --locked rejected a matching lock"; exit 1; }
echo "  ok  --locked passes when the resolution matches"

# AND THE FAILING DIRECTION IS THE ONE THAT MATTERS. Measured while writing
# this: with the fast path still enabled, a corrupted lock passed `--locked` and
# printed "Finished" — the flag was accepted and the check never ran.
sed -i.bak 's/version = "0.0.1"/version = "9.9.9"/' mcpp.lock
if out=$("$MCPP" build --locked 2>&1); then
    echo "FAIL: --locked accepted a lock that does not describe this resolution"
    echo "$out" | tail -3; exit 1
fi
case "$out" in
    *"differs from mcpp.lock"*) ;;
    *) echo "FAIL: the refusal does not say what is wrong"; echo "$out"; exit 1 ;;
esac
# The drift is NAMED. "Out of date" is true and useless.
case "$out" in
    *"9.9.9 -> 0.0.1"*) ;;
    *) echo "FAIL: the refusal does not name which package moved, and to what"
       echo "$out"; exit 1 ;;
esac
echo "  ok  --locked names the package that moved and both versions"
mv mcpp.lock.bak mcpp.lock

# ── E: the bill of materials describes the RECORDED resolution ─────────────
"$MCPP" emit sbom -o sbom.json >/dev/null 2>&1 || { echo "FAIL: mcpp emit sbom"; exit 1; }
python3 - <<'PY' || exit 1
import json, sys
d = json.load(open("sbom.json"))
assert d["bomFormat"] == "CycloneDX", d.get("bomFormat")
assert d["specVersion"] == "1.5", d["specVersion"]
root = d["metadata"]["component"]
assert root["name"] == "q", root
names = [c["name"] for c in d["components"]]
assert "cmdline" in names, names
# A component with no licence must SAY so rather than omit the field: an
# absent key reads as "not examined" and is the shape a reviewer cannot filter.
for c in d["components"]:
    assert "licenses" in c, c["name"]
# The purl is what correlates a component with an advisory feed.
for c in d["components"]:
    assert c["purl"].startswith("pkg:mcpp/"), c["purl"]
print("  ok  sbom is valid CycloneDX, names every component and its licence field")
PY

# AND IT REPORTS WHAT WAS RECORDED, NOT WHAT WOULD RESOLVE NOW. This is the
# one property an SBOM must have, so it is asserted rather than assumed.
sed -i.bak 's/version = "0.0.1"/version = "7.7.7"/' mcpp.lock
"$MCPP" emit sbom -o sbom2.json >/dev/null 2>&1 || { echo "FAIL: mcpp emit sbom (2)"; exit 1; }
python3 - <<'PY' || exit 1
import json
d = json.load(open("sbom2.json"))
v = [c["version"] for c in d["components"] if c["name"] == "cmdline"]
assert v == ["7.7.7"], f"sbom re-resolved instead of reading the lock: {v}"
print("  ok  sbom reads the lock rather than re-resolving")
PY
mv mcpp.lock.bak mcpp.lock

# ── B: one board package, two environments, chosen by the consumer ─────────
#
# THE EMULATOR/HARDWARE AXIS IS A FEATURE, NOT A FORK. A board reached
# through QEMU and the same board reached through a debug probe differ only in
# the argv of their device slots. Publishing two packages would duplicate the
# linker script, the startup code and the module surface to vary four strings.
#
# AND THE SITE THIS CATCHES IS A REAL ONE. Dependency-supplied RunGlobal
# entries reach the root through a DIFFERENT code path from a package's own;
# wiring only the latter left `mcpp flash` reporting "no flash is configured"
# while `mcpp run` found the runner the same build program emitted beside it.
mkdir -p "$work/bsp/src" "$work/consumer/src"
cd "$work/bsp"
cat > mcpp.toml <<'TOML'
[package]
name    = "demo-board-rt"
version = "0.1.0"

[features]
default  = ["emulator"]
emulator = []
hardware = []
TOML
cat > build.mcpp <<'BUILD'
import mcpp;
import std;
int main() {
    if (mcpp::has_feature("hardware")) {
        // The DEFAULT runner, not a named one. On real hardware "run" means
        // flash + reset + attach + report — one command — which is why
        // `mcpp run` needs no extra argument here.
        for (auto a : {"/bin/sh", "-c", "echo PROBE-RUN $0"}) mcpp::runner(a);
        for (auto a : {"/bin/sh", "-c", "echo GDBSERVER $0"}) mcpp::runner("debug", a);
        mcpp::run_exclusive();
    } else {
        for (auto a : {"/bin/sh", "-c", "echo EMULATOR-RUN $0"}) mcpp::runner(a);
    }
    return 0;
}
BUILD
printf 'export module demo_board;\n' > src/board.cppm

cd "$work/consumer"
cat > src/main.cpp <<'CPP'
int main() { return 0; }
CPP
consumer_manifest() {
    printf '[package]\nname = "consumer"\nversion = "0.1.0"\n[dependencies]\ndemo-board-rt = { path = "../bsp"%s }\n' "$1" > mcpp.toml
}

consumer_manifest ''
rm -rf target
out=$("$MCPP" run 2>&1) || { echo "FAIL: run under the default feature"; echo "$out"; exit 1; }
case "$out" in *EMULATOR-RUN*) ;; *) echo "FAIL: default feature did not select the emulator argv"; echo "$out"; exit 1 ;; esac
echo "  ok  plain run uses the dependency's default runner"

consumer_manifest ', features = ["hardware"]'
rm -rf target
# THE 80% CASE: THE COMMAND DOES NOT CHANGE. On hardware "run" means
# flash-and-go, so the feature redefines the DEFAULT runner rather than adding
# a named one. A design requiring `--runner flash` here would have made the
# most common action the one needing an extra argument.
out=$("$MCPP" run 2>&1) || { echo "FAIL: run under features=[hardware]"; echo "$out"; exit 1; }
case "$out" in
    *PROBE-RUN*) ;;
    *EMULATOR-RUN*) echo "FAIL: the feature did not move the default runner"; exit 1 ;;
    *) echo "FAIL: unexpected run output"; echo "$out"; exit 1 ;;
esac
echo "  ok  the SAME command serves hardware — the feature moved the default"

# The hardware arm supplies a debug server; the emulator arm does not. That
# asymmetry is the point: a slot is absent when the environment has no such
# thing, and absence is reported rather than faked.
out=$("$MCPP" run --runner debug 2>&1) || { echo "FAIL: debug under features=[hardware]"; echo "$out"; exit 1; }
case "$out" in *GDBSERVER*) ;; *) echo "FAIL: debug slot did not arrive"; echo "$out"; exit 1 ;; esac
consumer_manifest ''
rm -rf target
if out=$("$MCPP" run --runner debug 2>&1); then
    echo "FAIL: --runner debug succeeded under emulator, which supplies none"; exit 1
fi
echo "  ok  a runner the chosen environment lacks is refused, not faked"

echo "PASS: named runners, --locked and emit sbom"
