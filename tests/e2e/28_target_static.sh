#!/usr/bin/env bash
# 28_target_static.sh — `mcpp build --target <triple>` produces a binary
# matching the requested target, and `--target *-linux-musl` yields a
# fully-static ELF (no PT_INTERP, no RUNPATH).
#
# Skip if the host doesn't have musl-gcc available — we don't auto-install
# it in this test (would be a 200 MB download per CI run).
set -e

# musl-gcc lives in the workspace toolchain (.xlings.json) once
# `xlings install -y` is run. CI does this; locally the developer has it
# from the `.xlings.json` workspace install too.
if ! command -v x86_64-linux-musl-g++ >/dev/null 2>&1 \
   && ! [ -x "$HOME/.xlings/data/xpkgs/xim-x-musl-gcc/15.1.0/bin/x86_64-linux-musl-g++" ] \
   && ! [ -x "$HOME/.mcpp/registry/data/xpkgs/xim-x-musl-gcc/15.1.0/bin/x86_64-linux-musl-g++" ]
then
    echo "SKIP: no musl-gcc 15.1.0 available — not testing --target=*-linux-musl"
    echo "OK"
    exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new staticapp >/dev/null
cd staticapp

# Add a [target.x86_64-linux-musl] override that tells mcpp to use musl-gcc.
cat >> mcpp.toml <<'EOF'

[target.x86_64-linux-musl]
toolchain = "gcc@15.1.0-musl"
linkage   = "static"
EOF

"$MCPP" build --target x86_64-linux-musl > build.log 2>&1 || {
    cat build.log; echo "build failed"; exit 1; }

# Output path includes the triple — `target/x86_64-linux-musl/<fp>/bin/staticapp`.
binary=$(find target/x86_64-linux-musl -type f -name staticapp | head -1)
[[ -n "$binary" ]] || { echo "binary not produced under target/x86_64-linux-musl"; ls -R target; exit 1; }

# `file` should report a statically-linked ELF.
file "$binary" | grep -q 'statically linked' || {
    file "$binary"; echo "binary is not statically linked"; exit 1; }

# Sanity-check it runs.
"$binary" >/dev/null 2>&1 || { echo "static binary failed to run"; exit 1; }

# Defensive: no PT_INTERP and no RUNPATH should be present.
if readelf -l "$binary" 2>/dev/null | grep -q 'INTERP'; then
    echo "static binary unexpectedly has PT_INTERP"; readelf -l "$binary" | grep -A1 INTERP; exit 1
fi
if readelf -d "$binary" 2>/dev/null | grep -qE 'RUNPATH|RPATH'; then
    echo "static binary unexpectedly has RUNPATH/RPATH"; readelf -d "$binary" | grep -E 'RUNPATH|RPATH'; exit 1
fi

# Default GNU build still works (regression: --target should not have
# clobbered the default codepath for follow-up commands).
"$MCPP" build > build-gnu.log 2>&1 || {
    cat build-gnu.log; echo "default GNU build broke after musl build"; exit 1; }
gnu_binary=$(find target -type d -name x86_64-linux-musl -prune -o -type f -name staticapp -print | head -1)
[[ -n "$gnu_binary" ]] || { echo "default GNU binary missing"; exit 1; }

echo "OK"
