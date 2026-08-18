#!/usr/bin/env bash
# requires: msvc
# 262_pack_consumed_by_native_cl.sh — a packaged library linked by NATIVE cl.exe.
#
# The generated manifest states each leg's link line twice. The first spelling is
# GNU — `-Llib/<triple> -l<name>` — which every compiler DRIVER mcpp uses
# accepts, including clang targeting the MSVC ABI, and which native `cl.exe`
# rejects at the first `-L`. The second is the dialect-neutral pair:
#
#   [target.'cfg(…, env = "msvc")'.runtime]
#   link_library_dirs = ["lib/x86_64-windows-msvc"]
#   libraries         = ["mathkit"]
#
# mcpp renders that as `/LIBPATH:` + `<name>.lib` when it invokes link.exe
# directly, and as `-L` + `-l<name>` for every compiler driver. Both spellings
# ship because an older mcpp reads only the first and silently ignores the
# second, so dropping the first would leave every older client with no link line.
#
# ⚠️ WHY BOTH HALVES ARE ASSERTED. The renderer is unit tested
# (tests/unit/test_link_intent_spelling.cpp) and only a real `cl.exe` can say
# whether the result links. But a consumer that merely SUCCEEDS proves less than
# it looks: if the neutral form were ignored and the ldflags applied instead,
# clang would still link it. So the consumer here pins `msvc@system`, which is
# the one toolchain that cannot survive a stray `-L`.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p mathkit/src
cat > mathkit/src/mathkit.cppm <<'EOF'
export module mathkit;
export namespace mk { int answer(); }
EOF
cat > mathkit/src/impl.cpp <<'EOF'
module mathkit;
namespace mk { int answer() { return 42; } }
EOF
cat > mathkit/mcpp.toml <<'EOF'
[package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
[toolchain]
windows = "msvc@system"
EOF

cd mathkit
"$MCPP" pack mathkit > pack.log 2>&1 || { cat pack.log; echo "pack failed"; exit 1; }
pkg="$TMP/mathkit/$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0-*' | head -1)"

# ── the package carries the neutral form ────────────────────────────────
grep -q "\.runtime\]" "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"
    echo "FAIL: no conditional [runtime] block. Without it a cl.exe consumer has"
    echo "      only the GNU ldflags, and cl stops at the first -L."
    exit 1; }
grep -q 'link_library_dirs' "$pkg/mcpp.toml" || { cat "$pkg/mcpp.toml"; echo "FAIL: no link_library_dirs"; exit 1; }
grep -q 'libraries' "$pkg/mcpp.toml"         || { cat "$pkg/mcpp.toml"; echo "FAIL: no libraries"; exit 1; }
# …and still the GNU one, for clients that predate the neutral form.
grep -q 'ldflags' "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"
    echo "FAIL: the ldflags are gone. An older mcpp reads only those, so removing"
    echo "      them leaves it with no link line at all."
    exit 1; }

# ── and native cl.exe links it ──────────────────────────────────────────
PKG_HOST="$(host_path "$pkg")"
cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("cl-ok=%d\n", mk::answer()); return 0; }
EOF
cat > app/mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = { path = "$PKG_HOST" }
[targets.app]
kind = "bin"
main = "src/main.cpp"
[toolchain]
windows = "msvc@system"
EOF
( cd app && "$MCPP" run > run.log 2>&1 ) || {
    cat app/run.log
    echo "FAIL: native cl.exe could not consume the package."
    echo "      A message naming '-L' means the GNU ldflags reached the command"
    echo "      line — the neutral form must REPLACE that leg's library"
    echo "      references, not be added alongside them."
    exit 1; }
grep -q 'cl-ok=42' app/run.log || { cat app/run.log; echo "FAIL: wrong answer"; exit 1; }

# ── the GNU spelling did not reach the command line ─────────────────────
#
# The run above could also pass if `-L` were accepted and ignored. Asserted
# against the generated graph so the claim is about what mcpp emitted.
nj="$(find app/target -name build.ninja | head -1)"
grep -E 'ldflags|unit_ldflags' "$nj" | grep -q -- '-Llib/' && {
    grep -n -- '-Llib/' "$nj" | head -3
    echo "FAIL: a GNU -L for the package leg is still on the link line. cl.exe"
    echo "      happens to be tolerant here only by accident."
    exit 1; }

echo "PASS: native cl.exe links a packaged library through the neutral link intent"
