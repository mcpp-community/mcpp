#!/usr/bin/env bash
# requires: unix-shell python3
# 817 -- `emit build-database` writes nothing into a project that declares
# `[xlings] deps` (mcpp#724 side finding B, design 2026-09-27 §4.3, SPEC-005
# R2.1).
#
# e2e 688 already repeats a project-tree digest across `emit build-database`,
# but its fixture declares no `[xlings]` payloads, so the branch that writes
# `<root>/.mcpp/.xlings.json` (`ensure_project_index_dir`, reached through
# `src/build/prepare/xlings.cpp`) is never taken there -- the criterion missed
# the case that mattered. This repeats it on a fixture that does declare
# `[xlings] deps`, with a stub xlings as in e2e 733, so it needs no network.
#
# The call site used the private `work_dir` (`emit`'s planning cache under
# `$MCPP_HOME/cache/build-database/<key>`) for the custom-indices half of that
# file only when `runtimeSelection.ownerRoot == workRoot`; `ownerRoot` is
# always the real project root, so under `emit`'s private work_dir the
# runtime-environment half (deps/subos/workspace) went to the project instead.
# Criteria:
#   A. the project tree is byte-identical before and after `emit
#      build-database`.
#   B. `<root>/.mcpp/.xlings.json` does not exist afterwards.
#   C. the envelope's `effects` do not include `write-project`.
#   D. the private work directory DOES gain a `.xlings.json` naming the
#      declared dependency -- so the fix is "written at the private root",
#      not "never written at all", and xlings can still resolve it there.
set -e

TMP=$(mktemp -d)   # the measured tree
OUT=$(mktemp -d)   # everything this script writes, outside the measured tree
cleanup() { rm -rf "$TMP" "$OUT"; }
trap cleanup EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
PY=python3

export MCPP_HOME="$OUT/home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$OUT/bin"
cat > "$OUT/bin/xlings" <<'EOF'
#!/usr/bin/env bash
# The handshake mcpp asks of every xlings (docs/25 §5).
[[ "${1:-}" == "interface" && "${2:-}" == "--version" ]] && { echo '{"protocol_version":"1.13","version":"9999.1.1.1","capabilities":["install_packages"],"home_layouts":[2],"features":["index_dir","index_store","upstreams","payload_status","sync_index","payload_currency","binding_state","network_diagnostics","download_socket_budgets","read_only_queries"]}'; exit 0; }
echo "$*" >> "${STUB_LOG:?}"
exit 0
EOF
chmod +x "$OUT/bin/xlings"
{
    grep -v '^binary' "$MCPP_HOME/config.toml" 2>/dev/null | sed '/^\[xlings\]/d'
    printf '\n[xlings]\nbinary = "%s"\n' "$OUT/bin/xlings"
} > "$OUT/config.toml"
mv "$OUT/config.toml" "$MCPP_HOME/config.toml"

mkdir -p "$TMP/proj/src"
cat > "$TMP/proj/mcpp.toml" <<'EOF'
[package]
name    = "proj817"
version = "0.1.0"

[xlings]
deps = ["definitely-not-a-real-package-817"]
EOF
echo 'int main() { return 0; }' > "$TMP/proj/src/main.cpp"
cd "$TMP/proj"

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

STUB_LOG="$OUT/xlings.log" "$MCPP" emit build-database --format json \
    > "$OUT/env.json" 2> "$OUT/env.err" \
    || fail "emit build-database exited non-zero" "$OUT/env.err" "$OUT/env.json"

after=$(tree_digest)

# ── A ──────────────────────────────────────────────────────────────────────
[ "$before" = "$after" ] || fail "A: the project tree changed"
echo "ok: A, the project tree is byte-identical before and after"

# ── B ──────────────────────────────────────────────────────────────────────
[ ! -e "$TMP/proj/.mcpp/.xlings.json" ] \
    || fail "B: emit wrote <root>/.mcpp/.xlings.json"
echo "ok: B, no .mcpp/.xlings.json in the project"

# ── C ──────────────────────────────────────────────────────────────────────
"$PY" - "$OUT/env.json" <<'EOF' || fail "C: the envelope" "$OUT/env.json"
import json, sys
e = json.load(open(sys.argv[1]))
assert "write-project" not in e["effects"], e["effects"]
EOF
echo "ok: C, the envelope reports no write-project effect"

# ── D ──────────────────────────────────────────────────────────────────────
work_dir=$("$PY" -c '
import json, sys
db = json.load(open(sys.argv[1]))["data"]["database"]
for s in db["sets"]:
    for u in s["translation-units"]:
        norm = u["object"].replace("\\", "/")
        if "/target/" in norm:
            print(norm.split("/target/")[0])
            sys.exit(0)
sys.exit(1)
' "$OUT/env.json") || fail "D: could not recover the private work directory" "$OUT/env.json"
found=$(find "$work_dir" -name ".xlings.json" 2>/dev/null | head -1)
[ -n "$found" ] || fail "D: no .xlings.json under the private work directory $work_dir"
grep -q 'definitely-not-a-real-package-817' "$found" \
    || fail "D: the private .xlings.json does not name the declared dependency" "$found"
echo "ok: D, the runtime environment is written at the private root instead, where xlings resolves it from"

echo "PASS: 817 emit writes no .xlings.json into the project"
