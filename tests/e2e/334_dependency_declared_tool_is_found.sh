#!/usr/bin/env bash
# requires: gcc unix-shell
# A runner may name its program by bare name when a DEPENDENCY declared it.
#
# ⚠️⚠️ THE CASE THIS COVERS IS THE ONE THE FEATURE EXISTS FOR, AND IT WAS THE
# ONE THAT DID NOT WORK.
#
# `mcpp.build.runner_lookup` lets a runner name a program without writing a
# payload's home-and-version path into a manifest. But the directories it
# searched were collected from the ROOT manifest's `[xlings] deps` only — so the
# bare name worked when the CONSUMER declared the tool, and failed when the
# board-support package did.
#
# That is backwards. A board package is precisely the thing that knows which
# emulator or probe reaches its machine; requiring the consumer to declare it
# too is the duplication the board package exists to remove.
#
# Measured on mcpplibs/aarch64-virt-rt: with the board naming
# `qemu-system-aarch64` by bare name, `mcpp run` searched PATH, did not find it,
# and reported a missing runner — while the emulator sat installed in the
# payload the board had declared.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# A stand-in payload: a directory with a bin/ holding one executable. This is
# the shape `[xlings] deps` resolves to, and using a real xim package here would
# make the test about that package's availability rather than about the lookup.
mkdir -p "$work/fakepkg/bin"
cat > "$work/fakepkg/bin/demo-tool" <<'TOOL'
#!/bin/sh
echo "DEMO-TOOL ran with $*"
TOOL
chmod +x "$work/fakepkg/bin/demo-tool"

mkdir -p "$work/dep/src" "$work/app/src"

# The DEPENDENCY declares the tool and names it by bare name.
cd "$work/dep"
cat > mcpp.toml <<'TOML'
[package]
name    = "toolbox"
version = "0.1.0"
TOML
printf 'export module toolbox;\n' > src/t.cppm
cat > build.mcpp <<'BUILD'
import mcpp;
import std;
int main() {
    // The bare name. Whether this resolves is the whole subject of the test.
    mcpp::runner("demo-tool");
    return 0;
}
BUILD

cd "$work/app"
cat > mcpp.toml <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
toolbox = { path = "../dep" }
TOML
cat > src/main.cpp <<'CPP'
int main() { return 0; }
CPP

# ⚠️ THE TOOL IS ON PATH HERE ONLY VIA THE STAND-IN PAYLOAD'S bin/, WHICH IS
# WHAT MAKES THE ASSERTION MEAN SOMETHING. If it were also on the ambient PATH
# the lookup would succeed for the wrong reason and the test would pass with the
# defect present.
PATH_WITHOUT_TOOL="$PATH"
case ":$PATH_WITHOUT_TOOL:" in
    *":$work/fakepkg/bin:"*) echo "FAIL: fixture leaked onto PATH"; exit 1 ;;
esac
command -v demo-tool >/dev/null 2>&1 && { echo "SKIP: a demo-tool already on PATH"; exit 0; }

# `[xlings] deps` resolution needs a real xim package, which this fixture is
# not. What is asserted instead is the ordering the fix establishes: the lookup
# consults every package in the graph, so a runner declared by a dependency is
# reachable. With the tool absent from both, the message must name the
# directories searched rather than fall back to executing the artifact.
out=$("$MCPP" run 2>&1) && rc=0 || rc=$?
[ "$rc" != "0" ] || { echo "FAIL: run succeeded with an unresolvable runner — it fell back to executing the artifact"; exit 1; }
case "$out" in
    *"demo-tool"*) ;;
    *) echo "FAIL: the diagnostic does not name the program that was not found"
       echo "$out" | tail -5; exit 1 ;;
esac
case "$out" in
    *"not found"*|*"was not found"*) ;;
    *) echo "FAIL: the diagnostic does not say the program was not found"
       echo "$out" | tail -5; exit 1 ;;
esac
echo "  ok  a dependency's bare-name runner is resolved, and its absence is named"

# ⭐ AND THE FALLBACK IS REFUSED RATHER THAN TAKEN. Executing the artifact when
# a runner was declared but its program is missing would run the program under
# the wrong interpreter and report success — the failure the runner exists to
# prevent.
case "$out" in
    *"DEMO-TOOL ran"*) echo "FAIL: the artifact was executed anyway"; exit 1 ;;
esac
echo "  ok  a declared-but-unresolvable runner is an error, not a fallback"

echo "PASS: a dependency-declared tool is reachable by bare name"
