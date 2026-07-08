#!/usr/bin/env bash
# requires:
# 93_xpkg_parse.sh — `mcpp xpkg parse`: descriptor grammar v2 (long
# brackets) + strict-by-default unknown-key detection + --json.
#
# Single-source-of-truth lint: this command parses with EXACTLY the
# resolver's grammar, so what passes here is what builds for users.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── 1. valid descriptor using BOTH string spellings + block comment ──
cat > good.lua <<'EOF'
--[==[ decoy block comment: mcpp = { sources = { "WRONG" } } }}}} ]==]
package = {
    spec      = "1",
    namespace = "demo",
    name      = "demo.gen",
    xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        schema  = "0.1",
        sources = { [[mcpp_generated/gen.cc]] },
        generated_files = {
            ["mcpp_generated/gen.cc"] = [==[
export module gen;
int f() { return "x]]y"[0]; }
]==],
        },
        targets = { ["gen"] = { kind = "lib" } },
    },
}
EOF

"$MCPP" xpkg parse good.lua | tee parse.out
grep -q "parse OK" parse.out
grep -q "demo.gen" parse.out
grep -q "34 bytes\|generated" parse.out

# ── 2. --json round-trips the generated content verbatim ──
"$MCPP" xpkg parse --json good.lua > parse.json
python3 - <<'PY'
import json
j = json.load(open("parse.json"))
assert j["namespace"] == "demo" and j["name"] == "gen", j
c = j["generated_contents"]["mcpp_generated/gen.cc"]
assert c == 'export module gen;\nint f() { return "x]]y"[0]; }\n', repr(c)
assert j["unknown_keys"] == [], j
print("json ok")
PY

# ── 3. unknown key: error by default, warning with --allow-unknown ──
sed 's/schema  = "0.1",/schema = "0.1", bogus_key = "x",/' good.lua > bad.lua
if "$MCPP" xpkg parse bad.lua > /dev/null 2>&1; then
    echo "FAIL: unknown key should be an error by default"; exit 1
fi
"$MCPP" xpkg parse bad.lua 2>&1 | grep -q "unknown mcpp-segment key 'bogus_key'"
"$MCPP" xpkg parse --allow-unknown bad.lua > /dev/null

# ── 4. malformed segment: exact resolver error surfaces ──
cat > broken.lua <<'EOF'
package = {
    spec = "1",
    name = "broken",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = { sources = { },
}
EOF
if "$MCPP" xpkg parse broken.lua > /dev/null 2>&1; then
    echo "FAIL: broken descriptor should fail"; exit 1
fi

echo "PASS 93_xpkg_parse"
