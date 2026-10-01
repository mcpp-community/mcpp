#!/usr/bin/env bash
# requires:
# 849 -- a BMI served from the global cache waits for the modules it imports
# that this build compiles.
#
# A package's cache entry holds the units below its root. A module its build
# program generates lies below the consumer's target directory, so it is
# compiled in every build, also when the rest of the package is staged from
# the cache (xpkg's `lua_stdlib`, imported by its cached `executor`). The
# consumer's dyndep names the staged BMI only, and the stage edge had no input
# but the cache entry, so nothing ordered the consumer after the generated
# module's compile. A fresh build compiled the consumer first whenever the
# schedule allowed it:
#
#     mcpplibs.xpkg.lua_stdlib: error: failed to read compiled module: No such file or directory
#     mcpplibs.xpkg.executor: error: failed to read compiled module: Bad import dependency
#
# (the aarch64-linux-musl cross build of xlings, once the scan pass of
# 2026.9.30.2 changed the schedule). The stage edge of such a BMI now waits for
# the BMIs it imports that compile here.
#
# Criteria:
#   A. The second build of the project stages the package from the cache.
#   B. The stage edge of the cached BMI names the generated module's BMI after
#      `||`, and the aggregate every compile waits for does not hold it.
#   C. The order is carried by the graph and not by the schedule or by a
#      depfile of an earlier build: with the generated BMI, the consumer's
#      object and the recorded depfiles removed, ninja asked for the
#      consumer's object alone builds the generated module first.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

INDEX_DIR="$TMP/local-index"
INDEX_DIR_HOST="$(host_path "$INDEX_DIR")"
mkdir -p "$INDEX_DIR/pkgs/g"
cat > "$INDEX_DIR/pkgs/g/gen-dep.lua" <<'EOF'
package = {
    spec = "1",
    name = "gen-dep",
    description = "A package whose module imports a module its build program generates",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/gen-dep-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
EOF

mkdir -p "$TMP/app/src"
PAYLOAD="$TMP/app/.mcpp/.xlings/data/xpkgs/local-dev.gen-dep/1.0.0"
mkdir -p "$PAYLOAD/src"
cat > "$PAYLOAD/mcpp.toml" <<'EOF'
[package]
name    = "gen-dep"
version = "1.0.0"

[targets.gen-dep]
kind = "lib"
EOF
cat > "$PAYLOAD/build.mcpp" <<'EOF'
#include <cstdio>
#include <string>
import mcpp;
int main() {
    std::string p = std::string(mcpp::out_dir()) + "/gen-table.cppm";
    std::FILE* f = std::fopen(p.c_str(), "w");
    std::fputs("export module gen.dep.table;\nexport int table_value() { return 42; }\n", f);
    std::fclose(f);
    mcpp::generated(p.c_str());
    return 0;
}
EOF
cat > "$PAYLOAD/src/gen.dep.cppm" <<'EOF'
export module gen.dep;
import gen.dep.table;
export int dep_value() { return table_value(); }
EOF
cat > "$TMP/app/src/main.cpp" <<'EOF'
#include <cstdio>
import gen.dep;
int main() { std::printf("%d\n", dep_value()); }
EOF
cat > "$TMP/app/mcpp.toml" <<EOF
[package]
name    = "app"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR_HOST" }

[dependencies]
"local-dev.gen-dep" = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF

cd "$TMP/app"
"$MCPP" build > first.log 2>&1 || fail "the first build failed (a fixture problem)" first.log

# ── A ──────────────────────────────────────────────────────────────────────
"$MCPP" clean > /dev/null 2>&1
"$MCPP" build > hit.log 2>&1 || fail "A: the build that stages the package failed" hit.log
grep -qE 'Cached local-dev\.gen-dep v1\.0\.0' hit.log \
    || fail "A: the package was not staged from the cache, so nothing below is exercised" hit.log
[ "$(./target/*/*/bin/app | tr -d '\r')" = 42 ] || fail "A: the program does not print 42" hit.log
echo "ok: A, the package is staged from the cache"

# ── B ──────────────────────────────────────────────────────────────────────
N=$(find target -name build.ninja | head -1)
[ -n "$N" ] || fail "B: no build.ninja"
D=$(dirname "$N")
# The BMI directory and extension are the toolchain's (gcm.cache/*.gcm for
# GCC, pcm.cache/*.pcm for clang), and a dependency's BMIs lie below its
# package's directory there (pack drive and selection design 2026-10-01, B1);
# all three are read from the stage edge.
stage=$(grep -E '^build [a-z]+\.cache/[^ ]*gen\.dep\.[a-z]+ : stage_file ' "$N" || true)
[ -n "$stage" ] || fail "B: the cached BMI has no stage edge" "$N"
bmi=$(echo "$stage" | awk '{print $2}')
ext=${bmi##*.}
table="$(dirname "$bmi")/gen.dep.table.$ext"
case "$stage" in
    *"|| $table"*) ;;
    *) fail "B: the stage edge does not wait for the generated module's BMI: $stage" ;;
esac
phony=$(grep -E '^build _mcpp_staged_cache : phony' "$N" || true)
case " $phony " in
    *" $bmi "*) fail "B: the aggregate holds a stage that waits for a compile: $phony" ;;
esac
echo "ok: B, the stage edge waits for the generated module"

# ── C ──────────────────────────────────────────────────────────────────────
NINJA=""
for cand in "$MCPP_HOME"/registry/data/xpkgs/xim-x-ninja/*/ninja \
            "$MCPP_HOME"/registry/data/xpkgs/xim-x-ninja/*/bin/ninja; do
    if [ -f "$cand" ]; then NINJA="$cand"; break; fi
done
[ -n "$NINJA" ] || fail "C: no ninja binary"
main_obj=$(grep -E '^build [^ ]*main\.[a-z.]+ : cxx_object ' "$N" | head -1 | awk '{print $2}')
[ -n "$main_obj" ] || fail "C: no compile edge for main.cpp" "$N"
rm -f "$D/.ninja_deps" "$D/$table" "$D/$main_obj"
"$NINJA" -C "$D" "$main_obj" > c.log 2>&1 || fail "C: the consumer compiled before the module it reaches" c.log
[ -f "$D/$table" ] || fail "C: the generated module was not compiled" c.log
echo "ok: C, the graph orders the consumer after the generated module"

echo "PASS: 849_a_staged_bmi_waits_for_a_module_compiled_here"
