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

# Detect installed GNU gcc version for the default-build regression check.
# Without a known GNU toolchain the default build would auto-install
# musl-gcc and produce another musl binary, making the assertion meaningless.
XPKGS="${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs"
GNU_GCC_VER=""
if [[ -d "$XPKGS/xim-x-gcc" ]]; then
    GNU_GCC_VER=$(ls "$XPKGS/xim-x-gcc" 2>/dev/null | sort -V | tail -1)
fi

# Diagnostic: show what we detected
echo "DIAG: MCPP_HOME=${MCPP_HOME:-<unset>}"
echo "DIAG: XPKGS=$XPKGS"
echo "DIAG: GNU_GCC_VER=${GNU_GCC_VER:-<empty>}"
echo "DIAG: xim-x-gcc contents: $(ls "$XPKGS/xim-x-gcc" 2>&1 || echo 'N/A')"
echo "DIAG: config.toml toolchain line: $(grep -i toolchain "${MCPP_HOME:-$HOME/.mcpp}/config.toml" 2>/dev/null || echo 'none')"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
"$MCPP" new staticapp >/dev/null
cd staticapp

# Add a [target.x86_64-linux-musl] override that tells mcpp to use musl-gcc.
# If a GNU gcc is available, also pin [toolchain].linux so the default build
# uses it (preventing fallback to auto-install musl-gcc as default).
if [[ -n "$GNU_GCC_VER" ]]; then
cat >> mcpp.toml <<EOF

[toolchain]
linux = "gcc@${GNU_GCC_VER}"

[target.x86_64-linux-musl]
toolchain = "gcc@15.1.0-musl"
linkage   = "static"
EOF
else
cat >> mcpp.toml <<'EOF'

[target.x86_64-linux-musl]
toolchain = "gcc@15.1.0-musl"
linkage   = "static"
EOF
fi

echo "DIAG: mcpp.toml:"
cat mcpp.toml

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
# Skip this check if no GNU gcc is available — without it the default
# build would also use musl-gcc, making the assertion meaningless.
if [[ -n "$GNU_GCC_VER" ]]; then
    "$MCPP" build > build-gnu.log 2>&1 || {
        cat build-gnu.log; echo "default GNU build broke after musl build"; exit 1; }
    echo "DIAG: target/ tree after GNU build:"
    find target -type f -name staticapp 2>&1
    gnu_binary=$(find target -type d -name x86_64-linux-musl -prune -o -type f -name staticapp -print | head -1)
    [[ -n "$gnu_binary" ]] || { echo "default GNU binary missing"; exit 1; }
else
    echo "SKIP: no GNU gcc installed — skipping default-build regression check"
fi

echo "OK"
