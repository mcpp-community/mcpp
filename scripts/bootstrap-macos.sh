#!/usr/bin/env bash
# scripts/bootstrap-macos.sh — one-shot bootstrap of mcpp on macOS.
#
# Compiles mcpp from source using upstream LLVM/Clang with C++23 modules.
# Only needed ONCE to produce the first macOS binary; afterwards mcpp
# can self-host via `mcpp build`.
#
# Prerequisites:
#   - Clang 20+ with libc++ module support (xlings LLVM or Homebrew LLVM)
#   - macOS SDK (xcode-select --install)
#   - Python 3 (ships with macOS)
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

STD_CPPM=$(find "$LLVM_ROOT" -name "std.cppm" -path "*/libc++/*" | head -1)
if [ -z "$STD_CPPM" ] || [ ! -f "$STD_CPPM" ]; then
    echo "error: std.cppm not found in LLVM installation" >&2
    exit 1
fi
echo ":: std.cppm  = $STD_CPPM"

STD_COMPAT_CPPM=$(find "$LLVM_ROOT" -name "std.compat.cppm" -path "*/libc++/*" | head -1)
echo ":: std.compat= ${STD_COMPAT_CPPM:-(not found)}"

# ─── Setup ───────────────────────────────────────────────────────────────────

PROJROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$PROJROOT/target/bootstrap"
PCMDIR="$OUTDIR/pcm.cache"
OBJDIR="$OUTDIR/obj"
BINDIR="$OUTDIR/bin"
mkdir -p "$PCMDIR" "$OBJDIR" "$BINDIR"

# Export for Python
export PROJROOT OUTDIR PCMDIR OBJDIR BINDIR CXX LLVM_ROOT STD_CPPM STD_COMPAT_CPPM SDKROOT

echo
echo ":: Compiling mcpp (42 modules + main.cpp)..."
echo

# ─── All-in-one Python build script ─────────────────────────────────────────
python3 << 'PYTHON'
import os, re, sys, subprocess
from pathlib import Path
from collections import defaultdict

projroot = Path(os.environ["PROJROOT"])
outdir   = Path(os.environ["OUTDIR"])
pcmdir   = Path(os.environ["PCMDIR"])
objdir   = Path(os.environ["OBJDIR"])
bindir   = Path(os.environ["BINDIR"])
cxx      = os.environ["CXX"]
sdkroot  = os.environ["SDKROOT"]
std_cppm = os.environ["STD_CPPM"]
std_compat = os.environ.get("STD_COMPAT_CPPM", "")

# Base flags — check if clang++.cfg already sets sysroot
result = subprocess.run([cxx, "-v", "-x", "c++", "/dev/null", "-c", "-o", "/dev/null"],
                       capture_output=True, text=True, timeout=10)
has_sysroot = "sysroot" in (result.stdout + result.stderr)
cxxflags = f"-std=c++23 -O2 -I{projroot}/src/libs/json"
if not has_sysroot:
    cxxflags += f" --sysroot={sdkroot}"

def run(cmd, desc=""):
    rc = os.system(cmd)
    if rc != 0:
        print(f"\nFAILED ({desc}):\n  {cmd}", file=sys.stderr)
        sys.exit(1)

# ─── Phase 1: Pre-compile std module ────────────────────────────────────────
print(":: Phase 1: Pre-compile std + std.compat")
run(f'{cxx} {cxxflags} -Wno-reserved-module-identifier --precompile "{std_cppm}" -o "{pcmdir}/std.pcm"',
    "precompile std")
run(f'{cxx} {cxxflags} -Wno-reserved-module-identifier "{pcmdir}/std.pcm" -c -o "{objdir}/std.o"',
    "compile std.o")

all_objs = [str(objdir / "std.o")]
std_mod_flags = f'-fmodule-file=std="{pcmdir}/std.pcm"'

if std_compat and os.path.isfile(std_compat):
    run(f'{cxx} {cxxflags} -Wno-reserved-module-identifier {std_mod_flags} --precompile "{std_compat}" -o "{pcmdir}/std.compat.pcm"',
        "precompile std.compat")
    run(f'{cxx} {cxxflags} -Wno-reserved-module-identifier {std_mod_flags} "{pcmdir}/std.compat.pcm" -c -o "{objdir}/std.compat.o"',
        "compile std.compat.o")
    all_objs.append(str(objdir / "std.compat.o"))
    std_mod_flags += f' -fmodule-file=std.compat="{pcmdir}/std.compat.pcm"'

# ─── Phase 2: Scan module declarations from source ──────────────────────────
print("\n:: Phase 2: Scanning module declarations")

# Regex patterns for module declarations in the module declaration region
# (before the first non-preprocessor, non-module statement)
re_export = re.compile(r'^\s*export\s+module\s+([\w.]+)\s*;')
re_import = re.compile(r'^\s*(?:export\s+)?import\s+([\w.]+)\s*;')
re_module = re.compile(r'^\s*module\s+([\w.]+)\s*;')  # module implementation unit

sources = sorted(projroot.glob("src/**/*.cppm")) + sorted(projroot.glob("src/**/*.cpp"))

# Map: module_name -> source_path
mod_source = {}
# Map: source_path -> [module_names_it_provides]
src_provides = {}
# Map: source_path -> [module_names_it_requires] (excluding std/std.compat)
src_requires = {}

for src in sources:
    provides = []
    requires = []
    try:
        with open(src, 'r') as f:
            for line in f:
                line = line.strip()
                # Stop scanning at first function/class/namespace body
                # (module declarations must come before implementation)
                if line.startswith('//') or line.startswith('#') or not line:
                    continue
                m = re_export.match(line)
                if m:
                    provides.append(m.group(1))
                    continue
                m = re_module.match(line)
                if m and not provides:  # module implementation unit
                    requires.append(m.group(1))
                    continue
                m = re_import.match(line)
                if m:
                    mod_name = m.group(1)
                    if mod_name not in ("std", "std.compat"):
                        requires.append(mod_name)
                    continue
                # If we hit something that's not a module-related keyword, stop
                if not line.startswith('export') and not line.startswith('import') and not line.startswith('module'):
                    break
    except Exception:
        pass

    src_provides[str(src)] = provides
    src_requires[str(src)] = requires
    for mod in provides:
        mod_source[mod] = str(src)

print(f"   Found {len(mod_source)} modules")

# ─── Phase 3: Topological sort ──────────────────────────────────────────────
print("\n:: Phase 3: Topological sort")

# Build dependency graph
visited = set()
order = []
in_progress = set()

def visit(mod_name):
    if mod_name in visited:
        return
    if mod_name in in_progress:
        # Circular dependency — shouldn't happen with well-formed modules
        print(f"   WARNING: circular dependency involving {mod_name}")
        return
    in_progress.add(mod_name)
    src = mod_source.get(mod_name)
    if src:
        for dep in src_requires.get(src, []):
            if dep in mod_source:
                visit(dep)
    in_progress.discard(mod_name)
    visited.add(mod_name)
    order.append(mod_name)

for mod_name in mod_source:
    visit(mod_name)

print(f"   Build order: {len(order)} modules")

# ─── Phase 4: Compile modules in dependency order ───────────────────────────
print("\n:: Phase 4: Compiling modules")

# Track accumulated -fmodule-file flags
accumulated_mod_flags = std_mod_flags

for i, mod_name in enumerate(order):
    src = mod_source[mod_name]
    safe_name = mod_name.replace(".", "_")
    pcm_path = pcmdir / f"{safe_name}.pcm"
    obj_path = objdir / f"{safe_name}.o"

    # Build dep flags for this module's requirements
    dep_flags = accumulated_mod_flags

    # Precompile .cppm → .pcm
    print(f"   [{i+1}/{len(order)}] {mod_name}")
    run(f'{cxx} {cxxflags} {dep_flags} --precompile "{src}" -o "{pcm_path}"',
        f"precompile {mod_name}")

    # Compile .pcm → .o
    run(f'{cxx} {cxxflags} {dep_flags} "{pcm_path}" -c -o "{obj_path}"',
        f"compile {mod_name}")

    # Add this module to accumulated flags for subsequent modules
    accumulated_mod_flags += f' -fmodule-file={mod_name}="{pcm_path}"'
    all_objs.append(str(obj_path))

# ─── Phase 5: Compile main.cpp ──────────────────────────────────────────────
print("\n:: Phase 5: Compile main.cpp")
main_src = projroot / "src" / "main.cpp"
main_obj = objdir / "main.o"
run(f'{cxx} {cxxflags} {accumulated_mod_flags} -c "{main_src}" -o "{main_obj}"',
    "compile main.cpp")
all_objs.append(str(main_obj))

# ─── Phase 6: Link ──────────────────────────────────────────────────────────
print("\n:: Phase 6: Link")
binary = bindir / "mcpp"
objs_str = " ".join(f'"{o}"' for o in all_objs)
run(f'{cxx} {objs_str} -o "{binary}"', "link")

# ─── Done ────────────────────────────────────────────────────────────────────
print(f"\n:: Bootstrap complete!")
result = subprocess.run([str(binary), "--version"], capture_output=True, text=True)
print(f"   {result.stdout.strip()}")
PYTHON

echo
"$BINDIR/mcpp" --version
echo ":: SUCCESS: $BINDIR/mcpp"
