#!/usr/bin/env bash
# requires: macos
# 259_shared_library_macho.sh — `kind = "shared"` on Mach-O: a `.dylib` that can
# be moved, packed, and loaded from beside the consumer's binary.
#
# WHY THE INSTALL NAME IS THE WHOLE TEST. A Mach-O shared library records the
# name it will be FOUND by, and the default is the path it was LINKED at. So a
# library built in `/private/var/folders/…/target/…/bin` records that path, and
# the moment it is packed and extracted somewhere else, every consumer of it
# fails at load time looking for a directory that no longer exists — on the
# publisher's machine it works perfectly.
#
# `@rpath/<file>` is the only default that survives being moved: it defers the
# question to the consumer, whose own `-Wl,-rpath,@loader_path` answers "next to
# me". mcpp used to emit `-install_name` only when the manifest declared a
# `soname`, and to decide Mach-O-ness with `#if defined(__APPLE__)` on the HOST
# rather than from the target.
#
# AND WHY IT ASSERTS THE PATH IS *GONE*. Checking that the consumer runs in
# place proves nothing: the absolute install name still resolves while the build
# directory exists. The package has to be consumed from a location the original
# build path cannot satisfy — so the library is packed, the producer's whole
# build tree is DELETED, and only then is the consumer built and run.
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
kind = "shared"
EOF

# ── 1. it builds, and it is a dylib ─────────────────────────────────────
cd mathkit
"$MCPP" build > build.log 2>&1 || { cat build.log; echo "build failed"; exit 1; }
dylib="$(find target -name 'libmathkit.dylib' | head -1)"
[[ -n "$dylib" ]] || { find target -type f | head; echo "FAIL: no .dylib"; exit 1; }

# ── 2. the install name is @rpath, not this machine's build path ─────────
#
# A functional probe, not `command -v otool`: on macOS a bare tool name can
# resolve to an xlings shim that reports "not installed" and exits non-zero,
# which under `set -e` would kill the test instead of skipping the inspection.
if install_name="$(otool -D "$dylib" 2>/dev/null | tail -1)"; then
    case "$install_name" in
        @rpath/libmathkit.dylib) ;;
        *)
            echo "FAIL: install name is '$install_name'"
            echo "      Anything absolute here is this machine's build directory, and the"
            echo "      library stops loading the moment it is extracted anywhere else."
            exit 1 ;;
    esac
else
    echo "NOTE: otool unavailable — the install name was not inspected directly."
    echo "      Step 4 still fails if it is wrong, just with a loader error."
fi

# ── 3. pack it ──────────────────────────────────────────────────────────
rm -rf target
"$MCPP" pack mathkit --format dir > pack.log 2>&1 || { cat pack.log; echo "pack failed"; exit 1; }
pkgrel="$(find target/dist -maxdepth 1 -type d -name 'mathkit-0.1.0*' | head -1)"
[[ -n "$pkgrel" ]] || { cat pack.log; echo "FAIL: no package"; exit 1; }
# OUT of the producer's tree, so the next step can delete that tree entirely.
pkg="$TMP/pkg"
cp -R "$TMP/mathkit/$pkgrel" "$pkg"
# `find`, not `[[ -f "$pkg/lib/"*"/libmathkit.dylib" ]]`: inside `[[ ]]` the `*`
# is not path-expanded, so that form tests a literal string containing an
# asterisk and is false for every real package — a check that can only fail.
[[ -n "$(find "$pkg/lib" -name 'libmathkit.dylib' | head -1)" ]] || {
    find "$pkg" \( -type f -o -type l \)
    echo "FAIL: no dylib in the package"; exit 1; }

# ── 4. the producer's build tree is deleted, then the consumer runs ──────
#
# This is what makes the install-name assertion above load-bearing rather than
# decorative: after this `rm -rf`, an absolute install name names nothing.
rm -rf "$TMP/mathkit"

cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'EOF'
#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
EOF
PKG_HOST="$(host_path "$pkg")"
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
( cd app && "$MCPP" run > run.log 2>&1 ) || {
    cat app/run.log
    echo "FAIL: the consumer could not build or run against the packaged dylib."
    echo "      'image not found' naming a path under /private/var means the"
    echo "      install name was the build directory after all."
    exit 1; }
grep -q 'ok=42' app/run.log || { cat app/run.log; echo "FAIL: wrong answer"; exit 1; }

echo "PASS: a Mach-O shared library relocates — @rpath install name, packed, run"
