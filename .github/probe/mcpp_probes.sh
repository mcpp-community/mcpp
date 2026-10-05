#!/usr/bin/env bash
# TEMPORARY PROBE (do not merge): mcpp built from main, on the cases the design
# for #770 / #771 / #769 relies on. Prints; asserts nothing.
set -u
M="${MCPP_BUILT:?}"
W="$RUNNER_TEMP/mcpp-probes"; rm -rf "$W"; mkdir -p "$W"
run() { echo "\$ $*"; "$@" 2>&1; echo "rc=$?"; }
section() { echo; echo "### $1"; }
edges() {   # the link edges of the generated build.ninja, with their link flags
  local n; n=$(find target -name build.ninja | head -1)
  [ -n "$n" ] || { echo "(no build.ninja)"; return; }
  python - "$n" <<'PY'
import re, sys
glob, edges, cur = {}, {}, None
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    line = line.rstrip("\n")
    if line.startswith("build "):
        cur = line.split(" : ")[0][6:].strip()
        rule = line.split(" : ", 1)[1].split()[0] if " : " in line else "?"
        edges[cur] = {"_rule": rule}
        continue
    if line.startswith("  ") and cur:
        k, _, v = line.strip().partition(" = "); edges[cur][k] = v; continue
    cur = None
    m = re.match(r"^(\w+)\s*=\s*(.*)$", line)
    if m: glob[m.group(1)] = m.group(2)
for e, v in edges.items():
    if v["_rule"] not in ("cxx_shared", "cxx_link"): continue
    src = "group" if "ldflags" in v else "plan-global"
    ld = v.get("ldflags", glob.get("ldflags", "")) + " | unit: " + v.get("unit_ldflags", "")
    print(f"{e} [{v['_rule']}, {src}]\n    {ld}")
PY
}
"$M" --version

# ---------------------------------------------------------------- #771
section "771 workspace member's shared library and its build.mcpp link_lib (msvc@system)"
mkdir -p "$W/ws771/player/src" "$W/ws771/other/src"; cd "$W/ws771"
cat > mcpp.toml <<'EOF'
[workspace]
members = ["player", "other"]

[toolchain]
windows = "msvc@system"
EOF
cat > player/mcpp.toml <<'EOF'
[package]
name = "player"
version = "0.1.0"

[targets.player_dll]
kind = "shared"

[targets.player]
kind = "bin"
main = "src/main.cpp"
EOF
cat > player/build.mcpp <<'EOF'
import mcpp;
int main() { mcpp::link_lib("ws2_32"); return 0; }
EOF
cat > player/src/net.cpp <<'EOF'
#include <winsock2.h>
int player_err() { return WSAGetLastError(); }
EOF
cat > player/src/main.cpp <<'EOF'
int player_err();
int main() { return player_err() == 0 ? 0 : 0; }
EOF
cat > other/mcpp.toml <<'EOF'
[package]
name = "other"
version = "0.1.0"

[build]
ldflags = ["/IGNORE:4099"]

[targets.other]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int main() { return 0; }\n' > other/src/main.cpp
run "$M" build
edges
section "771 the same, selecting the member (-p player)"
rm -rf target; run "$M" build -p player
edges

# ---------------------------------------------------------------- #770 F6
section "770 F6 /GL in cxxflags + default windows_auto_export on a DLL (msvc@system)"
mkdir -p "$W/gl/src"; cd "$W/gl"
cat > mcpp.toml <<'EOF'
[package]
name = "glprobe"
version = "0.1.0"

[toolchain]
windows = "msvc@system"

[build]
cxxflags = ["/GL"]
ldflags = ["/LTCG"]

[targets.glprobe]
kind = "shared"
EOF
printf 'int gl_fn(int x) { return x + 1; }\n' > src/gl.cpp
run "$M" build

section "770 F6b /GL + windows_auto_export = false + __declspec(dllexport)"
mkdir -p "$W/gl2/src"; cd "$W/gl2"
cat > mcpp.toml <<'EOF'
[package]
name = "glprobe2"
version = "0.1.0"

[toolchain]
windows = "msvc@system"

[build]
cxxflags = ["/GL"]
ldflags = ["/LTCG"]

[targets.glprobe2]
kind = "shared"
windows_auto_export = false
EOF
printf '__declspec(dllexport) int gl_fn(int x) { return x + 1; }\n' > src/gl.cpp
run "$M" build
dll=$(find target -name 'glprobe2.dll' | head -1); [ -n "$dll" ] && MSYS2_ARG_CONV_EXCL='*' run dumpbin -nologo -exports "$dll"

section "770 lto = true on cl.exe today: what the build line says"
mkdir -p "$W/lto/src"; cd "$W/lto"
cat > mcpp.toml <<'EOF'
[package]
name = "ltoprobe"
version = "0.1.0"

[toolchain]
windows = "msvc@system"

[profile.release]
lto = true

[targets.ltoprobe]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'int main() { return 0; }\n' > src/main.cpp
run "$M" build --release
n=$(find target -name build.ninja | head -1); grep -n "GL\|LTCG\|flto" "$n" | head -5; echo "(grep done)"

# ---------------------------------------------------------------- #769 / D2
section "D2 import std only: is std.compat compiled anyway?"
mkdir -p "$W/stdonly/src"; cd "$W/stdonly"
cat > mcpp.toml <<'EOF'
[package]
name = "std-only"
version = "0.1.0"
standard = "c++23"

[toolchain]
windows = "llvm@22.1.8"

[target.x86_64-windows-msvc]
sysroot = "msvc@system"

[targets.std-only]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'import std;\nint main() { std::vector<int> v{1}; return v.size() == 1 ? 0 : 1; }\n' > src/main.cpp
run "$M" build
find "${MCPP_HOME:-$HOME/.mcpp}" -path '*std.compat*' \( -name '*.pcm' -o -name '*.o' -o -name '*.obj' \) 2>/dev/null | head -5
echo "(compat artefacts listed above, if any)"

section "769 llvm + MSVC STL: import std.compat"
mkdir -p "$W/compat/src"; cd "$W/compat"
cat > mcpp.toml <<'EOF'
[package]
name = "std-compat-canary"
version = "0.1.0"
standard = "c++23"

[toolchain]
windows = "llvm@22.1.8"

[target.x86_64-windows-msvc]
sysroot = "msvc@system"

[targets.std-compat-canary]
kind = "bin"
main = "src/main.cpp"
EOF
cat > src/main.cpp <<'EOF'
import std.compat;
int main() {
    auto* memory = ::malloc(32);
    if (!memory) return 1;
    ::free(memory);
    std::vector<int> values{1, 2, 3};
    ::printf("compat=%zu\n", values.size());
    return values.size() == 3 ? 0 : 1;
}
EOF
run "$M" build
run "$M" run
