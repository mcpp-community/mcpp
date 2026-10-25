#!/usr/bin/env bash
# requires: python3
# 913 -- `mcpp emit build-database` describes the build programs' graph
# (SPEC-005 R3.13, mcpp#797), and compile_commands.json does not.
#
#   A  the sets `app:build` (the program, family `app`), `<lib>:host` (a library
#      of the program), `mcpp:core` (the engine interface) and `mcpp:std:host`
#      are present, in the configuration `build-program`; the target's set
#      keeps its name and its configuration;
#   B  the arguments of build.mcpp are those of the graph's compile, up to the
#      directory a planning pass writes in;
#   C  compile_commands.json, written by the build and by
#      `--spec compile-commands`, names no unit of the build programs' graph;
#   D  `mcpp self env` states `engineModules`, the directory of the units of
#      `mcpp:core`, and it holds the interface.
set -e
# The project-root copy of the compile database is opt-in since
# 2026.10.26.1 ([build] compile_commands); this test reads it.
export MCPP_COMPILE_COMMANDS=compile_commands.json
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"
PY=python3

mkdir -p "$TMP/lib/src" "$TMP/app/src"
printf '[package]\nname = "lib"\nversion = "0.1.0"\n[targets.lib]\nkind = "lib"\n' > "$TMP/lib/mcpp.toml"
printf 'export module lib;\nexport int lib_value() { return 42; }\n' > "$TMP/lib/src/lib.cppm"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[build-dependencies]\nlib = { path = "../lib" }\n[targets.app]\nkind = "bin"\nmain = "src/main.cpp"\n' > "$TMP/app/mcpp.toml"
printf '#include <cstdio>\nint main() { std::printf("V=%%d\\n", V); }\n' > "$TMP/app/src/main.cpp"
printf 'import std;\nimport mcpp;\nimport lib;\nint main() { mcpp::cxxflag(std::format("-DV={}", lib_value()).c_str()); }\n' > "$TMP/app/build.mcpp"
cd "$TMP/app"

"$MCPP" build > build.log 2>&1 || fail "the project did not build" build.log
"$MCPP" emit build-database --format json > db.json 2> db.err || fail "emit build-database failed" db.err
"$MCPP" emit build-database --spec compile-commands > cc.json 2> cc.err || fail "--spec compile-commands failed" cc.err
"$MCPP" self env --format json > env.json 2> env.err || fail "mcpp self env failed" env.err
host_cc=$(find target/.build-mcpp/host -maxdepth 2 -name compile_commands.json | head -1)
[ -n "$host_cc" ] || fail "the build programs' graph wrote no compile_commands.json" build.log

"$PY" - db.json cc.json compile_commands.json env.json "$host_cc" <<'EOF' || fail "A-D" db.json
import json, os, sys
db, cc, root_cc, env, host_cc = (json.load(open(p)) for p in sys.argv[1:6])
sets = {s["name"]: s for s in db["data"]["database"]["sets"]}

# A
for name, kind in (("app:build", "executable"), ("lib:host", "library"),
                   ("mcpp:core", "library"), ("mcpp:std:host", None)):
    candidates = [n for n in sets if n == name or n.endswith("." + name)]
    assert candidates, f"A: no set {name}: {sorted(sets)}"
    s = sets[candidates[0]]
    assert s["ide"]["configuration"] == "build-program", (name, s["ide"])
    if kind: assert s["ide"]["kind"] == kind, (name, s["ide"])
assert sets["app:build"]["family-name"] == "app", sets["app:build"]["family-name"]
assert "app" in sets and sets["app"]["ide"]["configuration"] == "dev", sets.get("app", {}).get("ide")

# B
unit = next(u for u in sets["app:build"]["translation-units"] if u["source"].endswith("build.mcpp"))
built = next(e for e in host_cc if e["file"].endswith("build.mcpp"))
def norm(args, wd):
    # One separator, and the directory above the graph -- the planning
    # directory of the database, the project of the build -- named alike.
    root = wd.replace("\\", "/").split("/target/.build-mcpp/host/")[0]
    return [a.replace("\\", "/").replace(root + "/target/", "<root>/target/") for a in args]
assert norm(unit["arguments"], unit["work-directory"]) == norm(built["arguments"], built["directory"]), \
    ("B", unit["arguments"], built["arguments"])

# C
for entries in (cc, root_cc):
    names = [os.path.basename(e["file"]) for e in entries]
    assert "main.cpp" in names, ("C", names)
    for unwanted in ("build.mcpp", "lib.cppm", "mcpp.cppm", "mcpp_core.cppm"):
        assert unwanted not in names, ("C", unwanted, names)

# D
engine = env["data"]["engineModules"]
assert os.path.isfile(os.path.join(engine, "mcpp.cppm")), ("D", engine)
core = sets["mcpp:core"]["translation-units"]
real = os.path.realpath   # one directory, two spellings (macOS /var, Windows)
assert all(real(os.path.dirname(u["source"])) == real(engine) for u in core), \
    ("D", engine, [u["source"] for u in core])
EOF
echo "PASS: 913_the_build_database_describes_the_build_programs"
