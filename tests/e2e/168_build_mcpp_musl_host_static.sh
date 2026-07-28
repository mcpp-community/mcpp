#!/usr/bin/env bash
# requires: elf
# 168_build_mcpp_musl_host_static.sh — a build.mcpp is a HOST executable that
# the OS execs directly. Under a musl host toolchain it must be fully static:
# a dynamic musl binary carries PT_INTERP=/lib/ld-musl-<arch>.so.1, an absolute
# path no toolchain payload installs and no glibc distro ships, so execve fails
# with ENOENT and the build dies as an unexplained exit 127 (#295).
#
# Runs standalone too — the aarch64 fresh-install workflow invokes it directly:
#   MCPP=/path/to/mcpp bash tests/e2e/168_build_mcpp_musl_host_static.sh
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "SKIP: Linux ELF regression"
    echo "OK"
    exit 0
fi

: "${MCPP:?MCPP must point to the mcpp binary under test}"

# Which musl toolchain to pin is a property of the machine, not of the test:
# x86_64 dev boxes carry 15.1.0, the aarch64 runner resolves 16.1.0. Discover
# the installed payload instead of hardcoding either — which is also why this
# owns its own precondition (`requires: elf`, not `requires: musl`): run_all.sh's
# musl capability probe is pinned to x86_64/15.1.0 and would false-skip on the
# aarch64 host where this regression actually lives. Never auto-install here:
# that would be a ~200 MB download per CI run (same rule as 28_target_static).
find_musl_version() {
    local root gxx
    for root in "${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs/xim-x-musl-gcc" \
                "$HOME/.xlings/data/xpkgs/xim-x-musl-gcc"; do
        [[ -d "$root" ]] || continue
        gxx=$(ls "$root"/*/bin/*-linux-musl-g++ 2>/dev/null | head -1 || true)
        if [[ -n "$gxx" ]]; then
            # <root>/<version>/bin/<triple>-g++ → <version>
            basename "$(dirname "$(dirname "$gxx")")"
            return 0
        fi
    done
    return 1
}

MUSL_VERSION=$(find_musl_version || true)
if [[ -z "$MUSL_VERSION" ]]; then
    echo "SKIP: no musl-gcc payload installed — not testing the musl host helper"
    echo "OK"
    exit 0
fi
echo "musl toolchain: gcc@${MUSL_VERSION}-musl"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"
"$MCPP" new app > new.log 2>&1 || {
    cat new.log
    echo "FAIL: mcpp new failed"
    exit 1
}
cd app

cat >> mcpp.toml <<EOF

[toolchain]
default = "gcc@${MUSL_VERSION}-musl"
EOF

cat > build.mcpp <<'EOF'
#include <cstdio>
int main() {
    FILE* marker = std::fopen("helper-ran", "w");
    if (!marker) return 2;
    std::fputs("ok\n", marker);
    std::fclose(marker);
    std::puts("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
EOF

"$MCPP" build > build.log 2>&1 || {
    cat build.log
    echo "FAIL: musl build.mcpp helper did not run on this host"
    exit 1
}

helper=target/.build-mcpp/build.mcpp.bin
test -f helper-ran || { cat build.log; echo "FAIL: build.mcpp did not run"; exit 1; }
test -x "$helper" || { cat build.log; echo "FAIL: helper binary missing"; exit 1; }

file "$helper"
file "$helper" | grep -q 'statically linked' || {
    echo "FAIL: musl host helper is not statically linked"
    exit 1
}
# The property that actually decides whether execve succeeds. NB: an `if`, not
# `! readelf ... | grep -q ...` — bash exempts !-inverted pipelines from
# errexit, so the negated form would never fail the test.
if readelf -lW "$helper" | grep -q 'Requesting program interpreter'; then
    readelf -lW "$helper"
    echo "FAIL: musl host helper contains PT_INTERP"
    exit 1
fi

echo "OK"
