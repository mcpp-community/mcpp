#!/usr/bin/env bash
# requires: macos
# 721 -- an Android row links on a macOS host (#647 E3).
#
# The link line used to be chosen by the HOST: on macOS every target received
# the Apple SDK line, which carries `-isysroot <MacOSX.sdk>` and
# `-mmacosx-version-min` and no `--target`. The NDK's clang then linked an
# Android object as the host, `-fuse-ld=lld` selected `ld64.lld`, and the link
# failed with
#
#     ld64.lld: error: unknown argument '-soname'
#
# Measured on macos-15 (mcpp-plugins run 34968851641). The line is now chosen
# by the host AND the target's object format (`link_shape`, unit
# `LinkShape.*`), so an ELF target takes the line that names its target.
#
# The NDK (`xim:android-ndk`) is installed on first use when this machine does
# not have it; on a runner without network access the leg reports that it
# could not install rather than passing.
#
# Legs:
#   A. `mcpp build --target x86_64-linux-android` for a `kind = "app"` target
#      over a shared dependency succeeds, and both `libapp.so` and `libdep.so`
#      are ELF shared objects for x86-64.
#   B. The build log names no `ld64.lld`, no `-mmacosx-version-min` and no
#      `MacOSX.sdk` on the Android link.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; tail -60 "$f" 2>/dev/null; done; exit 1; }

mkdir -p dep/src app/src
cat > dep/mcpp.toml <<'EOF'
[package]
name     = "dep"
version  = "0.1.0"
standard = "c++20"

[targets.dep]
kind = "shared"

[build]
sources = ["src/*.cpp"]
EOF
printf '[[gnu::visibility("default")]] int dep_answer() { return 42; }\n' > dep/src/dep.cpp

cat > app/mcpp.toml <<'EOF'
[package]
name     = "app"
version  = "0.1.0"
standard = "c++20"

[dependencies]
dep = { path = "../dep" }

[targets.app]
kind = "app"
main = "src/main.cpp"

[target.x86_64-linux-android]
min_api_level = 24
EOF
cat > app/src/main.cpp <<'EOF'
int dep_answer();
[[gnu::visibility("default")]] int app_entry() { return dep_answer() + 1; }
int main() { return app_entry() == 43 ? 0 : 1; }
EOF

cd app
# ── A ───────────────────────────────────────────────────────────────────────
if ! "$MCPP" build --target x86_64-linux-android --verbose > a.log 2>&1; then
    if grep -qi "cannot be downloaded\|download.*failed\|not installed" a.log \
       && ! grep -q "ld64.lld\|unknown argument '-soname'" a.log; then
        fail "A: the NDK could not be installed on this runner" a.log
    fi
    fail "A: the Android row did not link on a macOS host" a.log
fi
app=$(find target -name libapp.so -path '*/bin/*' | head -1)
dep=$(find target -name libdep.so -path '*/bin/*' | head -1)
[ -n "$app" ] && [ -n "$dep" ] || fail "A: no libapp.so / libdep.so" a.log
for f in "$app" "$dep"; do
    kind=$(file -b "$f")
    echo "reading A: $(basename "$f"): $kind"
    case "$kind" in *ELF*"shared object"*x86-64*) ;; *) fail "A: $(basename "$f") is not an x86-64 ELF shared object: $kind" a.log ;; esac
done
echo "ok: A, the Android row links on a macOS host"

# ── B ───────────────────────────────────────────────────────────────────────
if grep -q "ld64.lld" a.log; then fail "B: ld64.lld appears in the build" a.log; fi
graph=$(ls target/x86_64-linux-android/*/build.ninja 2>/dev/null | head -1 || true)
[ -n "$graph" ] || fail "B: no build graph for the Android row" a.log
# The link line only: `ldflags` is what the host branch assembled.
if grep -E "^[[:space:]]*ldflags[[:space:]]*=" "$graph" | grep -q "mmacosx-version-min\|MacOSX.sdk"; then
    grep -nE "^[[:space:]]*ldflags[[:space:]]*=" "$graph" | head -3
    fail "B: the Android link line carries the macOS SDK or deployment floor" a.log
fi
echo "ok: B, the Android link carries no Apple SDK token"

echo "PASS: 721"
