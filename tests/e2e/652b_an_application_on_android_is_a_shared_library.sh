#!/usr/bin/env bash
# requires: android-ndk
# 652b_an_application_on_android_is_a_shared_library.sh -- the row half of
# `kind = "app"` (#622 A3, design record §2.3): on `*-linux-android` an
# application's link form is `SharedObject`, not `Executable`. Split from 652
# because it needs the NDK payload (`xim:android-ndk`) rather than the host
# ELF/GCC toolchain, and a script that needed it would skip on every runner
# that lacks the ~700 MB payload -- this repository has paid for that mistake
# once already (`# requires: llvm` ran on no CI shard while reporting green).
#
# `run_all.sh` gates this on the `android-ndk` capability, detected the same
# way `qemu-arm`/`nasm`/`scan-deps` are: a payload path probe, not a PATH
# probe, because a distro-supplied Android tool would answer the wrong
# question. `MCPP_NO_AUTO_INSTALL` is left UNSET on purpose: the point of this
# script is that the row's own toolchain resolution reaches the payload
# already on this machine, exactly as an end user's first `--target
# x86_64-linux-android` build would.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

TARGET=x86_64-linux-android

cd "$TMP"
mkdir -p src
cat > mcpp.toml <<'TOML'
[package]
name    = "myapp"
version = "0.1.0"

[targets.myapp]
kind = "app"
main = "src/main.cpp"

[targets.tool]
kind = "bin"
main = "src/tool.cpp"
TOML
cat > src/main.cpp <<'CPP'
int main() { return 0; }
CPP
cat > src/tool.cpp <<'CPP'
int main() { return 0; }
CPP

"$MCPP" build --target "$TARGET" > build.log 2>&1 \
    || fail "the Android build failed" build.log
outdir=$(dirname "$(ls -t target/$TARGET/*/build.ninja | head -1)")

# ── 1. `app` on Android links as `shared` does: lib<name>.so, not <name> ───
[ -f "$outdir/bin/libmyapp.so" ] \
    || fail "libmyapp.so was not linked" build.log
[ ! -e "$outdir/bin/myapp" ] \
    || fail "bin/myapp exists -- an Android app must have NO executable form" build.log
file "$outdir/bin/libmyapp.so" | grep -qi "shared object" \
    || fail "bin/libmyapp.so is not a shared object" build.log
echo "app links as lib<name>.so on Android OK"

# ── 2. `bin` keeps meaning binary: `tool` links as an ordinary executable ──
[ -f "$outdir/bin/tool" ] || fail "bin/tool was not linked" build.log
file "$outdir/bin/tool" | grep -qi "pie executable" \
    || fail "bin/tool is not an executable (bin must be unaffected by A3)" build.log
echo "bin keeps meaning binary on Android OK"

# ── 3. `mcpp run` of the app, with no --format, is refused naming it ───────
out=$("$MCPP" run myapp --target "$TARGET" 2>&1) \
    && fail "mcpp run of an app whose form is a library must be refused" <(echo "$out")
expected="error: 'myapp' is an application, and on $TARGET an application is a shared library that a package installs. Run it through a distributable: mcpp run --format <name>, where <name> is one of: none declared"
grep -qF "$expected" <<<"$out" \
    || fail "the refusal sentence does not match the design record's exact wording" <(echo "$out")
echo "mcpp run of the app without --format is refused, naming --format OK"

# Negative-direction control for (3): `tool` is a real Binary on this same
# row, so it is not caught by the app-only refusal above.
out=$("$MCPP" run tool --target "$TARGET" --no-runner 2>&1) || true
if grep -q "is an application" <<<"$out"; then
    fail "the app refusal fired for a plain bin target" <(echo "$out")
fi
echo "the refusal does not fire for a bin target OK"

echo "652b: kind = \"app\" on Android is a shared library OK"
