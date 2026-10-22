#!/usr/bin/env bash
# requires: gcc elf unix-shell python3
# A stated sysroot is the C library the artifact is built against AND runs on
# (design 2026-10-23, "target base and execution host"; S1, S2, S4, S6, S7).
#
# WHY IT IS ASSERTED ON THE ARTIFACT AND NOT ON THE BUILD'S SUCCESS
#
# Before this release a stated sysroot failed in two ways that read as one.
# The link could not find `/lib64/libm.so.6` (gcc's clean link specs dropped
# `--sysroot` for ld), and once that was worked around the artifact loaded the
# HOST's loader and the SubOS farm's libc while being compiled against the
# sysroot's -- a binary that links and dies at `symbol lookup error`. So every
# case below reads PT_INTERP and RUNPATH, not only the exit status.
set -e

MCPP="${MCPP:-mcpp}"
source "$(dirname "$0")/_toolchain_env.sh"
source "$(dirname "$0")/_sysroot_fixture.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export NO_COLOR=1
fail() { echo "FAIL: $*"; echo "----"; echo "${out:-}"; exit 1; }

mkdir -p "$work/app/src"
cd "$work/app"
cat > src/main.cpp <<'CPP'
#include <gnu/libc-version.h>
import std;
int main() { std::println("built {}.{} ran {}", __GLIBC__, __GLIBC_MINOR__, gnu_get_libc_version()); }
CPP
manifest() {   # $1: the [target.x86_64-linux-gnu] body, $2: anything else
    cat > mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"
[toolchain]
default = "gcc@${GCC_VERSION}"
$2
[target.x86_64-linux-gnu]
$1
TOML
}
[[ "$(uname -m)" == x86_64 ]] || { echo "SKIP: the fixture sysroot is x86_64"; exit 0; }

# The default build first: it installs the payloads the fixture is made of,
# and its output directory is the one a stated base must not reuse.
manifest "" ""
out="$("$MCPP" build 2>&1)" || fail "the default build failed"
default_dir=$(ls -d target/x86_64-linux-gnu/*/ | head -1)
make_glibc_sysroot "$work/app/third_party/sysroot" || { echo "SKIP: no glibc payload to build a sysroot from"; exit 0; }
SR="$work/app/third_party/sysroot"

artifact() { ls target/x86_64-linux-gnu/*/bin/app | xargs ls -t | head -1; }
interp() { readelf -l "$1" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'; }
runpath() { readelf -d "$1" | sed -n 's/.*R\(UN\)\{0,1\}PATH).*\[\(.*\)\]/\2/p'; }

# ── S2: a managed compiler, the sysroot named by a path relative to the file ──
manifest 'sysroot = { path = "third_party/sysroot" }' ""
out="$("$MCPP" run 2>&1)" || fail "S2: the build or the run failed"
grep -q "Using sysroot glibc $SYSROOT_GLIBC ← third_party/sysroot  \[custom · mcpp.toml:" <<<"$out" \
    || fail "S2: no Using line naming the sysroot, its C library and where it was stated"
grep -q "built $SYSROOT_GLIBC ran $SYSROOT_GLIBC" <<<"$out" || fail "S2: the program did not run on the sysroot's glibc"
a=$(artifact)
[[ "$(interp "$a")" == "$SR/lib64/ld-linux-x86-64.so.2" ]] || fail "S2: PT_INTERP is $(interp "$a")"
rp=$(runpath "$a")
[[ "$rp" == "$SR/lib64:"* ]] || fail "S2: RUNPATH does not start with the sysroot: $rp"
grep -q "subos" <<<"$rp" && fail "S2: the SubOS farm reached a stated base's RUNPATH: $rp"
[[ "$(dirname "$(dirname "$a")")/" != "$default_dir" ]] || fail "S2: the stated base reused the default output directory"
python3 - "$(dirname "$(dirname "$a")")/resolution.json" <<'PY' || fail "S2: resolution.json does not describe the base"
import json, sys
b = json.load(open(sys.argv[1]))["base"]
assert b["form"] == "path" and b["runtime"] == "binding" and b["c_abi"]["impl"] == "glibc", b
assert b["identity"], b
PY
out="$("$MCPP" why sysroot 2>&1)" || fail "S2: mcpp why sysroot failed"
grep -q "address=gnu-sysroot  runtime=binding" <<<"$out" || fail "S2: why sysroot omits the address and runtime"

# ── S4: the base rewritten in place is a different base ───────────────────────
first=$(dirname "$(dirname "$(artifact)")")
touch -d '2001-01-01' "$SR/usr/include/features.h"
out="$("$MCPP" build 2>&1)" || fail "S4: the rebuild failed"
second=$(ls -td target/x86_64-linux-gnu/*/ | head -1)
[[ "${second%/}" != "$first" ]] || fail "S4: a sysroot rewritten in place reused the output directory"

# ── S7: overrides outrank the project, the machine's default fills a gap ──────
make_glibc_sysroot "$work/other" >/dev/null
out="$(MCPP_SYSROOT="path:$work/other" "$MCPP" build 2>&1)" || fail "S7: MCPP_SYSROOT failed"
grep -q "Using sysroot glibc .* ← $work/other  \[custom · env MCPP_SYSROOT\]" <<<"$out" \
    || fail "S7: MCPP_SYSROOT did not outrank the manifest"
out="$(MCPP_TARGET_X86_64_LINUX_GNU_SYSROOT="path:$work/other" "$MCPP" build 2>&1)" || fail "S7: the per-target variable failed"
grep -q "env MCPP_TARGET_X86_64_LINUX_GNU_SYSROOT" <<<"$out" || fail "S7: the per-target variable was not used"
out="$("$MCPP" --managed-only build 2>&1 || true)"
grep -q "sysroot ← third_party/sysroot" <<<"$out" || fail "S7: --managed-only did not name the sysroot"
cfgdir="$work/home-cfg"; mkdir -p "$cfgdir"
manifest "" ""
home="${MCPP_HOME:-$HOME/.mcpp}"
cp "$home/config.toml" "$work/config.toml.bak" 2>/dev/null || true
restore() { if [[ -f "$work/config.toml.bak" ]]; then cp "$work/config.toml.bak" "$home/config.toml"; fi; }
trap 'restore; rm -rf "$work"' EXIT
printf '\n[target.x86_64-linux-gnu]\nsysroot = { path = "%s" }\n' "$work/other" >> "$home/config.toml"
out="$("$MCPP" build 2>&1)"; rc=$?
restore
[[ $rc -eq 0 ]] || fail "S7: the machine default failed"
grep -q "Using sysroot glibc .* ← $work/other  \[custom · .*config.toml" <<<"$out" \
    || fail "S7: config.toml's [target.<triple>].sysroot was not the default"

# ── S6: what is not a sysroot, and what is not there ─────────────────────────
manifest 'sysroot = { path = "does/not/exist" }' ""
out="$("$MCPP" build 2>&1 || true)"
grep -q "which does not exist" <<<"$out" || fail "S6: a missing path was not named"
grep -q "publish it as a package in your index" <<<"$out" || fail "S6: the message omits how to make it exist"
grep -q "MCPP_TARGET_X86_64_LINUX_GNU_SYSROOT=path:<dir>" <<<"$out" || fail "S6: the message omits the local override"
mkdir -p "$work/app/libs-only/usr/include"
manifest 'sysroot = { path = "libs-only" }' ""
out="$("$MCPP" build 2>&1 || true)"
grep -q "library prefix rather than a sysroot" <<<"$out" || fail "S6: a directory with no C library was accepted"

# ── S3: the compiler's runtime requires more than the base supplies ───────────
make_glibc_sysroot "$work/old" >/dev/null
sed -i 's/^\(#define[[:space:]]*__GLIBC_MINOR__[[:space:]]*\)[0-9]*/\120/' "$work/old/usr/include/features.h"
manifest "sysroot = { path = \"$work/old\" }" ""
out="$("$MCPP" build 2>&1 || true)"
grep -q "runtime requires glibc >= " <<<"$out" && grep -q "this sysroot provides glibc 2.20" <<<"$out" \
    || fail "S3: a base older than the compiler's runtime was not refused naming both versions"
grep -q "error:.*std.cc" <<<"$out" && fail "S3: the refusal arrived as a std module error"

# ── S1: a toolchain named by path, with the sysroot it carries ────────────────
manifest "" "[toolchain.linux]
path    = \"$MCPP_E2E_GCC_ROOT\"
sysroot = \"$SR\""
sed -i '/^default = /d' mcpp.toml
out="$("$MCPP" run 2>&1)" || fail "S1: [toolchain] { path, sysroot } failed"
grep -q "built $SYSROOT_GLIBC ran $SYSROOT_GLIBC" <<<"$out" || fail "S1: the program did not run on the carried sysroot"
[[ "$(interp "$(artifact)")" == "$SR/lib64/ld-linux-x86-64.so.2" ]] || fail "S1: PT_INTERP is not the carried sysroot's"
# `--no-strip`: a tree named by path need not carry binutils, and pack
# refuses to ship debug information it was not told to keep.
out="$("$MCPP" pack --no-strip 2>&1)" || fail "S1: pack failed"
grep -q "Requires glibc >= " <<<"$out" || fail "S1: pack did not state the C library floor"

echo "PASS: a stated sysroot is the C library the artifact is built against and runs on"
