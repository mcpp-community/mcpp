#!/usr/bin/env bash
# requires: unix-shell
# 746_macos_deployment_target_is_target_keyed_not_host_keyed.sh — mcpp#685.
#
# `macos_deployment_target` (and MACOSX_DEPLOYMENT_TARGET) used to reach the
# effective triple, the fingerprint and `-mmacosx-version-min` only through
# `platform::macos::deployment_target()`, which read `#if defined(__APPLE__)`
# — the platform mcpp ITSELF was built for. So `mcpp build --target
# aarch64-macos` on a Linux (or Windows) host silently ignored the manifest
# key and the env var: the deployment floor a project stated made no
# difference to the compiler flags, the output directory, or which BMI cache
# entry was reused. This runs the real code path — toolchain resolution,
# triple assembly, ninja-plan generation — end to end, on whatever host runs
# this suite.
#
# DOES NOT `# requires: llvm`. That token is granted by no CI shard in this
# repository (see 641's own header note and
# .agents/*/e2e-requires-llvm-never-runs-on-shards*), so a test that needs an
# LLVM payload probes for it itself and SKIPs cleanly when absent, the same
# pattern the mingw-cross / qemu-arm / android-ndk probes in run_all.sh use.
#
# NO macOS SDK IS NEEDED. `--configure-only` stops after writing
# compile_commands.json / build.ninja, before any header is opened — the
# refusal `toolchain list` reports for `aarch64-macos` on Linux ("needs the
# macOS SDK") is about locating the C library for a REAL compile, not about
# resolving the triple or the flags this test reads. An explicit
# `[target.aarch64-macos] toolchain = "llvm@<v>"` override is the escape
# hatch that reaches this without one (measured).
set -e

find_llvm() {
    local home="${MCPP_HOME:-$HOME/.mcpp}"
    for root in "$home/registry/data/xpkgs/xim-x-llvm" "$HOME/.xlings/data/xpkgs/xim-x-llvm"; do
        [ -d "$root" ] || continue
        # Highest version that actually ships a clang++ — a README-only stub
        # (see mcpp#687's 16.1.0 fixture) would otherwise match the glob and
        # resolve to a binary that is not there.
        for v in $(ls "$root" 2>/dev/null | sort -rV); do
            if [ -x "$root/$v/bin/clang++" ]; then
                echo "$v"
                return 0
            fi
        done
    done
    return 1
}

LLVM_VERSION="$(find_llvm || true)"
if [ -z "$LLVM_VERSION" ]; then
    echo "SKIP: no xim-x-llvm payload on this host to cross-resolve aarch64-macos with"
    exit 0
fi
echo "using llvm@$LLVM_VERSION"

t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail=0

mkdir -p "$t/src"
printf 'int main(){return 0;}\n' > "$t/src/main.cpp"

manifest() {  # deployment_target
    cat > "$t/mcpp.toml" <<EOF
[package]
name    = "deploytarget-probe"
version = "0.1.0"

[build]
macos_deployment_target = "$1"

[target.aarch64-macos]
toolchain = "llvm@$LLVM_VERSION"
EOF
}

configure() {  # extra env assignments go on the invocation itself
    ( cd "$t" && rm -rf target compile_commands.json \
      && MCPP_NO_AUTO_INSTALL=1 "$MCPP" build --target aarch64-macos --configure-only "$@" )
}

# ── 1. The manifest value reaches the effective triple and the flags ────────
manifest "11.0"
out=$(configure 2>&1) || { echo "FAIL: configure failed:"; echo "$out"; exit 1; }
ok1=1
echo "$out" | grep -qF -- 'aarch64-macos → arm64-apple-macos11.0' || {
    echo "FAIL: resolved triple did not carry the manifest's 11.0:"
    echo "$out"; fail=1; ok1=0
}
cc="$t/compile_commands.json"
[ -f "$cc" ] || { echo "FAIL: --configure-only produced no compile_commands.json"; fail=1; ok1=0; }
grep -qF -- '--target=arm64-apple-macos11.0' "$cc" || {
    echo "FAIL: --target=arm64-apple-macos11.0 absent from compile_commands.json"; fail=1; ok1=0; }
grep -qF -- '-mmacosx-version-min=11.0' "$cc" || {
    echo "FAIL: -mmacosx-version-min=11.0 absent from compile_commands.json"; fail=1; ok1=0; }
dir11=$(ls -d "$t"/target/aarch64-macos/*/ 2>/dev/null | head -1)
[ -n "$dir11" ] || { echo "FAIL: no target/aarch64-macos/<fp>/ directory written"; fail=1; ok1=0; }
[ "$ok1" = 1 ] && echo "  ok  manifest 11.0 -> arm64-apple-macos11.0, flags, and an output dir"

# ── 2. A different manifest value moves the flags AND the fingerprint ───────
manifest "12.0"
out=$(configure 2>&1) || { echo "FAIL: configure failed:"; echo "$out"; exit 1; }
echo "$out" | grep -qF -- 'aarch64-macos → arm64-apple-macos12.0' || {
    echo "FAIL: resolved triple did not move to 12.0:"; echo "$out"; fail=1; }
grep -qF -- '--target=arm64-apple-macos12.0' "$cc" || {
    echo "FAIL: --target=arm64-apple-macos12.0 absent after the manifest changed"; fail=1; }
dir12=$(ls -d "$t"/target/aarch64-macos/*/ 2>/dev/null | head -1)
if [ "$dir11" = "$dir12" ]; then
    echo "FAIL: output directory unchanged (was $dir11) after macos_deployment_target moved 11.0 -> 12.0"
    echo "       the fingerprint did not fold the new value — a std.pcm built for 11.0 would be replayed for 12.0"
    fail=1
else
    echo "  ok  manifest 12.0 -> arm64-apple-macos12.0, and a NEW output directory ($dir12 != $dir11)"
fi

# ── 3. MACOSX_DEPLOYMENT_TARGET overrides the manifest, same as cargo/cc ────
# manifest() above still writes "12.0"; the env value must win.
out=$(cd "$t" && rm -rf target compile_commands.json \
      && MCPP_NO_AUTO_INSTALL=1 MACOSX_DEPLOYMENT_TARGET=13.0 \
         "$MCPP" build --target aarch64-macos --configure-only 2>&1) \
    || { echo "FAIL: configure failed:"; echo "$out"; exit 1; }
ok3=1
echo "$out" | grep -qF -- 'aarch64-macos → arm64-apple-macos13.0' || {
    echo "FAIL: MACOSX_DEPLOYMENT_TARGET=13.0 did not beat manifest's 12.0:"
    echo "$out"; fail=1; ok3=0; }
grep -qF -- '--target=arm64-apple-macos13.0' "$cc" || {
    echo "FAIL: --target=arm64-apple-macos13.0 absent with the env override set"; fail=1; ok3=0; }
[ "$ok3" = 1 ] && echo "  ok  env MACOSX_DEPLOYMENT_TARGET=13.0 beats manifest's 12.0"

[ "$fail" = 0 ] || exit 1
echo "OK: macos_deployment_target resolves by TARGET, on a host that is not macOS"
