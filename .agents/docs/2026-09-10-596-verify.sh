#!/usr/bin/env bash
# Ecosystem verification for mcpp#596: the driver farm mirrors the sentinel,
# and mcpp reports a dlopen surface it cannot satisfy. Against a PUBLISHED
# mcpp, a PUBLISHED xim:libcuda-host-link and a PUBLISHED compat:sycl-runtime.
#
#   # The sandbox has an EMPTY $HOME and a fresh /tmp, so this file is not
#   # visible from inside it. Pass the script itself in:
#   B64=$(base64 -w0 <this file>)
#   xlings subos use verify-596 --sandbox --cmd \
#     "echo $B64 | base64 -d > /tmp/v.sh && MCPP_VERIFY_VERSION=2026.9.10.1 bash /tmp/v.sh"
#
# mcpp is addressed by its STORE path, which is the one thing the sandbox does
# share: the xlings data directory. A bare `mcpp` is not on PATH in there.
#
# WHAT A SANDBOX CAN AND CANNOT DECIDE HERE.
#
# It decides everything about the PACKAGING, which is where the defect was: how
# many driver sonames the sentinel publishes, and whether the farm carries all
# of them. Those are properties of what gets installed, and a machine that has
# built this before answers them from a directory that already existed.
#
# It cannot decide anything about a DEVICE. The sandbox's /dev has fourteen
# entries and no NVIDIA node, so `libcuda.so.1` resolves to a dangling link
# there exactly as it does on any machine without a driver -- which is a
# supported configuration and is asserted as such below, not worked around. The
# device side is measured on the host and recorded in the design record.
#
# EVERY CRITERION NAMES THE OBJECT IT SELECTED, and every section that did not
# run is listed again in the summary. "0 assertions failed" printed by a script
# that skipped three sections is the failure mode this shape exists to prevent.
set -u

VER="${MCPP_VERIFY_VERSION:?set MCPP_VERIFY_VERSION}"
STORE="${MCPP_VERIFY_BIN:-$HOME/.xlings/data/xpkgs/xim-x-mcpp/$VER/bin/mcpp}"
SENTINEL_VER="${MCPP_VERIFY_SENTINEL:-0.0.2}"
COMPAT_VER="${MCPP_VERIFY_COMPAT:-2026.09.10}"

fails=0
skipped=""
fail()    { printf 'ASSERT-FAIL: %s\n' "$1"; fails=$((fails + 1)); }
ok()      { printf 'ok: %s\n' "$1"; }
section() { printf '\n== %s ==\n' "$1"; }
skip()    { printf 'NOT RUN: %s\n' "$1"; skipped="$skipped
  - $1"; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

section "A. the published mcpp answers for itself"
if [ ! -x "$STORE" ]; then
    fail "no mcpp at $STORE"
    printf '\nfails=%d (nothing else can run)\n' "$fails"
    exit 1
fi
got=$("$STORE" --version 2>&1 | head -1)
case "$got" in
    *"$VER"*) ok "mcpp --version says $got" ;;
    *)        fail "mcpp --version says '$got', expected $VER" ;;
esac

# ---------------------------------------------------------------------------
section "B. the sentinel publishes a SET, and the size is the assertion"
#
# The count is the criterion rather than the presence of one name: a list that
# lost an entry passes every per-name test that only asks about the names it
# still has.
if "$STORE" self >/dev/null 2>&1 || true; then :; fi
xl="$HOME/.xlings/data/xpkgs/xim-x-libcuda-host-link/$SENTINEL_VER/lib"
if command -v xlings >/dev/null 2>&1; then
    xlings install "libcuda-host-link@$SENTINEL_VER" -y >"$work/sentinel.log" 2>&1 || true
fi
if [ -d "$xl" ]; then
    n=$(ls -1 "$xl" | wc -l)
    [ "$n" -ge 2 ] && ok "sentinel $SENTINEL_VER publishes $n sonames" \
                   || fail "sentinel publishes $n soname(s); the set is at least 2"
    for s in libcuda.so.1 libnvidia-ml.so.1; do
        # islink, not exists: a dangling link is the documented self-heal shape
        # on a machine with no driver, and the sandbox is such a machine.
        [ -L "$xl/$s" ] && ok "sentinel carries $s" || fail "sentinel has no $s"
    done
else
    skip "B: the sentinel is not installed in this environment ($xl)"
fi

# ---------------------------------------------------------------------------
section "C. the farm mirrors the sentinel"
#
# THE DEFECT ITSELF. The farm linked one hand-written name while enumerating
# every other directory it draws from, so it carried libcuda.so.1 and not
# libnvidia-ml.so.1, the CUDA adapter did not load, and the program aborted
# with no message.
cat >"$work/mcpp.toml" <<TOML
[package]
name    = "farm-probe"
version = "0.1.0"

[dependencies.compat]
sycl-runtime = "$COMPAT_VER"
TOML
mkdir -p "$work/src"
cat >"$work/src/main.cpp" <<'CPP'
int main() { return 0; }
CPP
if (cd "$work" && "$STORE" build >"$work/build.log" 2>&1); then
    # Searched under the REGISTRY roots rather than all of $HOME: a developer
    # home holds tens of gigabytes of packages and the walk costs minutes,
    # which reads exactly like a hung verification.
    farm=""
    for root in "${MCPP_HOME:-$HOME/.mcpp}" "$work/.mcpp" "$HOME/.xlings"; do
        [ -d "$root" ] || continue
        farm=$(find "$root" -path "*compat-x-sycl-runtime/$COMPAT_VER*/sycl_runtime/lib" \
                    -type d 2>/dev/null | head -1)
        [ -n "$farm" ] && break
    done
    if [ -n "$farm" ]; then
        for s in libcuda.so.1 libnvidia-ml.so.1; do
            [ -L "$farm/$s" ] && ok "farm carries $s" \
                              || fail "farm has no $s -- this is mcpp#596"
        done
        # The denominator: a farm that failed to build is empty, and every
        # per-name test above would then have failed for the wrong reason.
        n=$(ls -1 "$farm" | wc -l)
        [ "$n" -ge 20 ] && ok "farm has $n entries" \
                        || fail "farm has only $n entries; the payload half did not build"
    else
        fail "C: no farm directory for compat:sycl-runtime@$COMPAT_VER"
    fi
else
    skip "C: the probe project did not build (see $work/build.log; the dpcpp payload is over a gigabyte)"
fi

# ---------------------------------------------------------------------------
section "D. mcpp reports a dlopen surface it cannot satisfy"
#
# The record rather than the message: a test that greps a warning's wording
# fails the next time the wording improves. Both denominators are asserted for
# the reason they exist -- "no findings" and "nothing was examined" must not
# read the same.
#
# THE OBJECT IS SELECTED BY IDENTITY, NOT BY WHERE `find` ARRIVES FIRST. The
# probe project's own build tree is `$work/target/<triple>/<fingerprint>`, and
# a run that also builds a dependency can leave more than one resolution.json
# under the work tree. "The first one" is a property of directory traversal
# order, not of the record being tested, so the count is asserted and named.
res=""
res_n=0
for candidate in "$work"/target/*/*/resolution.json; do
    [ -f "$candidate" ] || continue
    res_n=$((res_n + 1))
    res="$candidate"
done
if [ "$res_n" -gt 1 ]; then
    fail "D: $res_n resolution.json files under $work/target; the probe builds one project"
    res=""
fi
if [ -n "$res" ] && command -v python3 >/dev/null 2>&1; then
    ok "reading ${res#"$work"/}"
    python3 - "$res" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
rec = doc.get("runtime", {}).get("dlopen_surface")
if rec is None:
    print("ASSERT-FAIL: resolution.json has no runtime.dlopen_surface")
    sys.exit(1)
# A published non-answer carries its reason. Distinguishing it from a real
# reading is the whole point of publishing one: this assertion is what caught
# the check erasing its own record on a second pass.
if rec.get("reason"):
    print(f"ASSERT-FAIL: the check did not apply: {rec['reason']}")
    sys.exit(1)
members, walked = rec.get("members", 0), rec.get("walked", 0)
if members <= 0:
    print(f"ASSERT-FAIL: dlopen_surface examined {members} members")
    sys.exit(1)
print(f"ok: dlopen_surface examined {walked} of {members} members")
missing = [f for f in rec.get("findings", []) if f.get("kind") == "missing"]
for f in missing:
    print(f"note: {f['library']} needs {f['soname']} (declared unserved: libOpenCL.so.1)")
bad = [f for f in missing if f.get("soname") == "libnvidia-ml.so.1"]
if bad:
    print("ASSERT-FAIL: NVML is still missing from the farm -- mcpp#596")
    sys.exit(1)
print("ok: no driver soname is missing from the farm")
PY
    [ $? -eq 0 ] || fails=$((fails + 1))
elif [ "$res_n" -le 1 ]; then
    skip "D: no resolution.json under $work/target (section C did not build) or no python3"
fi

# ---------------------------------------------------------------------------
printf '\n== summary ==\n'
printf 'assertions failed: %d\n' "$fails"
if [ -n "$skipped" ]; then
    printf 'sections NOT RUN:%s\n' "$skipped"
    printf 'A pass with sections not run is not a pass for those sections.\n'
fi
exit $((fails > 0))
