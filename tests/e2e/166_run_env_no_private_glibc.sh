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

# ── The other half: when a dlopen-reachable dependency library DOES exist,
# the payload dir must still be there. Without this, a later change could drop
# the entry entirely and the negative assertion above would happily pass.
GLIBC_STORE=$(ls -d "$HOME"/.mcpp/registry/data/xpkgs/xim-x-glibc/*/ 2>/dev/null | head -1)
if [ -z "$GLIBC_STORE" ]; then
    echo "SKIP (positive half): no private glibc payload installed"
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

out2=$("$MCPP" run 2>&1) || { echo "FAIL: mcpp run failed (positive half)"; echo "$out2"; exit 1; }
line2=$(printf '%s\n' "$out2" | grep -oE 'LDLP=\[[^]]*\]' | head -1)
echo "observed (with [runtime] library_dirs): $line2"

printf '%s' "$line2" | grep -q 'xim-x-glibc' || {
    echo "FAIL: a build WITH a dlopen-reachable dependency library dir lost the"
    echo "      private glibc payload from LD_LIBRARY_PATH. dlopen'd libraries do"
    echo "      not consult the executable's RUNPATH, so they need it here."
    echo "      $line2"
    exit 1; }

echo OK
