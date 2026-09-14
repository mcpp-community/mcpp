#!/usr/bin/env bash
# requires: python3
# 688 -- `mcpp emit build-database` prints the plan as an S1 build database and
# writes nothing into the project (docs/specs/build-database.md, SPEC-005).
#
# The project has every module declaration form, a test, a path
# dev-dependency and `import std`. Criteria:
#   A. `--format json`: kind `mcpp.build-database`, kindVersion 1, `data.spec`
#      names S1 0.2.0, effects without `write-project`, and `--protocol-version`
#      declares the command without it.
#   B. The project tree is byte-identical before and after. The control leg at
#      the end runs `build --configure-only` on the same tree and sees it change,
#      which shows the measurement can see a write.
#   C. The document validates against the vendored S1 schema.
#   D. Each unit's role is its declaration form; sets are per package plus
#      `<package>:test` and `mcpp:std`, and each set sees every other set.
#   E. The `mcpp:std` unit names an existing source that provides `std`.
#   F. `--spec compile-commands` lists the same arguments as the S1 units, and,
#      once the output directory is mapped, the arguments `build
#      --configure-only` writes to compile_commands.json.
#   G. Usage errors write nothing to stdout and exit 2; outside a project the
#      envelope has no `data` and exits 1.
#   H. The bare document goes to stdout, or to `-o <file>` with stdout empty.
#   I. `watch` names the manifests, the lock, the source and test globs; the
#      fingerprint is stable and follows a source edit.
#   J. Each unit's arguments are its driver, its set's `baseline-arguments`, its
#      `local-arguments` and its `-c <source> -o <object>`; `private` is false;
#      each toolchain's `config-files` names existing files, and no `.cfg` that
#      the units bypass with `--no-default-config`.
#   K. The discovered test is scanned as a package source is: the imports inside
#      its comment and its raw string are not in `requires`.
#   L. The planning pass compiles nothing and writes no link input: its work
#      directory, fresh for this project, holds the resolution record and no
#      object, BMI or `mcpp-clean-link.specs`; in a home whose std cache is cold
#      the document still lists the std unit and the home gains no object or
#      BMI. The control leg at the end runs `build --configure-only` on a copy
#      in that home and sees the std module compiled there.
set -e

TMP=$(mktemp -d)   # the measured tree: the project and its dev-dependency
OUT=$(mktemp -d)   # everything this script writes, outside the measured tree
cleanup() {
    if [ -n "${WORK_DIR:-}" ] && [ -d "$WORK_DIR" ]; then rm -rf "$WORK_DIR"; fi
    rm -rf "$TMP" "$OUT"
}
trap cleanup EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
SCHEMA="$HERE/fixtures/s1/s1-build-database.schema.json"
VALIDATE="$HERE/_json_schema_subset.py"
PY=python3

mkdir -p "$TMP/devkit/include" "$TMP/hello/src" "$TMP/hello/tests"
cat > "$TMP/devkit/mcpp.toml" <<'EOF'
[package]
name = "devkit"
version = "0.1.0"

[build]
include_dirs = ["include"]
EOF
echo '#define DEVKIT_MARKER 1' > "$TMP/devkit/include/devkit.hpp"

cd "$TMP/hello"
cat > mcpp.toml <<'EOF'
[package]
name = "hello"
version = "0.1.0"
standard = "c++23"

[dev-dependencies]
devkit = { path = "../devkit" }
EOF
printf 'export module hello.greet;\nexport import :detail;\nimport std;\nexport std::string greet();\n' > src/greet.cppm
printf 'export module hello.greet:detail;\nexport int answer() { return 42; }\n' > src/detail.cppm
printf 'module hello.greet:impl;\nint hidden() { return 7; }\n' > src/impl.cppm
printf 'module hello.greet;\nimport :impl;\nstd::string greet() { return "hi"; }\n' > src/greet_impl.cpp
printf 'import hello.greet;\nimport std;\nint main() { std::println("{}", greet()); }\n' > src/main.cpp
printf '#include <devkit.hpp>\n/*\nimport in.comment;\n*/\nconst char* text = R"x(\nimport in.raw;\n)x";\nint main() { return DEVKIT_MARKER - 1; }\n' > tests/test_smoke.cpp

tree_digest() {
    "$PY" - "$TMP" <<'EOF'
import hashlib, os, sys
root = sys.argv[1]
h = hashlib.sha256()
for dirpath, dirnames, filenames in os.walk(root):
    dirnames.sort()
    rel = os.path.relpath(dirpath, root)
    h.update(("D " + rel + "\n").encode())
    for name in sorted(filenames):
        with open(os.path.join(dirpath, name), "rb") as f:
            h.update(("F " + os.path.join(rel, name) + " ").encode() + hashlib.sha256(f.read()).hexdigest().encode() + b"\n")
print(h.hexdigest())
EOF
}

before=$(tree_digest)

# ── A ──────────────────────────────────────────────────────────────────────
"$MCPP" emit build-database --format json > "$OUT/env.json" 2> "$OUT/env.err" \
    || fail "A: emit build-database exited non-zero" "$OUT/env.err" "$OUT/env.json"
after=$(tree_digest)

"$PY" - "$OUT/env.json" <<'EOF' || fail "A: the envelope" "$OUT/env.json"
import json, sys
e = json.load(open(sys.argv[1]))
assert e["kind"] == "mcpp.build-database", e["kind"]
assert e["kindVersion"] == 1, e["kindVersion"]
assert "write-project" not in e["effects"], e["effects"]
assert "read-project" in e["effects"], e["effects"]
d = e["data"]
assert d["spec"] == {"name": "s1", "version": "0.2.0"}, d["spec"]
assert d["database"]["ide"]["profile-version"] == "0.2.0"
assert d["inputs-fingerprint"].startswith("fnv1a:"), d["inputs-fingerprint"]
EOF
"$MCPP" --protocol-version > "$OUT/proto.json"
"$PY" - "$OUT/proto.json" <<'EOF' || fail "A: the protocol document" "$OUT/proto.json"
import json, sys
p = json.load(open(sys.argv[1]))
assert p["kinds"]["mcpp.build-database"] == 1
fx = p["commands"]["emit build-database"]["effects"]
assert "write-project" not in fx and "exec-build-script" in fx, fx
EOF
echo "ok: A, the envelope and the declared effects"

# ── B ──────────────────────────────────────────────────────────────────────
[ "$before" = "$after" ] || fail "B: the project tree changed"
echo "ok: B, the project tree is unchanged"

# ── C ──────────────────────────────────────────────────────────────────────
"$PY" -c 'import json,sys; json.dump(json.load(open(sys.argv[1]))["data"]["database"], open(sys.argv[2], "w"))' \
    "$OUT/env.json" "$OUT/db.json"
"$PY" "$VALIDATE" "$SCHEMA" "$OUT/db.json" > "$OUT/validate.out" \
    || fail "C: the document does not validate against S1 0.2.0" "$OUT/validate.out"
echo "ok: C, the document validates against the S1 schema"

# ── D, E ───────────────────────────────────────────────────────────────────
"$PY" - "$OUT/env.json" <<'EOF' || fail "D/E: sets and roles" "$OUT/env.json"
import json, os, sys
db = json.load(open(sys.argv[1]))["data"]["database"]
sets = {s["name"]: s for s in db["sets"]}
assert set(sets) == {"hello", "hello:test", "mcpp:std"}, sorted(sets)
for name, s in sets.items():
    assert sorted(s["visible-sets"]) == sorted(n for n in sets if n != name), (name, s["visible-sets"])
    assert s["ide"]["toolchain"] in db["ide"]["toolchains"], s["ide"]
want = {
    "greet.cppm": "module-interface",
    "detail.cppm": "module-partition-interface",
    "impl.cppm": "module-partition-implementation",
    "greet_impl.cpp": "module-implementation",
    "main.cpp": "non-module",
}
roles = {os.path.basename(u["source"]): u["ide"]["role"] for u in sets["hello"]["translation-units"]}
assert roles == want, roles
assert sets["hello"]["ide"]["kind"] == "executable", sets["hello"]["ide"]
tests = [os.path.basename(u["source"]) for u in sets["hello:test"]["translation-units"]]
assert tests == ["test_smoke.cpp"], tests
assert sets["hello:test"]["ide"]["kind"] == "test"
greet = next(u for u in sets["hello"]["translation-units"] if u["source"].endswith("greet.cppm"))
assert greet["provides"] == {"hello.greet": ""}, greet["provides"]
assert greet["requires"] == ["hello.greet:detail", "std"], greet["requires"]
std = [u for u in sets["mcpp:std"]["translation-units"] if "std" in u["provides"]]
assert len(std) == 1, sets["mcpp:std"]
assert os.path.isfile(std[0]["source"]), std[0]["source"]
driver = next(iter(db["ide"]["toolchains"].values()))["driver"]
assert std[0]["arguments"][0] == driver, (std[0]["arguments"][0], driver)
assert std[0]["source"] in std[0]["arguments"] or any(a.endswith(os.path.basename(std[0]["source"])) for a in std[0]["arguments"]), std[0]["arguments"]
EOF
echo "ok: D, roles and sets; E, the std unit"

# ── J, K ───────────────────────────────────────────────────────────────────
"$PY" - "$OUT/env.json" <<'EOF' || fail "J/K: argument decomposition, config-files, test scan" "$OUT/env.json"
import json, os, sys
db = json.load(open(sys.argv[1]))["data"]["database"]
def same(word, path, cwd):
    return os.path.normpath(os.path.join(cwd, word)) == os.path.normpath(path)
bypassed = False
for s in db["sets"]:
    base = s["baseline-arguments"]
    for u in s["translation-units"]:
        a, local = u["arguments"], u["local-arguments"]
        head = [a[0]] + base + local
        assert a[:len(head)] == head, (s["name"], u["source"], base, local, a)
        tail = a[len(head):]
        assert tail == [] or (len(tail) == 4 and tail[0] == "-c" and tail[2] == "-o"
                              and same(tail[1], u["source"], u["work-directory"])
                              and same(tail[3], u["object"], u["work-directory"])), (u["source"], tail)
        assert u["private"] is False, u
        bypassed = bypassed or "--no-default-config" in a
hello = next(s for s in db["sets"] if s["name"] == "hello")
assert "-std=c++23" in hello["baseline-arguments"] or "/std:c++latest" in hello["baseline-arguments"], hello["baseline-arguments"]
for tid, t in db["ide"]["toolchains"].items():
    for f in t["config-files"]:
        assert os.path.isabs(f) and os.path.isfile(f), (tid, f)
        assert not (bypassed and f.endswith(".cfg")), (tid, f)
test = next(u for s in db["sets"] if s["name"] == "hello:test" for u in s["translation-units"])
assert test["requires"] == [], test["requires"]
EOF
echo "ok: J, baseline and local arguments, private, config-files; K, the test's imports"

# ── F (the S1 and compile-commands renderings agree) ──────────────────────
"$MCPP" emit build-database --spec compile-commands > "$OUT/cc.json" 2> "$OUT/cc.err" \
    || fail "F: --spec compile-commands exited non-zero" "$OUT/cc.err"
"$PY" - "$OUT/env.json" "$OUT/cc.json" <<'EOF' || fail "F: the two renderings disagree" "$OUT/cc.json"
import json, sys
db = json.load(open(sys.argv[1]))["data"]["database"]
cc = json.load(open(sys.argv[2]))
s1 = {u["source"]: u["arguments"] for s in db["sets"] if s["name"] != "mcpp:std" for u in s["translation-units"]}
cdb = {e["file"]: e["arguments"] for e in cc}
assert s1 == cdb, (sorted(s1), sorted(cdb))
EOF
echo "ok: F, the S1 units and the compile-commands entries carry the same arguments"

# ── G ──────────────────────────────────────────────────────────────────────
set +e
"$MCPP" emit build-database --format ndjson > "$OUT/g1.out" 2> "$OUT/g1.err"; rc1=$?
"$MCPP" emit build-database --spec nope > "$OUT/g2.out" 2> "$OUT/g2.err"; rc2=$?
mkdir -p "$OUT/empty" && (cd "$OUT/empty" && "$MCPP" emit build-database --format json > "$OUT/g3.out" 2> "$OUT/g3.err"); rc3=$?
set -e
[ "$rc1" = 2 ] && [ ! -s "$OUT/g1.out" ] || fail "G: --format ndjson rc=$rc1" "$OUT/g1.out" "$OUT/g1.err"
[ "$rc2" = 2 ] && [ ! -s "$OUT/g2.out" ] || fail "G: --spec nope rc=$rc2" "$OUT/g2.out" "$OUT/g2.err"
[ "$rc3" = 1 ] || fail "G: outside a project rc=$rc3" "$OUT/g3.out" "$OUT/g3.err"
"$PY" - "$OUT/g3.out" <<'EOF' || fail "G: the failure envelope" "$OUT/g3.out"
import json, sys
e = json.load(open(sys.argv[1]))
assert "data" not in e, e
assert e["diagnostics"][0]["code"] == "MCPP_BUILD_DATABASE_NO_PROJECT", e["diagnostics"]
assert e["diagnostics"][0]["severity"] == "error"
EOF
echo "ok: G, usage errors and the failure envelope"

# ── H ──────────────────────────────────────────────────────────────────────
"$MCPP" emit build-database > "$OUT/bare.json" 2> "$OUT/bare.err" || fail "H: bare document" "$OUT/bare.err"
"$MCPP" emit build-database -o "$OUT/file.json" > "$OUT/o.out" 2> "$OUT/o.err" || fail "H: -o" "$OUT/o.err"
[ ! -s "$OUT/o.out" ] || fail "H: -o also wrote to stdout" "$OUT/o.out"
"$PY" - "$OUT/bare.json" "$OUT/file.json" <<'EOF' || fail "H: bare and -o documents" "$OUT/bare.json" "$OUT/file.json"
import json, sys
a = json.load(open(sys.argv[1])); b = json.load(open(sys.argv[2]))
assert a["version"] == 1 and "sets" in a and a == b
EOF
echo "ok: H, the bare document and -o"

# ── I ──────────────────────────────────────────────────────────────────────
"$MCPP" emit build-database --format json > "$OUT/env2.json" 2> /dev/null || fail "I: second run"
printf '\n// edited\n' >> src/detail.cppm
"$MCPP" emit build-database --format json > "$OUT/env3.json" 2> /dev/null || fail "I: after an edit"
"$PY" - "$OUT/env.json" "$OUT/env2.json" "$OUT/env3.json" <<'EOF' || fail "I: watch and fingerprint" "$OUT/env.json"
import json, sys
e1, e2, e3 = (json.load(open(p))["data"] for p in sys.argv[1:4])
w = e1["watch"]
for entry in ("mcpp.toml", "mcpp.lock", "tests/**/*.cpp"):
    assert entry in w, (entry, w)
assert any(x.startswith("src/**/") for x in w), w
assert any(x.replace("\\", "/").endswith("devkit/mcpp.toml") for x in w), w
assert e1["inputs-fingerprint"] == e2["inputs-fingerprint"], "the fingerprint is not stable"
assert e1["inputs-fingerprint"] != e3["inputs-fingerprint"], "the fingerprint did not follow an edit"
EOF
echo "ok: I, watch and the inputs fingerprint"

# The work directory the planning used, for cleanup: the object paths are under
# it, and it is not inside the project.
WORK_DIR=$("$PY" -c '
import json, sys
db = json.load(open(sys.argv[1]))["data"]["database"]
obj = next(u["object"] for s in db["sets"] if s["name"] == "hello" for u in s["translation-units"])
norm = obj.replace("\\", "/")
print(obj[: norm.index("/target/")])
' "$OUT/env.json")

# ── L ──────────────────────────────────────────────────────────────────────
[ -n "$(find "$WORK_DIR" -type f -name resolution.json 2>/dev/null)" ] \
    || fail "L: no resolution record under the work directory $WORK_DIR"
written=$(find "$WORK_DIR" -type f \( -name '*.o' -o -name '*.obj' -o -name '*.gcm' \
               -o -name '*.pcm' -o -name '*.ifc' -o -name 'mcpp-clean-link.specs' \) | head -5)
[ -z "$written" ] || fail "L: the planning pass wrote compile or link outputs: $written"

# A home with the machine's toolchain payloads and an empty build cache.
COLD="$OUT/cold-home"
compiled_in_cold_home() {
    find "$COLD" -path "$COLD/registry" -prune -o -type f \( -name '*.o' -o -name '*.obj' \
         -o -name '*.gcm' -o -name '*.pcm' -o -name '*.ifc' \) -print | head -5
}
(
    export MCPP_HOME="$COLD" MCPP_OFFLINE=1
    source "$HERE/_inherit_toolchain.sh"
    "$MCPP" emit build-database --format json > "$OUT/cold.json" 2> "$OUT/cold.err"
) || fail "L: emit in a home with a cold std cache" "$OUT/cold.err"
"$PY" - "$OUT/cold.json" <<'EOF' || fail "L: the cold-home document" "$OUT/cold.json"
import json, sys
db = json.load(open(sys.argv[1]))["data"]["database"]
std = [u for s in db["sets"] if s["name"] == "mcpp:std" for u in s["translation-units"] if "std" in u["provides"]]
assert len(std) == 1, db["sets"]
EOF
written=$(compiled_in_cold_home)
[ -z "$written" ] || fail "L: emit compiled into a cold home: $written" "$OUT/cold.err"
echo "ok: L, the planning pass wrote no object, BMI or link input, and a cold std cache stays cold"

# ── F (against configure-only) and the control leg of B ───────────────────
"$MCPP" build --configure-only > "$OUT/conf.out" 2>&1 || fail "F: configure-only" "$OUT/conf.out"
[ "$(tree_digest)" != "$before" ] || fail "B control: configure-only did not change the tree"
[ -f compile_commands.json ] || fail "F: configure-only wrote no compile_commands.json" "$OUT/conf.out"
"$PY" - "$OUT/cc.json" compile_commands.json <<'EOF' || fail "F: compile-commands differs from configure-only" compile_commands.json
import json, sys
emitted = json.load(open(sys.argv[1]))
written = json.load(open(sys.argv[2]))
# The one difference by construction is where the build writes: the planning
# pass writes under its work directory, configure-only under the project.
def slash(text):
    # One spelling for the comparison: a Windows argument may name a path with
    # either separator, and the mapping below is textual.
    return text.replace("\\", "/")
def write_root(entry):
    return slash(entry["output"]).split("/target/")[0]
work, project = write_root(emitted[0]), write_root(written[0])
def mapped(args):
    return [slash(a).replace(work, project) for a in args]
e = {slash(x["file"]): mapped(x["arguments"]) for x in emitted}
w = {slash(x["file"]): [slash(a) for a in x["arguments"]] for x in written}
assert set(e) <= set(w), (sorted(e), sorted(w))
for f in e:
    assert e[f] == w[f], (f, e[f], w[f])
EOF
echo "ok: F, the arguments are configure-only's; B control, configure-only writes the project"

# ── the control leg of L ──────────────────────────────────────────────────
mkdir -p "$OUT/copy"
cp -R "$TMP/hello" "$TMP/devkit" "$OUT/copy/"
rm -rf "$OUT/copy/hello/target" "$OUT/copy/hello/compile_commands.json"
(
    export MCPP_HOME="$COLD" MCPP_OFFLINE=1
    cd "$OUT/copy/hello" && "$MCPP" build --configure-only > "$OUT/cold-conf.out" 2>&1
) || fail "L control: configure-only in the cold home" "$OUT/cold-conf.out"
[ -n "$(compiled_in_cold_home)" ] || fail "L control: configure-only compiled nothing into the home" "$OUT/cold-conf.out"
echo "ok: L control, configure-only compiles the std module into the same home"
