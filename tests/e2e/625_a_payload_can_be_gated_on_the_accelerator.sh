#!/usr/bin/env bash
# requires: elf gcc
# A payload predicated on the ACCELERATOR is installed for a device build and
# not for a CPU-only one.
#
# `accelerator` was grouped with the five resolved layer keys (`c-abi`,
# `compiler`, ...) and refused in `[target.'cfg(...)'.xlings]` for their reason:
# a layer is answered by dependency resolution, which runs after provisioning.
# That reason does not hold for this one key. The accel is an INPUT -- `--accel`
# or `[build] accel` -- read before the first package is resolved.
#
# The cost of the old grouping was paid on every build of every project with a
# device island: the vendor toolkit is declared unconditionally or not at all,
# so a CPU-only build downloaded gigabytes for a device it was not compiling
# for. There was no third spelling.
#
# TWO LEGS AND THE SECOND IS THE POINT. A test that only checked the device leg
# would pass on an engine that installs the payload always.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# Same selection rule as 618: a payload mcpp does not install for its own
# reasons, or the criterion measures something that is present anyway.
TOOL=shaderc
TOOL_VERSION="2026.3"

mkdir -p p/src
cat > p/src/main.cpp <<'EOF'
int main() { return 0; }
EOF
cat > p/mcpp.toml <<EOF
[package]
name         = "gated"
version      = "0.1.0"
accelerators = ["vulkan"]
[targets.gated]
kind = "bin"
main = "src/main.cpp"

# The payload the DEVICE build needs, and only it.
[target.'cfg(accelerator = "vulkan")'.xlings.workspace]
"xim:$TOOL" = "$TOOL_VERSION"
EOF

export MCPP_HOME="$TMP/home"
mkdir -p "$MCPP_HOME"
store="$MCPP_HOME/registry/data/xpkgs/xim-x-$TOOL"

cd p

# ── leg 1: no accelerator named -- the payload must NOT be fetched ──────────
"$MCPP" build >cpu.log 2>&1 || { echo "FAIL: the CPU-only build was refused"; tail -20 cpu.log; exit 1; }
if [ -d "$store" ]; then
    echo "FAIL: a CPU-only build installed the device payload at $store"
    exit 1
fi
echo "ok: no accel -- the gated payload was not installed"

# ── leg 2: the accelerator named -- it must be ─────────────────────────────
"$MCPP" build --accel "vulkan1.2" >dev.log 2>&1 || { echo "FAIL: the device build was refused"; tail -20 dev.log; exit 1; }
[ -d "$store" ] || {
    echo "FAIL: the device build did not install the payload its predicate names"
    tail -20 dev.log
    exit 1
}
echo "ok: --accel vulkan1.2 -- the gated payload was installed"

echo "PASS: a payload can be gated on the accelerator, in both directions"
