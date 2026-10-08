#!/usr/bin/env bash
# requires: llvm mingw-host-headers python3
source "$(dirname "${BASH_SOURCE[0]}")/_toolchain_env.sh"
# 738 -- when the target's C library and C++ runtime both come from the
# dependency graph, clang's own driver stops searching the HOST's copies of
# either (mcpp#662).
#
# THE DEFECT. The link side has read `plan.targetSide.cAbi.prebuilt()` since
# #511 and dropped `-nostdlib` when a package supplies the C library. The
# compile side asked the same question of a DIFFERENT, narrower predicate —
# and for the one arrangement openkal actually ships (C library AND C++
# runtime both from the graph) that predicate answered false, so clang kept
# its own header search active. A C unit that text-includes a host mingw
# header no synced package intended (`io.h`, wanted by a package written for
# an ordinary MinGW Windows CRT) found the HOST's copy instead of failing --
# and where that copy's declarations disagreed with the graph's C library
# (musl), a compile that should not have found `io.h` at all failed on a
# TYPE CONFLICT instead, naming neither the host header nor why it was read.
#
# THE CRITERION IS THE SEARCH LIST ITSELF, NOT WHETHER THE BUILD SUCCEEDS.
# A build can succeed while quietly mixing two C libraries' declarations, and
# it can fail on an unrelated, in-graph gap (a package not yet ported to
# musl) that this fix does nothing about and must not be blamed for. So this
# test takes each unit's OWN command line out of the build database, adds
# `-v -fsyntax-only`, and inspects clang's own report of where it looked —
# the same judgement call `an-implicit-include-search-is-not-on-the-command-
# line` records: an implicit search is invisible on the command line and
# visible only in the driver's verbose report.
#
# GATED ON `mingw-host-headers`, NOT ON THE BUILD SUCCEEDING. The assertion
# "the host's mingw does not appear" has no discriminating power on a host
# that never had it -- the search list would end at the compiler's own
# resource directory before AND after this fix, for an unrelated reason.
#
# The dedicated CI job supplies an xlings-managed MinGW fixture in a
# separate store. An unrestricted driver must first find those headers;
# only then can their absence from graph-supplied commands prove isolation.
# Ordinary suites can still use the distro's host MinGW prefix.
set -e

MCPP="${MCPP:-mcpp}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

# PATH exposes a genuine host cross toolchain, outside the store whose
# include paths the graph is allowed to use. Do not export GCC_ROOT: it is
# a compiler control variable, not a fixture label.
if [ -n "${MCPP_E2E_HOST_MINGW_ROOT:-}" ]; then
    export PATH="$MCPP_E2E_HOST_MINGW_ROOT/bin:$PATH"
    python3 - "$LLVM_ROOT/bin/clang" "$MCPP_E2E_HOST_MINGW_ROOT" "${MCPP_HOME:-$HOME/.mcpp}" <<'PYCONTROL'
from pathlib import Path
import subprocess, sys
compiler, root, home = sys.argv[1], Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()
if root.is_relative_to(home):
    sys.exit("FAIL: the host MinGW fixture must be outside MCPP_HOME")
include = (root / "x86_64-w64-mingw32/include").resolve()
if not (include / "io.h").is_file():
    sys.exit("FAIL: the separate host MinGW headers are absent")
r = subprocess.run([compiler, "--no-default-config", "--target=x86_64-w64-windows-gnu",
                    "-E", "-v", "-x", "c", "-"], input="#include <io.h>\n",
                   capture_output=True, text=True)
searched, capture = [], False
for line in r.stderr.splitlines():
    if "search starts here" in line:
        capture = True
    elif "End of search list" in line:
        capture = False
    elif capture:
        searched.append(Path(line.strip()).resolve())
if r.returncode != 0 or include not in searched:
    print(r.stderr)
    sys.exit("FAIL: unrestricted Clang did not find the host MinGW fixture")
print("ok: unprotected clang finds the separate host MinGW headers")
PYCONTROL
fi

mkdir -p "$TMP/app/src"
cd "$TMP/app"

# Same stack as e2e 286 (the ecosystem's own arrangement, not a synthetic
# manifest) plus a plain C unit: the defect's C-library half is invisible
# from `import std`-only sources, which is exactly why the issue's own
# minimal repro (root project only `import std`) did not reach it.
cat > mcpp.toml <<TOML
[package]
name    = "hdriso"
version = "0.1.0"

[toolchain]
default = "llvm@${LLVM_VERSION}"

[dependencies]
openkal-musl = "0.3.5"
openkal-windows = "0.1.5"
openkal-llvm-runtime = "0.1.3"
TOML

cat > src/main.cpp <<'CPP'
#include <cstdio>
int main() { std::puts("hi"); return 0; }
CPP
cat > src/probe.c <<'C'
int probe(void) { return 0; }
C

if ! out="$("$MCPP" build --target x86_64-windows-gnu --cache off 2>&1)"; then
    case "$out" in
      *"not found in the synced index"*|*"install_packages failed"*)
        echo "SKIP: the openkal packages are not reachable from here"
        exit 0 ;;
    esac
    echo "FAIL: the openkal windows-gnu stack did not build"
    printf '%s\n' "$out" | grep -iE 'error' | head -5
    exit 1
fi
printf '%s\n' "$out" > build.log

cdb="compile_commands.json"
[ -f "$cdb" ] || cdb="$(find target -name compile_commands.json | head -1)"
[ -n "$cdb" ] && [ -f "$cdb" ] || fail "no compile_commands.json produced"

home="${MCPP_HOME:-$HOME/.mcpp}"
if ! python3 - "$cdb" "$home" <<'PY'
import json, subprocess, sys, os

cdb_path, home = sys.argv[1], os.path.realpath(sys.argv[2])
entries = json.load(open(cdb_path))
units = [e for e in entries
         if e["file"].endswith("main.cpp") or e["file"].endswith("probe.c")]
if len(units) != 2:
    print(f"FAIL: expected 2 units (main.cpp, probe.c), found {len(units)}")
    print(json.dumps([e["file"] for e in entries], indent=1))
    sys.exit(1)

bad = False
for e in units:
    args = e.get("arguments") or e.get("command")
    r = subprocess.run(list(args) + ["-v", "-fsyntax-only"],
                        capture_output=True, text=True, cwd=e.get("directory"))
    lines, capture, searched = r.stderr.splitlines(), False, []
    for l in lines:
        if "search starts here" in l:
            capture = True
            continue
        if "End of search list" in l:
            capture = False
            continue
        if capture:
            searched.append(l.strip())

    if not searched:
        print(f"FAIL: {e['file']}: clang printed no header search list "
              "(the assertion below would be vacuous)")
        print(r.stderr)
        bad = True
        continue

    # THE CRITERION: every directory clang looked in is under the mcpp store
    # (the packages' own directories, OR the compiler payload's resource
    # directory, which lives in the same store) -- nothing from the host.
    outside = [p for p in searched
               if os.path.realpath(p).startswith(os.sep)
               and not os.path.realpath(p).startswith(home)]
    print(f"{e['file']}: {len(searched)} directories searched, "
          f"{len(outside)} outside the store")
    if outside:
        print("  outside the store:")
        for p in outside:
            print(f"    {p}")
        bad = True

sys.exit(1 if bad else 0)
PY
then
    fail "the search list names a directory outside the store" build.log
fi

echo "PASS: 738 a graph-supplied target closes the host's own search"
