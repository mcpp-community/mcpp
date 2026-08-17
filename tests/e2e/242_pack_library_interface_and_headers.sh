#!/usr/bin/env bash
# requires: gcc
# 242_pack_library_interface_and_headers.sh — `mcpp pack <lib target>` produces
# a package a consumer can use through EITHER interface mode, or both at once.
#
# Acceptance for §1.1 / §2 of
# .agents/docs/2026-08-17-library-distribution-design.md: a package carries
# `include/` (text, consumed by #include) and `interface/` (module units the
# consumer compiles), the two do not interfere, and neither needs a flag.
#
# Also pins the two lists `mcpp pack` prints. A closed-source publisher needs
# to see what is NOT travelling as much as what is.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── the producer: a module interface AND a C header, over one library ──
mkdir -p mathkit/src mathkit/include
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
export import :api;
EOF
cat > mathkit/src/api.cppm <<'EOF'
export module mathkit:api;
export namespace mk { int add(int a, int b); }
EOF
cat > mathkit/src/secret.cppm <<'EOF'
module mathkit:secret;
namespace mk { int bias() { return 0; } }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk {
int bias();
int add(int a, int b) { return a + b + bias(); }
}
EOF
cat > mathkit/src/capi.c <<'EOF'
int mathkit_add(int a, int b) { return a + b; }
EOF
cat > mathkit/include/mathkit_c.h <<'EOF'
#ifdef __cplusplus
extern "C" {
#endif
int mathkit_add(int a, int b);
#ifdef __cplusplus
}
#endif
EOF
cat > mathkit/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources      = ["src/*.cppm", "src/*.cpp", "src/*.c"]
include_dirs = ["include"]
[targets.mathkit]
kind = "lib"
EOF

cd mathkit
"$MCPP" pack mathkit > pack.log 2>&1 || { cat pack.log; echo "pack failed"; exit 1; }

pkg="$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"
# The manifest below is FILE CONTENT: on Git Bash a shell-spelled
# /tmp/... path is read by a native mcpp.exe as "root of the current
# drive". host_path is the conversion (tests/e2e/_host_path.sh).
PKG_HOST="$(host_path "$TMP/mathkit/$pkg")"
[[ -n "$pkg" ]] || { cat pack.log; echo "no package directory"; exit 1; }

# The layout is the contract: two interface modes, one artifact dir per triple.
for f in mcpp.toml interface/mathkit.cppm interface/api.cppm include/mathkit_c.h; do
    [[ -f "$pkg/$f" ]] || { echo "package is missing $f"; find "$pkg" -type f; exit 1; }
done
[[ -n "$(find "$pkg/lib" -name 'libmathkit.a' | head -1)" ]] || {
    echo "package has no artifact under lib/<triple>/"; find "$pkg" -type f; exit 1; }

# Both lists are printed, and the implementation partition is on the right one.
grep -q 'Interface.*mathkit.cppm' pack.log || { cat pack.log; echo "no interface list"; exit 1; }
grep -q 'Withheld.*secret.cppm'   pack.log || { cat pack.log; echo "no withheld list"; exit 1; }

cd "$TMP"

# ── three consumers: header only, module only, both ────────────────────
consume() {   # $1 = name, $2 = main.cpp body
    mkdir -p "$1/src"
    printf '%s' "$2" > "$1/src/main.cpp"
    cat > "$1/mcpp.toml" <<EOF
[package]
name    = "$1"
version = "0.1.0"
[dependencies]
mathkit = { path = "$PKG_HOST" }
[targets.$1]
kind = "bin"
main = "src/main.cpp"
EOF
    ( cd "$1" && "$MCPP" run > run.log 2>&1 ) || { cat "$1/run.log"; echo "$1 failed"; exit 1; }
    grep -q 'ok=5' "$1/run.log" || { cat "$1/run.log"; echo "$1 printed the wrong answer"; exit 1; }
}

consume c_hdr '#include <cstdio>
#include <mathkit_c.h>
int main(){ std::printf("ok=%d\n", mathkit_add(2,3)); return 0; }
'
consume c_mod '#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::add(2,3)); return 0; }
'
consume c_both '#include <cstdio>
#include <mathkit_c.h>
import mathkit;
int main(){ std::printf("ok=%d\n", mathkit_add(2,3) + mk::add(2,3) - 5); return 0; }
'

echo "PASS: a library package serves header, module, and both-at-once consumers"
