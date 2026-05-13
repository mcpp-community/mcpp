#!/usr/bin/env bash
# 30_pack_modes.sh — `mcpp pack` smoke tests for all three modes.
#
# Verifies the contract of each mode by extracting the produced tarball
# and inspecting the resulting binary + (for non-static) bundled libs.
#
# Skipped when musl-gcc isn't installed (Mode A needs it). The default
# Mode C path runs unconditionally.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
export MCPP_HOME=$HOME/.mcpp

cd "$TMP"
"$MCPP" new myapp > /dev/null
cd myapp
# Keep Mode C independent from whatever global default toolchain earlier
# e2e tests wrote into ~/.mcpp/config.toml.
cat >> mcpp.toml <<'EOF'

[toolchain]
linux = "gcc@16.1.0"
EOF

# ─── Mode C (default = bundle-project) ─────────────────────────────────
"$MCPP" pack > "$TMP/pack-c.log" 2>&1 || {
    cat "$TMP/pack-c.log"; echo "Mode C pack failed"; exit 1; }

tarball="target/dist/myapp-0.1.0-x86_64-linux-gnu.tar.gz"
[[ -f "$tarball" ]] || { echo "Mode C tarball not produced at $tarball"; exit 1; }

# Extract + verify binary + RUNPATH/INTERP rewrite + runs from new location.
mkdir -p "$TMP/c"
tar -xzf "$tarball" -C "$TMP/c"
[[ -x "$TMP/c/myapp-0.1.0-x86_64-linux-gnu/bin/myapp" ]] || { echo "missing bin/myapp"; exit 1; }

# Mode C: PT_INTERP must be repointed to the LSB-standard path. Use
# `file` rather than `readelf` because some sandboxes route readelf
# through an xvm shim that needs a per-tool version pin we can't rely
# on in CI.
file_out=$(file "$TMP/c/myapp-0.1.0-x86_64-linux-gnu/bin/myapp")
echo "$file_out" | grep -q 'interpreter /lib64/ld-linux-x86-64.so.2' || {
    echo "Mode C: PT_INTERP not repointed to /lib64/ld-linux-x86-64.so.2"
    echo "$file_out"
    exit 1
}

# Mode C with no third-party deps: lib/ should be empty (or absent).
if [[ -d "$TMP/c/myapp-0.1.0-x86_64-linux-gnu/lib" ]]; then
    extra=$(ls "$TMP/c/myapp-0.1.0-x86_64-linux-gnu/lib" 2>/dev/null | wc -l)
    [[ "$extra" -eq 0 ]] || {
        echo "Mode C: project has no third-party deps but lib/ has $extra entries:"
        ls "$TMP/c/myapp-0.1.0-x86_64-linux-gnu/lib"; exit 1; }
fi

# Run from new location. Should work because PT_INTERP is system-standard.
"$TMP/c/myapp-0.1.0-x86_64-linux-gnu/bin/myapp" > "$TMP/c-out.log" 2>&1 || {
    cat "$TMP/c-out.log"; echo "Mode C extracted binary failed to run"; exit 1; }
grep -q 'Hello' "$TMP/c-out.log" || {
    cat "$TMP/c-out.log"; echo "Mode C runtime output missing"; exit 1; }

# Mode C should also drop a top-level <name> entry-point script so users
# can do `./myapp` from the bundle root without typing bin/.
[[ -x "$TMP/c/myapp-0.1.0-x86_64-linux-gnu/myapp" ]] || {
    ls "$TMP/c/myapp-0.1.0-x86_64-linux-gnu"; echo "Mode C: missing top-level <name> entry"; exit 1; }
"$TMP/c/myapp-0.1.0-x86_64-linux-gnu/myapp" > "$TMP/c-name.log" 2>&1 || {
    cat "$TMP/c-name.log"; echo "Mode C: top-level entry failed to run"; exit 1; }
grep -q 'Hello' "$TMP/c-name.log" || {
    cat "$TMP/c-name.log"; echo "Mode C: top-level entry output missing"; exit 1; }

# ─── Mode A (static, musl) ─────────────────────────────────────────────
musl_gcc="$HOME/.mcpp/registry/data/xpkgs/xim-x-musl-gcc/15.1.0/bin/x86_64-linux-musl-g++"
if [[ -x "$musl_gcc" ]]; then
    "$MCPP" pack --mode static > "$TMP/pack-a.log" 2>&1 || {
        cat "$TMP/pack-a.log"; echo "Mode A pack failed"; exit 1; }

    tarball_a="target/dist/myapp-0.1.0-x86_64-linux-musl-static.tar.gz"
    [[ -f "$tarball_a" ]] || { echo "Mode A tarball missing at $tarball_a"; exit 1; }

    mkdir -p "$TMP/a"
    tar -xzf "$tarball_a" -C "$TMP/a"
    [[ -x "$TMP/a/myapp-0.1.0-x86_64-linux-musl-static/bin/myapp" ]] \
        || { echo "Mode A: missing bin/myapp"; exit 1; }
    file "$TMP/a/myapp-0.1.0-x86_64-linux-musl-static/bin/myapp" | grep -q 'statically linked' || {
        file "$TMP/a/myapp-0.1.0-x86_64-linux-musl-static/bin/myapp"
        echo "Mode A: not statically linked"
        exit 1
    }
    "$TMP/a/myapp-0.1.0-x86_64-linux-musl-static/bin/myapp" > "$TMP/a-out.log" 2>&1 || {
        cat "$TMP/a-out.log"; echo "Mode A extracted binary failed to run"; exit 1; }
    grep -q 'Hello' "$TMP/a-out.log" || { echo "Mode A runtime output missing"; exit 1; }

    # Mode A also gets a top-level <name> entry-point wrapper.
    [[ -x "$TMP/a/myapp-0.1.0-x86_64-linux-musl-static/myapp" ]] || {
        echo "Mode A: missing top-level <name> entry"; exit 1; }
    "$TMP/a/myapp-0.1.0-x86_64-linux-musl-static/myapp" > "$TMP/a-name.log" 2>&1 || {
        echo "Mode A: top-level entry failed to run"; exit 1; }
    grep -q 'Hello' "$TMP/a-name.log" || {
        echo "Mode A: top-level entry output missing"; exit 1; }
else
    echo "(skip Mode A: musl-gcc not installed)"
fi

# ─── Mode B (bundle-all + wrapper) ─────────────────────────────────────
"$MCPP" pack --mode bundle-all > "$TMP/pack-b.log" 2>&1 || {
    cat "$TMP/pack-b.log"; echo "Mode B pack failed"; exit 1; }

tarball_b="target/dist/myapp-0.1.0-x86_64-linux-gnu-bundle-all.tar.gz"
[[ -f "$tarball_b" ]] || { echo "Mode B tarball missing at $tarball_b"; exit 1; }

mkdir -p "$TMP/b"
tar -xzf "$tarball_b" -C "$TMP/b"
[[ -x "$TMP/b/myapp-0.1.0-x86_64-linux-gnu-bundle-all/bin/myapp" ]] || { echo "Mode B: missing bin/myapp"; exit 1; }
[[ -x "$TMP/b/myapp-0.1.0-x86_64-linux-gnu-bundle-all/run.sh"      ]] || { echo "Mode B: missing run.sh wrapper"; exit 1; }
[[ -d "$TMP/b/myapp-0.1.0-x86_64-linux-gnu-bundle-all/lib"         ]] || { echo "Mode B: missing lib/"; exit 1; }

# Mode B must include the dynamic linker + libc — those are the libs the
# wrapper relies on to detach from the host's glibc.
ls "$TMP/b/myapp-0.1.0-x86_64-linux-gnu-bundle-all/lib" | grep -q 'ld-linux' \
    || { echo "Mode B: ld-linux not bundled"; exit 1; }
ls "$TMP/b/myapp-0.1.0-x86_64-linux-gnu-bundle-all/lib" | grep -q '^libc\.so' \
    || { echo "Mode B: libc.so not bundled"; exit 1; }

# Wrapper script must run via both run.sh AND the program-name entry.
[[ -x "$TMP/b/myapp-0.1.0-x86_64-linux-gnu-bundle-all/myapp" ]] || {
    ls "$TMP/b"
    echo "Mode B: missing top-level <name> entry alongside run.sh"
    exit 1
}
"$TMP/b/myapp-0.1.0-x86_64-linux-gnu-bundle-all/run.sh" > "$TMP/b-out.log" 2>&1 || {
    cat "$TMP/b-out.log"; echo "Mode B run.sh failed"; exit 1; }
grep -q 'Hello' "$TMP/b-out.log" || {
    cat "$TMP/b-out.log"; echo "Mode B runtime output missing"; exit 1; }
"$TMP/b/myapp-0.1.0-x86_64-linux-gnu-bundle-all/myapp" > "$TMP/b-name.log" 2>&1 || {
    cat "$TMP/b-name.log"; echo "Mode B <name> entry failed"; exit 1; }
grep -q 'Hello' "$TMP/b-name.log" || {
    cat "$TMP/b-name.log"; echo "Mode B <name>-entry output missing"; exit 1; }

echo "OK"
