#!/usr/bin/env bash
# requires: gcc musl
# 245_pack_library_fat_target_selection.sh — one package, several targets, and
# each build picks exactly its own leg.
#
# `musl` is now declared as well as `gcc`. It always needed both — the pack below
# asks for a musl leg outright — and CI was green only because the payload
# happens to be warm there. A requirement the test does not declare is one that
# turns into a confusing failure the day it stops being true.
#
# The Windows counterpart (msvc + mingw legs in one package) is 256; `# requires:`
# cannot express "gcc+musl OR msvc+mingw", and a host that has neither should
# skip rather than half-run.
#
# THE PREDICATE IS THE POINT. Each leg is selected by a generated
# `cfg(all(arch=…, os=…, env=…))` block and NOT by a bare `[target.'<triple>']`
# key. The bare form only matched when `--target` was passed: a plain
# `mcpp build` resolved the host and compared it against an empty string, so
# the section was silently inert. That shape is the worst kind — CI passes
# `--target` and is green, the developer's own build drops the flags, and the
# failure arrives at the linker naming a symbol instead of a predicate.
#
# So this test builds the NATIVE case as well as the explicit ones, and asserts
# the selected directory each time.
#
# gnu + musl and not gnu + windows: CI warms both of those toolchains, so this
# test RUNS rather than skipping. A `# requires: mingw-cross` here would have
# made the whole fat-package mechanism unverified on every ordinary CI run
# while still reporting a green suite. The PE leg is covered by 248.
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
EOF

cd mathkit
"$MCPP" pack mathkit \
    --target x86_64-linux-gnu \
    --target x86_64-linux-musl > pack.log 2>&1 \
    || { cat pack.log; echo "fat pack failed"; exit 1; }

pkg="$TMP/mathkit/target/dist/mathkit-0.1.0"
# The manifest below is FILE CONTENT: on Git Bash a shell-spelled
# /tmp/... path is read by a native mcpp.exe as "root of the current
# drive". host_path is the conversion (tests/e2e/_host_path.sh).
PKG_HOST="$(host_path "$pkg")"
for t in x86_64-linux-gnu x86_64-linux-musl; do
    [[ -n "$(find "$pkg/lib/$t" -name 'libmathkit.a' | head -1)" ]] || {
        echo "no artifact for $t"; find "$pkg" -type f; exit 1; }
done

# The generated blocks must be cfg(...), never a bare triple.
grep -q "target\.'cfg(" "$pkg/mcpp.toml" || {
    cat "$pkg/mcpp.toml"; echo "legs are not selected by cfg() predicates"; exit 1; }
grep -qE "^\[target\.'x86_64-" "$pkg/mcpp.toml" && {
    cat "$pkg/mcpp.toml"
    echo "a leg is selected by a BARE TRIPLE, which is inert on a native build"
    exit 1; }

cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
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
EOF

check() {   # $1 = label, $2 = expected leg dir, $3.. = build args
    local label="$1" want="$2"; shift 2
    rm -rf app/target
    ( cd app && "$MCPP" build "$@" > "$TMP/$label.log" 2>&1 ) \
        || { cat "$TMP/$label.log"; echo "$label build failed"; exit 1; }
    local nj; nj="$(find app/target -name build.ninja | head -1)"
    # Separator-agnostic and anchored on the PACKAGE name: a native mcpp.exe
    # writes native separators into build.ninja, so a `dist/…/lib/…` pattern
    # matches nothing there and the failure reads like a packaging bug. Same
    # helper as 256, which is where that was measured.
    grep -oE "mathkit-0\.1\.0[\\/]lib[\\/][A-Za-z0-9_-]+" "$nj" \
        | sed 's|.*[\\/]||' | sort -u > "$TMP/$label.legs"
    [[ "$(wc -l < "$TMP/$label.legs")" -eq 1 ]] || {
        echo "$label saw $(wc -l < "$TMP/$label.legs") leg(s), expected exactly 1:"
        cat "$TMP/$label.legs"; exit 1; }
    grep -qx "$want" "$TMP/$label.legs" || {
        echo "$label picked the wrong leg:"; cat "$TMP/$label.legs"; exit 1; }
}

# The native build is the one the bare-triple form used to get wrong.
check native x86_64-linux-gnu
check gnu    x86_64-linux-gnu    --target x86_64-linux-gnu
check musl   x86_64-linux-musl   --target x86_64-linux-musl

echo "PASS: a fat package selects one leg per target, native build included"
