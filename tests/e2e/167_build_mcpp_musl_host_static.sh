#!/usr/bin/env bash
# requires: musl elf
# A build.mcpp is a host executable. When the host toolchain targets musl it
# must be fully static, otherwise a glibc host cannot exec the generated helper
# because /lib/ld-musl-<arch>.so.1 is absent.
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "SKIP: Linux ELF regression"
    exit 0
fi

: "${MCPP:?MCPP must point to the self-built mcpp binary}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"
"$MCPP" new app > new.log 2>&1 || {
    cat new.log
    echo "FAIL: mcpp new failed"
    exit 1
}
cd app

cat >> mcpp.toml <<'EOF'
[toolchain]
default = "gcc@15.1.0-musl"
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
    echo "FAIL: musl build.mcpp helper did not run on the glibc host"
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
if readelf -lW "$helper" | grep -q 'Requesting program interpreter'; then
    readelf -lW "$helper"
    echo "FAIL: musl host helper contains PT_INTERP"
    exit 1
fi

echo "OK"
