#!/usr/bin/env bash
# requires: python3
# 840_a_workspace_database_describes_each_package_once.sh -- workspace design
# 2026-09-29 §15 and §17.1; SPEC-005 R3.3, R3.5, R5.2.
#
# `mcpp emit build-database` and `mcpp build --configure-only` plan a
# workspace as `mcpp build` does: one plan per configuration group, with each
# member's tests. 2026.9.29.4 planned each member separately, so a package two
# members use was described once per member, each time with other arguments.
#
#   A  one configuration: each set is named by its package, with no prefix;
#      each source is described once; a member that is a program is described
#      as one and its tests as tests; `--configure-only` writes one compile
#      command per source;
#   B  two configurations that both compile core: each set name is prefixed
#      with its configuration's name, core is described once per
#      configuration, and the root `compile_commands.json` of `--configure-only`
#      and of `build --workspace` describes both configurations (it was the
#      database of whichever group was written last);
#   C  a member whose planning fails, in a group with others: it fails alone,
#      with its own `mcpp.toml` as the diagnostic's path, and the others are
#      described (R5.2).
set -e
# The project-root copy of the compile database is opt-in since
# 2026.10.26.1 ([build] compile_commands); this test reads it.
export MCPP_COMPILE_COMMANDS=compile_commands.json

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

member() {   # $1 = name, $2 = standard ("" for the default)
    mkdir -p $1/src
    { printf '[package]\nname = "%s"\nversion = "0.1.0"\n' $1
      [ -n "$2" ] && printf 'standard = "%s"\n' "$2"
      printf '\n[dependencies]\ncore = { path = "../core" }\n\n[targets.%s]\nkind = "bin"\nmain = "src/main.cpp"\n' $1
    } > $1/mcpp.toml
    printf 'import db_core;\nint main() { return core_v() == 1 ? 0 : 1; }\n' > $1/src/main.cpp
}
core() {
    mkdir -p core/src
    printf '[package]\nname = "core"\nversion = "0.1.0"\n\n[targets.core]\nkind = "lib"\n' > core/mcpp.toml
    printf 'export module db_core;\nexport int core_v() { return 1; }\n' > core/src/core.cppm
}

cat > check.py <<'PY'
import json, sys, re, collections
d = json.load(open(sys.argv[1])); part = sys.argv[2]
sets = d["data"]["database"]["sets"]
names = [s["name"] for s in sets]
assert len(names) == len(set(names)), f"{part}: a set is described twice: {names}"
prefix = lambda n: n.split("/")[0] if "/" in n else ""
base = lambda n: n.split("/")[-1]
units = collections.Counter((prefix(s["name"]), t["source"]) for s in sets for t in s["translation-units"])
assert all(c == 1 for c in units.values()), f"{part}: a source is described twice in one configuration"
kinds = {base(s["name"]): s["ide"]["kind"] for s in sets}
if part == "A":
    assert all("/" not in n for n in names), f"A: a set of one configuration is prefixed: {names}"
    assert names.count("core") == 1, f"A: core: {names}"
    assert kinds.get("cli") == "executable" and kinds.get("cli:test") == "test", f"A: kinds {kinds}"
if part == "B":
    assert all(re.fullmatch(r"[0-9a-f]{16}/.+", n) for n in names), f"B: names {names}"
    cores = [n for n in names if base(n) == "core"]
    assert len(cores) == 2 and len({prefix(n) for n in cores}) == 2, f"B: core per configuration: {names}"
if part == "C":
    errors = [x for x in d["diagnostics"] if x["severity"] == "error"]
    assert len(errors) == 1 and errors[0].get("path", "").replace("\\", "/") == "broken/mcpp.toml", \
        f"C: diagnostics {d['diagnostics']}"
    assert {"core", "cli", "gui"} <= {base(n) for n in names}, f"C: the other members: {names}"
    # Planned one by one, the members are still one configuration.
    assert all("/" not in n for n in names), f"C: the fallback split the configuration: {names}"
PY
cat > root.py <<'PY'
import json, pathlib, sys, collections
# argv: <configurations that compile core> <file>...
entries = json.loads(pathlib.Path("compile_commands.json").read_text())
norm = lambda s: s.replace("\\", "/")
pairs = collections.Counter((norm(e["file"]), norm(e.get("output", ""))) for e in entries)
assert all(c == 1 for c in pairs.values()), "a file and output have two commands"
files = [norm(e["file"]) for e in entries]
cores = sum(1 for f in files if f.endswith("core/src/core.cppm"))
assert cores == int(sys.argv[1]), f"core has {cores} commands, expected one per configuration ({sys.argv[1]})"
for want in sys.argv[2:]:
    assert any(f.endswith(want) for f in files), f"{want} is not described"
PY

# ── A ──────────────────────────────────────────────────────────────────────
mkdir a && cd a
printf '[workspace]\nmembers = ["core", "cli", "gui"]\n' > mcpp.toml
core; member cli; member gui
mkdir -p cli/tests
printf 'import db_core;\nint main() { return core_v() == 1 ? 0 : 1; }\n' > cli/tests/check.cpp
"$MCPP" emit build-database --format json -o db.json > e.log 2>&1 || fail "A: emit failed" e.log
python3 ../check.py db.json A || fail "A" db.json
"$MCPP" build --configure-only > c.log 2>&1 || fail "A: --configure-only failed" c.log
python3 ../root.py 1 cli/src/main.cpp || fail "A: compile_commands.json" c.log
cd ..

# ── B ──────────────────────────────────────────────────────────────────────
mkdir b && cd b
printf '[workspace]\nmembers = ["core", "cli", "modern"]\n' > mcpp.toml
core; member cli; member modern c++26
"$MCPP" emit build-database --format json -o db.json > e.log 2>&1 || fail "B: emit failed" e.log
python3 ../check.py db.json B || fail "B" db.json
for cmd in "build --configure-only" "build --workspace"; do
    rm -f compile_commands.json
    "$MCPP" $cmd > d.log 2>&1 || fail "B: mcpp $cmd failed" d.log
    python3 ../root.py 2 cli/src/main.cpp modern/src/main.cpp \
        || fail "B: the root database of mcpp $cmd" d.log
done
cd ..

# ── C ──────────────────────────────────────────────────────────────────────
mkdir c && cd c
printf '[workspace]\nmembers = ["core", "cli", "gui", "broken"]\n' > mcpp.toml
core; member cli; member gui
mkdir -p broken/src
printf '[package]\nname = "broken"\nversion = "0.1.0"\n\n[dependencies]\nmissing = { path = "../missing" }\n' > broken/mcpp.toml
printf 'int main() { return 0; }\n' > broken/src/main.cpp
set +e
"$MCPP" emit build-database --format json -o db.json > e.log 2>&1
rc=$?
set -e
[ "$rc" = 1 ] || fail "C: emit exited $rc, expected 1" e.log
python3 ../check.py db.json C || fail "C" db.json e.log

echo "PASS: 840_a_workspace_database_describes_each_package_once"
