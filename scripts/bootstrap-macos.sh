#!/usr/bin/env bash
# scripts/bootstrap-macos.sh — one-shot bootstrap of mcpp on macOS.
#
# This script compiles mcpp from source on macOS using upstream LLVM/Clang.
# It is only needed ONCE to produce the first macOS binary; afterwards
# mcpp can self-host via `mcpp build`.
#
# Prerequisites:
#   - Clang 20+ with libc++ module support (xlings LLVM or Homebrew LLVM)
#   - ninja (brew install ninja / xlings install ninja)
#   - macOS SDK (xcode-select --install)
#
# Usage:
#   ./scripts/bootstrap-macos.sh [LLVM_ROOT]
#
# Output:
#   target/bootstrap/bin/mcpp
#
set -euo pipefail

# ─── Locate LLVM ────────────────────────────────────────────────────────────

if [ -n "${1:-}" ] && [ -d "$1/bin" ]; then
    LLVM_ROOT="$1"
elif [ -n "${LLVM_ROOT:-}" ]; then
    : # already set via env
elif [ -d "$HOME/.xlings/data/xpkgs/xim-x-llvm" ]; then
    LLVM_ROOT=$(find "$HOME/.xlings/data/xpkgs/xim-x-llvm" -maxdepth 1 -type d | sort -V | tail -1)
elif command -v brew >/dev/null 2>&1 && [ -d "$(brew --prefix llvm)/bin" ]; then
    LLVM_ROOT=$(brew --prefix llvm)
else
    echo "error: cannot find LLVM. Pass LLVM_ROOT or install via xlings/Homebrew." >&2
    exit 1
fi

CXX="$LLVM_ROOT/bin/clang++"
AR="$LLVM_ROOT/bin/llvm-ar"
SCAN_DEPS="$LLVM_ROOT/bin/clang-scan-deps"

echo ":: LLVM_ROOT = $LLVM_ROOT"
echo ":: CXX       = $CXX"
"$CXX" --version | head -1

# ─── Locate macOS SDK ────────────────────────────────────────────────────────

SDKROOT=$(xcrun --show-sdk-path 2>/dev/null || true)
if [ -z "$SDKROOT" ]; then
    echo "error: macOS SDK not found. Run: xcode-select --install" >&2
    exit 1
fi
echo ":: SDKROOT   = $SDKROOT"

# ─── Locate std.cppm ────────────────────────────────────────────────────────

STD_CPPM=$("$CXX" -print-library-module-manifest-path 2>/dev/null || true)
if [ -n "$STD_CPPM" ] && [ -f "$STD_CPPM" ]; then
    # Parse the manifest JSON to find std source
    STD_CPPM=$(python3 -c "
import json, sys, os
m = json.load(open('$STD_CPPM'))
for mod in m.get('modules', []):
    if mod.get('logical-name') == 'std':
        p = mod['source-path']
        if not os.path.isabs(p):
            p = os.path.join(os.path.dirname('$STD_CPPM'), p)
        print(os.path.realpath(p))
        break
")
fi
if [ -z "$STD_CPPM" ] || [ ! -f "$STD_CPPM" ]; then
    STD_CPPM=$(find "$LLVM_ROOT" -name "std.cppm" -path "*/libc++/*" | head -1)
fi
if [ -z "$STD_CPPM" ] || [ ! -f "$STD_CPPM" ]; then
    echo "error: std.cppm not found in LLVM installation" >&2
    exit 1
fi
echo ":: std.cppm  = $STD_CPPM"

STD_COMPAT_CPPM=$(find "$LLVM_ROOT" -name "std.compat.cppm" -path "*/libc++/*" | head -1)
echo ":: std.compat= ${STD_COMPAT_CPPM:-(not found)}"

# ─── Setup output directory ─────────────────────────────────────────────────

PROJROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$PROJROOT/target/bootstrap"
PCMDIR="$OUTDIR/pcm.cache"
OBJDIR="$OUTDIR/obj"
BINDIR="$OUTDIR/bin"
mkdir -p "$PCMDIR" "$OBJDIR" "$BINDIR"

# Common flags
CXXFLAGS="-std=c++23 -O2 -I$PROJROOT/src/libs/json"
# If clang++.cfg doesn't already set sysroot, add it explicitly
if ! "$CXX" -v 2>&1 | grep -q "sysroot"; then
    CXXFLAGS="$CXXFLAGS --sysroot=$SDKROOT"
fi

echo
echo ":: Phase 1: Pre-compile std module"
"$CXX" $CXXFLAGS -Wno-reserved-module-identifier \
    --precompile "$STD_CPPM" -o "$PCMDIR/std.pcm"
"$CXX" $CXXFLAGS -Wno-reserved-module-identifier \
    "$PCMDIR/std.pcm" -c -o "$OBJDIR/std.o"

if [ -n "$STD_COMPAT_CPPM" ] && [ -f "$STD_COMPAT_CPPM" ]; then
    echo ":: Phase 1b: Pre-compile std.compat module"
    "$CXX" $CXXFLAGS -Wno-reserved-module-identifier \
        -fmodule-file=std="$PCMDIR/std.pcm" \
        --precompile "$STD_COMPAT_CPPM" -o "$PCMDIR/std.compat.pcm"
    "$CXX" $CXXFLAGS -Wno-reserved-module-identifier \
        -fmodule-file=std="$PCMDIR/std.pcm" \
        "$PCMDIR/std.compat.pcm" -c -o "$OBJDIR/std.compat.o"
fi

echo
echo ":: Phase 2: Scan module dependencies (P1689)"
# Generate compilation database for clang-scan-deps
SOURCES=()
while IFS= read -r f; do
    SOURCES+=("$f")
done < <(find "$PROJROOT/src" -name "*.cppm" -o -name "*.cpp" | sort)

# Create a compilation database
CDB="$OUTDIR/compile_commands.json"
echo "[" > "$CDB"
first=true
for src in "${SOURCES[@]}"; do
    if [ "$first" = true ]; then first=false; else echo "," >> "$CDB"; fi
    cat >> "$CDB" << ENTRY
  {
    "directory": "$OUTDIR",
    "file": "$src",
    "command": "$CXX $CXXFLAGS -fmodule-file=std=$PCMDIR/std.pcm -c $src -o /dev/null"
  }
ENTRY
done
echo "]" >> "$CDB"

# Run clang-scan-deps to get module dependency graph in P1689 format
P1689_FILE="$OUTDIR/p1689.json"
"$SCAN_DEPS" -compilation-database "$CDB" -format=p1689 > "$P1689_FILE" 2>/dev/null || {
    echo "warning: clang-scan-deps failed, trying alternative scan..."
    # Fallback: scan one-by-one
    echo '{"revision":0,"rules":[' > "$P1689_FILE"
    first=true
    for src in "${SOURCES[@]}"; do
        result=$("$SCAN_DEPS" -compilation-database "$CDB" -format=p1689 -- "$CXX" $CXXFLAGS -fmodule-file=std="$PCMDIR/std.pcm" -c "$src" 2>/dev/null || true)
        if [ -n "$result" ]; then
            if [ "$first" = true ]; then first=false; else echo "," >> "$P1689_FILE"; fi
            echo "$result" >> "$P1689_FILE"
        fi
    done
    echo "]}" >> "$P1689_FILE"
}

echo
echo ":: Phase 3: Topological sort and compile modules"
# Use Python to parse P1689 and build in dependency order
python3 << 'PYTHON'
import json, os, subprocess, sys

outdir = os.environ.get("OUTDIR", "target/bootstrap")
pcmdir = os.path.join(outdir, "pcm.cache")
objdir = os.path.join(outdir, "obj")
projroot = os.environ.get("PROJROOT", ".")
cxx = os.environ.get("CXX", "clang++")
cxxflags = os.environ.get("CXXFLAGS", "-std=c++23 -O2")
p1689_file = os.path.join(outdir, "p1689.json")

# Parse P1689
with open(p1689_file) as f:
    data = json.load(f)

rules = data.get("rules", [])

# Build module map: module_name -> source file
mod_to_src = {}
src_to_provides = {}
src_to_requires = {}

for rule in rules:
    src = rule.get("primary-output", rule.get("source", ""))
    # clang-scan-deps may use "primary-output" or find source from provides
    if not src:
        continue

    provides = []
    for p in rule.get("provides", []):
        name = p.get("logical-name", "")
        if name:
            provides.append(name)
            mod_to_src[name] = rule.get("source", src)

    requires = []
    for r in rule.get("requires", []):
        name = r.get("logical-name", "")
        if name and name != "std" and name != "std.compat":
            requires.append(name)

    actual_src = rule.get("source", src)
    src_to_provides[actual_src] = provides
    src_to_requires[actual_src] = requires

# Topological sort
visited = set()
order = []

def visit(mod_name):
    if mod_name in visited:
        return
    visited.add(mod_name)
    src = mod_to_src.get(mod_name)
    if src:
        for dep in src_to_requires.get(src, []):
            visit(dep)
    order.append(mod_name)

for mod_name in mod_to_src:
    visit(mod_name)

print(f"Module build order ({len(order)} modules):")
for m in order:
    print(f"  {m}")

# Compile modules in order
compiled_objs = []
mod_flags = f"-fmodule-file=std={pcmdir}/std.pcm"
if os.path.exists(os.path.join(pcmdir, "std.compat.pcm")):
    mod_flags += f" -fmodule-file=std.compat={pcmdir}/std.compat.pcm"

for mod_name in order:
    src = mod_to_src[mod_name]
    pcm_path = os.path.join(pcmdir, f"{mod_name.replace('.', '_')}.pcm")
    obj_path = os.path.join(objdir, f"{mod_name.replace('.', '_')}.o")

    # Build -fmodule-file flags for dependencies
    dep_flags = mod_flags
    for dep in src_to_requires.get(src, []):
        dep_pcm = os.path.join(pcmdir, f"{dep.replace('.', '_')}.pcm")
        dep_flags += f" -fmodule-file={dep}={dep_pcm}"

    # Precompile
    cmd = f"{cxx} {cxxflags} {dep_flags} --precompile {src} -o {pcm_path}"
    print(f"  PRECOMPILE {mod_name}")
    rc = os.system(cmd)
    if rc != 0:
        print(f"FAILED: {cmd}", file=sys.stderr)
        sys.exit(1)

    # Add this module's pcm to future compilations
    mod_flags += f" -fmodule-file={mod_name}={pcm_path}"

    # Compile to object
    cmd = f"{cxx} {cxxflags} {dep_flags} -fmodule-file={mod_name}={pcm_path} {pcm_path} -c -o {obj_path}"
    rc = os.system(cmd)
    if rc != 0:
        print(f"FAILED: {cmd}", file=sys.stderr)
        sys.exit(1)

    compiled_objs.append(obj_path)

# Compile main.cpp
main_src = os.path.join(projroot, "src/main.cpp")
main_obj = os.path.join(objdir, "main.o")
cmd = f"{cxx} {cxxflags} {mod_flags} -c {main_src} -o {main_obj}"
print(f"  COMPILE main.cpp")
rc = os.system(cmd)
if rc != 0:
    print(f"FAILED: {cmd}", file=sys.stderr)
    sys.exit(1)
compiled_objs.append(main_obj)

# Add std.o
compiled_objs.insert(0, os.path.join(objdir, "std.o"))
if os.path.exists(os.path.join(objdir, "std.compat.o")):
    compiled_objs.insert(1, os.path.join(objdir, "std.compat.o"))

# Link
bindir = os.path.join(outdir, "bin")
binary = os.path.join(bindir, "mcpp")
objs_str = " ".join(compiled_objs)
cmd = f"{cxx} {objs_str} -o {binary}"
print(f"  LINK {binary}")
rc = os.system(cmd)
if rc != 0:
    print(f"FAILED: {cmd}", file=sys.stderr)
    sys.exit(1)

print(f"\n:: Bootstrap complete: {binary}")
PYTHON

echo
echo ":: Phase 4: Verify"
"$BINDIR/mcpp" --version
echo
echo ":: SUCCESS: Bootstrap mcpp built at $BINDIR/mcpp"
echo ":: You can now use it for self-hosting: $BINDIR/mcpp build"
