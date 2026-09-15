#!/usr/bin/env bash
# requires: python3
# 720 -- the root package's build program reads the resolved dependency graph
# and each package's `[package.metadata]` through `mcpp::graph_file()` (#647 E1).
#
# A framework merges what its libraries contribute (resources, platform
# sources) at build time, in dependency order, including libraries the
# application does not name. `dep_dir` answers by name for a direct dependency
# only, so a transitive library had no name to be asked for.
#
# Legs:
#   A. app -> spike.a -> spike.b: the document lists b before a before app, the
#      root last; b's entry carries its absolute manifest directory, its target
#      kind and `metadata.demo.resources = "res"` verbatim.
#   B. A second build with nothing changed does not re-run the root program.
#   C. Editing b's `[package.metadata]` re-runs the root program, and the
#      document states the new value.
#   D. Editing b's source does not re-run the root program.
#   E. A dependency's program reads "" from graph_file().
#   F. `[package]` reports an unknown key the way `[build]` does: a warning,
#      an error under --strict; `metadata` is a known key.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/b/src" "$TMP/a/src" "$TMP/app/src"

write_b() {  # write_b <resources value>
    cat > "$TMP/b/mcpp.toml" <<EOF
[package]
name      = "b"
namespace = "spike"
version   = "0.2.0"

[package.metadata.demo]
resources = "$1"
languages = ["en", "zh"]

[targets.b]
kind = "lib"
EOF
}
write_b res
printf 'int b_answer() { return 40; }\n' > "$TMP/b/src/b.cpp"

cat > "$TMP/a/mcpp.toml" <<'EOF'
[package]
name      = "a"
namespace = "spike"
version   = "0.1.0"

[dependencies]
spike.b = { path = "../b" }

[targets.a]
kind = "lib"
EOF
printf 'int b_answer();\nint a_answer() { return b_answer() + 1; }\n' > "$TMP/a/src/a.cpp"
# The dependency's program: graph_file() is not offered to it.
cat > "$TMP/a/build.mcpp" <<'EOF'
#include <cstdio>
import mcpp;
int main() {
    const char* g = mcpp::graph_file();
    if (g != nullptr && g[0] != '\0') {
        std::fprintf(stderr, "a: a dependency's program was offered graph_file()=%s\n", g);
        return 1;
    }
    return 0;
}
EOF

cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
spike.a = { path = "../a" }
EOF
printf 'int a_answer();\nint main() { return a_answer() == 41 ? 0 : 1; }\n' > "$TMP/app/src/main.cpp"
# The root's program copies the document it was given, one copy per run.
cat > "$TMP/app/build.mcpp" <<'EOF'
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
import mcpp;
int main() {
    const char* g = mcpp::graph_file();
    if (g == nullptr || g[0] == '\0') {
        std::fprintf(stderr, "app: graph_file() is empty\n");
        return 1;
    }
    std::ifstream in(g, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string dir = mcpp::manifest_dir();
    int n = 0;
    for (;; ++n) {
        std::ifstream probe(dir + "/graph-" + std::to_string(n) + ".json");
        if (!probe) break;
    }
    std::ofstream(dir + "/graph-" + std::to_string(n) + ".json", std::ios::binary) << ss.str();
    return 0;
}
EOF

cd "$TMP/app"
runs() { ls graph-*.json 2>/dev/null | wc -l | tr -d ' '; }

# ── A, E ────────────────────────────────────────────────────────────────────
"$MCPP" build > a.log 2>&1 || fail "A: build failed" a.log
[ "$(runs)" -eq 1 ] || fail "A: the root program ran $(runs) times" a.log
python3 - "$TMP" graph-0.json res <<'EOF' || fail "A: the graph document" graph-0.json
import json, os, sys
tmp, path, want = sys.argv[1], sys.argv[2], sys.argv[3]
d = json.load(open(path))
names = [p["package"]["canonical"] for p in d["packages"]]
assert names == ["spike.b@0.2.0", "spike.a@0.1.0", "mcpplibs.app@0.1.0"], names
b, a, app = d["packages"]
assert app["root"] is True and b["root"] is False, (app["root"], b["root"])
assert os.path.isabs(b["manifest_dir"]), b["manifest_dir"]
assert os.path.realpath(b["manifest_dir"]) == os.path.realpath(os.path.join(tmp, "b")), b["manifest_dir"]
assert b["targets"] == [{"name": "b", "kind": "lib"}], b["targets"]
assert b["metadata"] == {"demo": {"resources": want, "languages": ["en", "zh"]}}, b["metadata"]
assert a["metadata"] == {}, a["metadata"]
assert [r["requester"] for r in b["requested_by"]] == ["spike.a@0.1.0"], b["requested_by"]
assert isinstance(b["features"], list)
EOF
echo "ok: A, the document lists the graph in dependency order with metadata"
echo "ok: E, a dependency's program is not offered the document"

# ── B ───────────────────────────────────────────────────────────────────────
"$MCPP" build > b.log 2>&1 || fail "B: build failed" b.log
[ "$(runs)" -eq 1 ] || fail "B: an unchanged build re-ran the root program" b.log
echo "ok: B, an unchanged build does not re-run the root program"

# ── C ───────────────────────────────────────────────────────────────────────
write_b assets
"$MCPP" build > c.log 2>&1 || fail "C: build failed" c.log
[ "$(runs)" -eq 2 ] || fail "C: editing a dependency's metadata did not re-run the root program" c.log
python3 - "$TMP" graph-1.json assets <<'EOF' || fail "C: the document after the edit" graph-1.json
import json, sys
d = json.load(open(sys.argv[2]))
b = d["packages"][0]
assert b["metadata"]["demo"]["resources"] == sys.argv[3], b["metadata"]
EOF
echo "ok: C, a metadata edit re-runs the root program with the new value"

# ── D ───────────────────────────────────────────────────────────────────────
printf 'int b_answer() { return 39 + 1; }\n' > "$TMP/b/src/b.cpp"
"$MCPP" build > d.log 2>&1 || fail "D: build failed" d.log
[ "$(runs)" -eq 2 ] || fail "D: editing a dependency's source re-ran the root program" d.log
echo "ok: D, a source edit does not re-run the root program"

# ── F ───────────────────────────────────────────────────────────────────────
cat >> "$TMP/app/mcpp.toml" <<'EOF'
EOF
python3 - "$TMP/app/mcpp.toml" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read().replace('version = "0.1.0"\n', 'version = "0.1.0"\nlicence = "MIT"\n', 1)
open(p, "w").write(s)
EOF
"$MCPP" build > f.log 2>&1 || fail "F: build failed on a warning" f.log
grep -q "\[package\] has unsupported key 'licence'" f.log || fail "F: no warning for an unknown [package] key" f.log
if "$MCPP" build --strict > f2.log 2>&1; then fail "F: --strict accepted an unknown [package] key" f2.log; fi
grep -q "\[package\] has unsupported key 'licence'" f2.log || fail "F: --strict failed for another reason" f2.log
grep -q "unsupported key 'metadata'" a.log c.log f.log && fail "F: metadata reported as unknown" a.log
echo "ok: F, [package] reports an unknown key and knows metadata"

echo "PASS: 720"
