#!/usr/bin/env bash
# Observations for .agents/docs/2026-10-11-build-dependencies-one-pipeline-design.md.
# Prints what each case does; asserts nothing. Usage: MCPP=/path/to/mcpp bash <this>
#
#   R1  #797 body: a module wrapper over a header library, imported by build.mcpp
#       through `host-module = true` (its GMF #includes the package's own header).
#   R2  #797 comment: one package in [dependencies] and [build-dependencies].
#   R2b the same library split across two workspace members: m1 links it, m2's
#       build.mcpp imports it (works today; a regression guard).
#   R2c a library reached by the target through another library, and named
#       directly in [build-dependencies] (works today; a regression guard).
#   R3  a feature requested only by a [build-dependencies] edge reaches the
#       target instance of the same package.
#   R4  [workspace.xlings.overrides] on a rooted workspace whose root package
#       depends on a member by path.
#   R5  the same with the root's own [xlings] by position: the warning appears
#       on a plain `mcpp build` of the root package.
#   R6  a header that a path host module includes is edited; the build
#       program is neither recompiled nor rerun (its identity hashes the
#       interface file only).
#   R7  a build plugin built as the root of its own build (the shape of
#       examples/08-build-rules/rules-embed): the engine module does not exist.
#   R8  `mcpp emit build-database` and compile_commands.json do not describe
#       build.mcpp, the host modules or the engine module.
#   W1  a build.mcpp beside a workspace root without [package] is ignored
#       without a word (the package tables there are reported).
#   W2  `features` written in [workspace.dependencies] is dropped without a
#       word when a member inherits the entry.
set -u
MCPP="${MCPP:-mcpp}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
show() { echo "── $1"; shift; ( "$@" ) 2>&1 | grep -E "error|warning|fatal|FEATURE|ok:" | head -8; echo; }

# ── a header library with a module wrapper (the toml++ shape) ───────────────
mklib() {  # $1 = dir
    mkdir -p "$1/include/hl" "$1/src"
    cat > "$1/include/hl/hl.hpp" <<'EOF'
#pragma once
namespace hl { inline int answer() { return 42; } }
EOF
    cat > "$1/src/hl.cppm" <<'EOF'
module;
#include <hl/hl.hpp>
export module hl;
export namespace hl { using hl::answer; }
#ifdef HL_FEATURE_X
export namespace hl { inline int feature_x() { return 1; } }
#endif
EOF
    cat > "$1/mcpp.toml" <<'EOF'
[package]
name = "hl"
version = "0.1.0"
[lib]
path = "src/hl.cppm"
[build]
sources = ["src/hl.cppm"]
include_dirs = ["include"]
[features]
x = { defines = ["HL_FEATURE_X"] }
[targets.hl]
kind = "lib"
EOF
}
mklib "$T/hl"
# The same library with the include made relative to the unit -- the shape
# mcpp-index uses today to get past R1 -- so that R2 isolates the edge merge.
mklib "$T/hlrel"
sed -i 's|#include <hl/hl.hpp>|#include "../include/hl/hl.hpp"|' "$T/hlrel/src/hl.cppm"

mkapp() {  # $1 = dir, $2 = manifest tail
    mkdir -p "$1/src"
    printf '[package]\nname = "app"\nversion = "0.1.0"\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n%s\n' "$2" > "$1/mcpp.toml"
}

# R1
mkapp "$T/r1" $'[build-dependencies]\nhl = { path = "../hl", host-module = true }'
echo 'int main() { return 0; }' > "$T/r1/src/main.cpp"
printf 'import std;\nimport hl;\nint main() { std::println("mcpp:cxxflag=-DANSWER={}", hl::answer()); }\n' > "$T/r1/build.mcpp"
show "R1 host-module over a header library" bash -c "cd $T/r1 && $MCPP build"

# R2
mkapp "$T/r2" $'[dependencies]\nhl = { path = "../hlrel" }\n[build-dependencies]\nhl = { path = "../hlrel", host-module = true }'
printf 'import std;\nimport hl;\nint main() { std::println("ok: {}", hl::answer()); }\n' > "$T/r2/src/main.cpp"
printf 'import std;\nimport hl;\nint main() { std::println("mcpp:cxxflag=-DANSWER={}", hl::answer()); }\n' > "$T/r2/build.mcpp"
show "R2 one package in both tables (target import)" bash -c "cd $T/r2 && $MCPP build"

# R2b
mkdir -p "$T/r2b"
printf '[workspace]\nmembers = ["m1", "m2"]\n' > "$T/r2b/mcpp.toml"
mkapp "$T/r2b/m1" $'[dependencies]\nhl = { path = "../../hlrel" }'
sed -i 's/name = "app"/name = "m1"/; s/targets.app/targets.m1/' "$T/r2b/m1/mcpp.toml"
printf 'import std;\nimport hl;\nint main() { std::println("ok: m1 {}", hl::answer()); }\n' > "$T/r2b/m1/src/main.cpp"
mkapp "$T/r2b/m2" $'[build-dependencies]\nhl = { path = "../../hlrel", host-module = true }'
sed -i 's/name = "app"/name = "m2"/; s/targets.app/targets.m2/' "$T/r2b/m2/mcpp.toml"
printf 'import std;\nint main() { std::println("ok: m2 {}", ANSWER); }\n' > "$T/r2b/m2/src/main.cpp"
printf 'import std;\nimport hl;\nint main() { std::println("mcpp:cxxflag=-DANSWER={}", hl::answer()); }\n' > "$T/r2b/m2/build.mcpp"
show "R2b workspace split" bash -c "cd $T/r2b && $MCPP build --workspace && $MCPP run -p m1 && $MCPP run -p m2"

# R2c
mkdir -p "$T/lib/src"
printf '[package]\nname = "lib"\nversion = "0.1.0"\n[dependencies]\nhl = { path = "../hlrel" }\n[targets.lib]\nkind = "lib"\n' > "$T/lib/mcpp.toml"
printf 'export module lib;\nimport hl;\nexport int via_lib() { return hl::answer(); }\n' > "$T/lib/src/lib.cppm"
mkapp "$T/r2c" $'[dependencies]\nlib = { path = "../lib" }\n[build-dependencies]\nhl = { path = "../hlrel", host-module = true }'
printf 'import std;\nimport lib;\nint main() { std::println("ok: {} {}", via_lib(), ANSWER); }\n' > "$T/r2c/src/main.cpp"
cp "$T/r2b/m2/build.mcpp" "$T/r2c/build.mcpp"
show "R2c transitive target use and a direct build dependency" bash -c "cd $T/r2c && $MCPP build && $MCPP run"

# R3 -- the library itself is a dependency of `mid`, which asks for feature x
# only in its [build-dependencies]; the app imports hl for its target.
mkdir -p "$T/mid/src"
printf '[package]\nname = "mid"\nversion = "0.1.0"\n[build-dependencies]\nhl = { path = "../hl", features = ["x"] }\n[targets.mid]\nkind = "lib"\n' > "$T/mid/mcpp.toml"
printf 'export module mid;\nexport int mid_v() { return 1; }\n' > "$T/mid/src/mid.cppm"
mkapp "$T/r3" $'[dependencies]\nhl = { path = "../hl" }\nmid = { path = "../mid" }'
cat > "$T/r3/src/main.cpp" <<'EOF'
import std;
import hl;
int main() {
#ifdef HL_FEATURE_X
    std::println("FEATURE x reached the target");
#endif
    std::println("ok: {}", hl::answer());
}
EOF
show "R3 a build-only feature request and the target instance" bash -c "cd $T/r3 && $MCPP build && $MCPP run"
grep -rhoE -- "-DHL_FEATURE_X" "$T/r3/target" 2>/dev/null | head -1 | sed 's/^/FEATURE define in target compile: /'

# R4, R5
mkws() {  # $1 = dir, $2 = root tail
    mkdir -p "$1/src" "$1/sub/src"
    printf '[package]\nname = "root"\nversion = "0.1.0"\n[targets.root]\nkind = "bin"\nmain = "src/main.cpp"\n[dependencies]\nsub = { path = "sub" }\n[workspace]\nmembers = ["sub"]\n%s\n' "$2" > "$1/mcpp.toml"
    echo 'int main() { return 0; }' > "$1/src/main.cpp"
    printf '[package]\nname = "sub"\nversion = "0.1.0"\n[targets.sub]\nkind = "lib"\n' > "$1/sub/mcpp.toml"
    printf 'export module sub;\nexport int f() { return 1; }\n' > "$1/sub/src/sub.cppm"
}
mkws "$T/r4" $'[workspace.xlings.overrides]\n"xim:cmake" = "/usr/bin/cmake"'
show "R4 [workspace.xlings.overrides] with the root depending on a member" bash -c "cd $T/r4 && $MCPP build"
mkws "$T/r5" $'[xlings.overrides]\n"xim:cmake" = "/usr/bin/cmake"'
show "R5 the root's own [xlings] by position, plain build of the root" bash -c "cd $T/r5 && $MCPP build"

# R6 -- uses the relative-include library so that the host compile succeeds.
mkapp "$T/r6" $'[build-dependencies]\nhl = { path = "../hlrel", host-module = true }'
printf 'import std;\nint main() { std::println("ok: ANSWER={}", ANSWER); }\n' > "$T/r6/src/main.cpp"
printf 'import std;\nimport hl;\nint main() { std::println("mcpp:cxxflag=-DANSWER={}", hl::answer()); }\n' > "$T/r6/build.mcpp"
show "R6 before the header edit" bash -c "cd $T/r6 && $MCPP build >/dev/null && $MCPP run"
sed -i 's/return 42/return 43/' "$T/hlrel/include/hl/hl.hpp"
show "R6 after editing the header to 43" bash -c "cd $T/r6 && $MCPP build >/dev/null && $MCPP run"

# R7
mkdir -p "$T/r7/src"
printf '[package]\nname = "rule"\nversion = "0.1.0"\n[targets.rule]\nkind = "lib"\n' > "$T/r7/mcpp.toml"
printf 'export module rule;\nimport std;\nimport mcpp;\nexport namespace rule { inline bool compile() { return true; } }\n' > "$T/r7/src/rule.cppm"
show "R7 a build plugin as the root of its own build" bash -c "cd $T/r7 && $MCPP build"

# R8 -- r6 has a build.mcpp importing a host module
( cd "$T/r6" && $MCPP emit build-database --format json 2>/dev/null ) > "$T/db.json"
python3 - "$T/db.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))["data"]["database"]
def sources(o):
    if isinstance(o, dict):
        for k, v in o.items():
            if k == "source" and isinstance(v, str): yield v.replace("\\", "/").split("/")[-1]
            else: yield from sources(v)
    elif isinstance(o, list):
        for v in o: yield from sources(v)
units = sorted(set(sources(doc)))
print("── R8 build database")
print("ok: translation units:", units)
print("ok: build.mcpp / hl.cppm / mcpp.cppm described:",
      "build.mcpp" in units, "hl.cppm" in units, "mcpp.cppm" in units)
PY
echo

# W1
mkdir -p "$T/w1/m/src"
printf '[workspace]\nmembers = ["m"]\n' > "$T/w1/mcpp.toml"
printf 'int main() { return 1; }\n' > "$T/w1/build.mcpp"      # would fail the build if it ran
printf '[package]\nname = "m"\nversion = "0.1.0"\n[targets.m]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$T/w1/m/mcpp.toml"
echo 'int main() { return 0; }' > "$T/w1/m/src/main.cpp"
show "W1 build.mcpp beside a virtual root (no output = ignored silently)" bash -c "cd $T/w1 && $MCPP build"

# W2
mkdir -p "$T/w2/m/src"
printf '[workspace]\nmembers = ["m"]\n[workspace.dependencies]\nhl = { path = "../hl", features = ["x"] }\n' > "$T/w2/mcpp.toml"
printf '[package]\nname = "m"\nversion = "0.1.0"\n[dependencies]\nhl.workspace = true\n[targets.m]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$T/w2/m/mcpp.toml"
cat > "$T/w2/m/src/main.cpp" <<'CPP'
import std;
import hl;
int main() {
#ifdef HL_FEATURE_X
    std::println("ok: feature x inherited");
#else
    std::println("FEATURE x from [workspace.dependencies] was dropped");
#endif
}
CPP
show "W2 features in [workspace.dependencies]" bash -c "cd $T/w2 && $MCPP build && $MCPP run -p m"
