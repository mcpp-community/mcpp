#!/usr/bin/env bash
# requires:
# mcpp#291 — a plain binary must NOT be handed the private glibc payload on
# LD_LIBRARY_PATH.
#
# That variable is inherited by the entire process subtree. When the target is
# something like a course provider that shells out (popen("mcpp test ...")),
# /bin/sh is a HOST binary: its PT_INTERP is baked in, so it loads the HOST
# ld.so while this variable hands it the PAYLOAD libc.so.6. glibc's libc and
# ld.so are version-locked to each other via GLIBC_PRIVATE, so on any host
# whose glibc differs from the payload's the shell dies of SIGSEGV inside the
# dynamic linker — before main, with empty stdout and no diagnostic.
#
# The payload dir belongs on LD_LIBRARY_PATH only when the build actually has
# a dlopen()-reachable dependency library (whose own DT_NEEDED closure cannot
# see the executable's RUNPATH). A project with no such dependency must get
# nothing.
#
# ASSERTS ON THE EMITTED ENVIRONMENT, not on whether a shell crashes: the crash
# needs host glibc != payload glibc. On a matching host (the common CI case)
# a crash-based test passes for the wrong reason and would never have caught
# this in the first place.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/src
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "envprobe"
version = "0.1.0"
standard = "c++23"
EOF

# Print the loader path variable exactly as the run target receives it.
cat > src/main.cpp <<'EOF'
#include <cstdio>
#include <cstdlib>
int main() {
    const char* p = std::getenv("LD_LIBRARY_PATH");
    std::printf("LDLP=[%s]\n", p ? p : "");
    return 0;
}
EOF

out=$("$MCPP" run 2>&1) || { echo "FAIL: mcpp run failed"; echo "$out"; exit 1; }

line=$(printf '%s\n' "$out" | grep -oE 'LDLP=\[[^]]*\]' | head -1)
[ -n "$line" ] || { echo "FAIL: probe never printed LDLP"; echo "$out"; exit 1; }
echo "observed: $line"

# The specific poison: the private glibc payload store directory.
if printf '%s' "$line" | grep -q 'xim-x-glibc'; then
    echo "FAIL: the run target was handed the private glibc payload on LD_LIBRARY_PATH."
    echo "      $line"
    echo "      A binary with no dlopen-reachable dependency must not get it —"
    echo "      it propagates to every descendant process, including host shells."
    exit 1
fi

# ── The other half. This test used to require the OPPOSITE here: with a
# dlopen-reachable dependency library present, the payload dir had to BE on
# LD_LIBRARY_PATH, "because dlopen'd libraries do not consult the executable's
# RUNPATH". The guard was right to exist — it stopped anyone from "fixing"
# mcpp#291 by deleting the entry and quietly breaking dlopen — but it asserted
# the MECHANISM instead of the capability, and the mechanism was wrong:
#
#   * a dlopen() performed by the executable DOES consult the executable's
#     DT_RUNPATH, and the link model already puts the payload glibc there
#     (measured: the artifact's RUNPATH is byte-identical with and without the
#     environment entry);
#   * so the entry bought nothing, while reaching every descendant process —
#     and on a payload whose GLIBC_PRIVATE needs match its own loader, that
#     kills /bin/sh outright (mcpp#401, glibc 2.44).
#
# The capability it was really protecting now lives in
# 208_private_libc_stays_in_the_binary.sh, which asserts the dlopen actually
# resolves and that the directory is in the artifact RUNPATH. What belongs
# here is the rule this test is named for, applied to BOTH shapes: the private
# libc is never handed to the process environment.
GLIBC_STORE=$(ls -d "$HOME"/.mcpp/registry/data/xpkgs/xim-x-glibc/*/ 2>/dev/null | head -1)
if [ -z "$GLIBC_STORE" ]; then
    echo "SKIP (second half): no private glibc payload installed"
    echo OK
    exit 0
fi

cd "$TMP"
mkdir -p pkg2/src pkg2/runtime
cd pkg2
cat > mcpp.toml <<'EOF'
[package]
name    = "envprobe2"
version = "0.1.0"
standard = "c++23"

[runtime]
library_dirs = ["runtime"]
EOF
cp ../pkg/src/main.cpp src/main.cpp

out2=$("$MCPP" run 2>&1) || { echo "FAIL: mcpp run failed (second half)"; echo "$out2"; exit 1; }
line2=$(printf '%s\n' "$out2" | grep -oE 'LDLP=\[[^]]*\]' | head -1)
echo "observed (with [runtime] library_dirs): $line2"

if printf '%s' "$line2" | grep -q 'xim-x-glibc'; then
    echo "FAIL: a build WITH a dlopen-reachable dependency library dir still put"
    echo "      the private glibc payload on LD_LIBRARY_PATH. That variable"
    echo "      reaches every descendant process, including host shells the host"
    echo "      loader loads — see mcpp#401."
    echo "      $line2"
    exit 1
fi

# The project's own runtime dir is a plain shared-library directory with no
# loader coupling, so it keeps its environment scope. Losing it here would mean
# the entry was dropped wholesale rather than narrowed to the private libc.
printf '%s' "$line2" | grep -q 'runtime' || {
    echo "FAIL: the project's own [runtime] library_dirs entry disappeared too."
    echo "      Only the private libc is binary-scoped; ordinary dependency"
    echo "      runtime dirs still belong in the environment."
    echo "      $line2"
    exit 1; }

echo OK
