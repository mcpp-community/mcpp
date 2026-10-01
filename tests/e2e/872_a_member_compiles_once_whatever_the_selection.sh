#!/usr/bin/env bash
# requires: python3
# 872 -- a unit's compile does not depend on which members a command selects
# (pack drive and selection design 2026-10-01, B; mcpp#751).
#
# A workspace's build directory serves every selection of one configuration:
# `--workspace` and `-p app` compile `core` into the same directory. Three facts
# computed over the plan's graph reached `core`'s commands, so each switch
# between selections recompiled it:
#
#   - a module name two members provide moved every BMI of that name below its
#     provider's directory, and told the importers so, only when the graph held
#     both providers (GalTranslPP 3.1.3: 3m22s to recompile its core);
#   - a file no member contains (`../shared/m.cppm`) was owned by the virtual
#     root, whose object census then depended on how many members listed it;
#   - a member that builds a shared library put `-fPIC` on every unit of the
#     graph.
#
# Every package's BMIs now lie below its own directory except the root's, a
# unit reads one map of what it reaches, a source belongs to the member that
# declares it, and position independence follows the target.
#
# Criteria:
#   A. `core` and `tool` compile one file `shared/m.cppm`, `tool2` has its own
#      module `m`, `dso` is a shared library: `--workspace` builds them all and
#      each program prints its own module's value.
#   B. After `--workspace`, `-p M` compiles nothing for every member M, and
#      alternating selections compiles nothing.
#   C. `emit build-database -p M` and `--workspace` give every source of M's
#      closure the same argument list (compared per source as parsed JSON,
#      each database's private build directory set aside).
#   D. The one stated exception: a feature that only one member asks of a
#      shared member changes exactly that define (docs/06; e2e 851 C).
#   E. A dependency's module served from the global cache is staged below its
#      package's directory, and the program that imports it runs.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac
bin_of() { find "$1" -path "*/bin/*" -name "$2$EXE" -type f | head -1; }
# The compile steps `.ninja_log` records after line $2 of the log in $1.
compiles_after() {
    tail -n +"$(( $2 + 1 ))" "$1" | awk -F'\t' '$4 ~ /\.(o|obj)$/ { n++ } END { print n + 0 }'
}

cd "$TMP"
mkdir -p ws/shared ws/core/src ws/app/src ws/tool/src ws/tool2/src ws/dso/src
cat > ws/mcpp.toml <<'EOF'
[workspace]
members = ["core", "app", "tool", "tool2", "dso"]
EOF
printf 'export module m;\nexport int answer() { return 42; }\n' > ws/shared/m.cppm
printf 'export module corelib;\nimport m;\nexport int core_value() { return answer(); }\n' > ws/core/src/core.cppm
cat > ws/core/mcpp.toml <<'EOF'
[package]
name    = "core"
version = "0.1.0"

[build]
sources = ["src/*.cppm", "../shared/m.cppm"]

[targets.core]
kind = "lib"
EOF
printf '#include <cstdio>\nimport corelib;\nint main() { std::printf("%%d\\n", core_value()); }\n' > ws/app/src/main.cpp
cat > ws/app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
core = { path = "../core" }

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
printf '#include <cstdio>\nimport m;\nint main() { std::printf("%%d\\n", answer() + 1); }\n' > ws/tool/src/main.cpp
cat > ws/tool/mcpp.toml <<'EOF'
[package]
name    = "tool"
version = "0.1.0"

[build]
sources = ["src/*.cpp", "../shared/m.cppm"]

[targets.tool]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'export module m;\nexport int answer() { return 7; }\n' > ws/tool2/src/m.cppm
printf '#include <cstdio>\nimport m;\nint main() { std::printf("%%d\\n", answer()); }\n' > ws/tool2/src/main.cpp
cat > ws/tool2/mcpp.toml <<'EOF'
[package]
name    = "tool2"
version = "0.1.0"

[targets.tool2]
kind = "bin"
main = "src/main.cpp"
EOF
printf 'export module dso;\nexport int dso_value() { return 5; }\n' > ws/dso/src/dso.cppm
cat > ws/dso/mcpp.toml <<'EOF'
[package]
name    = "dso"
version = "0.1.0"

[targets.dso]
kind = "shared"
EOF
cd ws

# ── A ──────────────────────────────────────────────────────────────────────
"$MCPP" build --workspace > a.log 2>&1 || fail "A: the workspace did not build" a.log
for p in app:42 tool:43 tool2:7; do
    b=$(bin_of target "${p%%:*}")
    [ -n "$b" ] || fail "A: no program ${p%%:*}" a.log
    [ "$("$b" | tr -d '\r')" = "${p#*:}" ] || fail "A: ${p%%:*} does not print ${p#*:}"
done
echo "ok: A, the workspace builds and each program prints its own module's value"

# ── B ──────────────────────────────────────────────────────────────────────
log=$(find target -name .ninja_log | head -1)
[ -n "$log" ] || fail "B: no .ninja_log"
[ "$(find target -name .ninja_log | wc -l | tr -d ' ')" = 1 ] || fail "B: more than one build directory"
for sel in "-p core" "-p app" "-p tool" "-p tool2" "-p dso" "-p app" "-p tool2" "-p app" "--workspace"; do
    before=$(wc -l < "$log")
    "$MCPP" build $sel > b.log 2>&1 || fail "B: build $sel failed" b.log
    n=$(compiles_after "$log" "$before")
    [ "$n" = 0 ] || { tail -n +"$(( before + 1 ))" "$log"; fail "B: build $sel compiled $n units after --workspace" b.log; }
done
echo "ok: B, no selection recompiles what another selection compiled"

# ── C ──────────────────────────────────────────────────────────────────────
# compare <db-a> <db-b> <member> [<allowed token substring>]: every source of
# the member's closure has one argument list in both databases; with a fourth
# argument, the lists may differ only by tokens holding that substring.
compare() {
    python3 - "$@" <<'PY'
import json, re, sys
a, b, member = sys.argv[1], sys.argv[2], sys.argv[3]
allowed = sys.argv[4] if len(sys.argv) > 4 else None
def units(path):
    out = {}
    for s in json.load(open(path))["sets"]:
        for u in s["translation-units"]:
            args = [re.sub(r"build-database[/\\][0-9a-f]+", "<db>", x) for x in u["arguments"]]
            out[(s["name"], u["source"])] = args
    return out
ua, ub = units(a), units(b)
shared = sorted(set(ua) & set(ub))
if not any(k[0] == member for k in shared):
    print(f"FAIL: no source of {member} is in both databases"); sys.exit(1)
bad = 0
for k in shared:
    if ua[k] == ub[k]: continue
    diff = set(ua[k]) ^ set(ub[k])
    if allowed and diff and all(allowed in t for t in diff): continue
    bad += 1
    print(f"FAIL: {k[1]} ({k[0]}) differs: {sorted(diff)}")
sys.exit(1 if bad else 0)
PY
}
"$MCPP" emit build-database --workspace > c-ws.json 2> c-ws.err || fail "C: emit --workspace failed" c-ws.err
for m in core app tool tool2 dso; do
    "$MCPP" emit build-database -p "$m" > "c-$m.json" 2> "c-$m.err" || fail "C: emit -p $m failed" "c-$m.err"
    compare "c-$m.json" c-ws.json "$m" || fail "C: the argument lists of $m's closure depend on the selection"
done
# An argument file that a database names is one a reader can expand.
python3 - c-ws.json <<'PY' || fail "C: the database names an argument file that does not exist"
import json, os, sys
missing = [a for s in json.load(open(sys.argv[1]))["sets"] for u in s["translation-units"]
           for a in u["arguments"] if a.startswith("@") and not os.path.isfile(a[1:])]
for a in missing: print("missing:", a)
sys.exit(1 if missing else 0)
PY
grep -q '\.modmap' c-ws.json || [ -n "$(grep -l fmodule-mapper c-ws.json)" ] \
    || fail "C: no unit names a module map, so the comparison above measured nothing" c-ws.json
echo "ok: C, every source of every member has one argument list in both selections"

# ── D ──────────────────────────────────────────────────────────────────────
cd "$TMP"
mkdir -p fws/lib/src fws/a/src fws/b/src
printf '[workspace]\nmembers = ["lib", "a", "b"]\n' > fws/mcpp.toml
cat > fws/lib/mcpp.toml <<'EOF'
[package]
name    = "lib"
version = "0.1.0"

[features]
extra = []

[targets.lib]
kind = "lib"
EOF
printf 'export module featlib;\nexport int v() { return 1; }\n' > fws/lib/src/featlib.cppm
for m in a b; do
    feat=""; [ "$m" = a ] && feat=', features = ["extra"]'
    cat > fws/$m/mcpp.toml <<EOF
[package]
name    = "$m"
version = "0.1.0"

[dependencies]
lib = { path = "../lib"$feat }

[targets.$m]
kind = "bin"
main = "src/main.cpp"
EOF
    printf 'import featlib;\nint main() { return v() == 1 ? 0 : 1; }\n' > fws/$m/src/main.cpp
done
cd fws
"$MCPP" emit build-database --workspace > d-ws.json 2> d-ws.err || fail "D: emit --workspace failed" d-ws.err
"$MCPP" emit build-database -p b > d-b.json 2> d-b.err || fail "D: emit -p b failed" d-b.err
compare d-b.json d-ws.json lib FEATURE_EXTRA \
    || fail "D: lib differs by more than the feature that only a asks for"
if compare d-b.json d-ws.json lib > /dev/null 2>&1; then
    fail "D: the feature union did not reach lib's compile, so this criterion measured nothing"
fi
echo "ok: D, a feature one member asks for changes exactly its define"

# ── E ──────────────────────────────────────────────────────────────────────
cd "$TMP"
export MCPP_HOME="$TMP/mcpp-home"
source "$HERE/_inherit_toolchain.sh"
INDEX_DIR="$TMP/local-index"
INDEX_DIR_HOST="$(host_path "$INDEX_DIR")"
mkdir -p "$INDEX_DIR/pkgs/q" e/src
cat > "$INDEX_DIR/pkgs/q/qdep.lua" <<'EOF'
package = {
    spec = "1",
    name = "qdep",
    description = "A package that provides a module",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/qdep-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
EOF
PAYLOAD="$TMP/e/.mcpp/.xlings/data/xpkgs/local-dev.qdep/1.0.0"
mkdir -p "$PAYLOAD/src"
cat > "$PAYLOAD/mcpp.toml" <<'EOF'
[package]
name    = "qdep"
version = "1.0.0"

[targets.qdep]
kind = "lib"
EOF
printf 'export module qdep.part:inner;\nexport int inner() { return 40; }\n' > "$PAYLOAD/src/inner.cppm"
printf 'export module qdep.part;\nexport import :inner;\nexport int value() { return inner() + 2; }\n' > "$PAYLOAD/src/part.cppm"
printf '#include <cstdio>\nimport qdep.part;\nint main() { std::printf("%%d\\n", value()); }\n' > e/src/main.cpp
cat > e/mcpp.toml <<EOF
[package]
name    = "app"
version = "0.1.0"

[indices]
local-dev = { path = "$INDEX_DIR_HOST" }

[dependencies]
"local-dev.qdep" = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
EOF
cd e
"$MCPP" build > e1.log 2>&1 || fail "E: the first build failed" e1.log
"$MCPP" clean > /dev/null 2>&1
"$MCPP" build > e2.log 2>&1 || fail "E: the build served from the cache failed" e2.log
grep -qE 'Cached local-dev\.qdep v1\.0\.0' e2.log \
    || fail "E: the dependency was not served from the cache, so nothing below is exercised" e2.log
staged=$(find target -path '*.cache/*qdep*/qdep.part.*' -type f | head -1)
[ -n "$staged" ] || { find target -name 'qdep.part.*'; fail "E: the staged BMI does not lie below its package's directory" e2.log; }
[ "$("$(bin_of target app)" | tr -d '\r')" = 42 ] || fail "E: the program does not print 42" e2.log
echo "ok: E, a cached dependency's module is staged below its package's directory and loads"

echo "PASS: 872_a_member_compiles_once_whatever_the_selection"
