#!/usr/bin/env bash
# requires: gcc
# 711 -- a `[feature-deps.<f>]` entry that adds `tools` to a dependency declared
# unconditionally restates that dependency's source, and a restatement naming
# another source is refused (#647 E4.2). A build program addresses the tool of a
# package written `namespace = "spike"`, `name = "installer"` by the qualified
# name as well as the bare one (#647 E4.3).
#
# The grammar requires the source in a dependency table, so a source-less entry
# is refused by the parser, whose message prescribes restating it. The merge
# took only the additive fields of a restatement: one naming another path was
# ignored without a word, under `--strict` too. And the tool variable was
# published under `package.name` alone, so `dep_bin("spike.installer", ...)`
# read nothing while `dep_dir("spike.installer")` answered.
#
# Readings: the source-less entry is refused and names the remedy; the
# restatement with the same source builds the tool only under the feature, and
# the build program sees both spellings; a restatement naming another path is
# refused under `--strict` naming both sources.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
if [ -d "$HOME/.mcpp/registry" ]; then
    ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fi

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p installer/src other/src app/src
for d in installer other; do
cat > $d/mcpp.toml <<'EOF'
[package]
name      = "installer"
namespace = "spike"
version   = "0.1.0"

[targets.installer]
kind = "bin"
main = "src/main.cpp"
EOF
cat > $d/src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("installer ran"); return 0; }
EOF
done

cat > app/mcpp.toml <<'EOF'
[package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.installer = { path = "../installer" }

[features]
installer = []

[feature-deps.installer]
spike.installer = { tools = ["installer"] }
EOF
echo 'int main() { return 0; }' > app/src/main.cpp
cat > app/build.mcpp <<'EOF'
import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("installer", "installer");
    const char* b = mcpp::dep_bin("spike.installer", "installer");
    mcpp::warning((std::string("DEPBIN short=[") + (a ? a : "") + "] qualified=["
                   + (b ? b : "") + "]").c_str());
    return 0;
}
EOF

cd "$TMP/app"

# ── 1. a source-less entry is refused, and the refusal names the remedy ──
if "$MCPP" build --features installer > b1.log 2>&1; then
    fail "a source-less [feature-deps] entry was accepted" b1.log
fi
grep -q "source" b1.log || fail "the refusal does not say to restate the source" b1.log

# ── 2. the same source restated: the tool, both spellings, only under the feature
sed -i.bak 's|spike.installer = { tools = \["installer"\] }|spike.installer = { path = "../installer", tools = ["installer"] }|' mcpp.toml
"$MCPP" build --strict --features installer > b2.log 2>&1 \
    || fail "the restated feature-deps entry was refused under --strict" b2.log
grep -q 'DEPBIN short=\[[^]]' b2.log || fail "dep_bin(\"installer\") read nothing" b2.log
grep -q 'qualified=\[[^]]' b2.log \
    || fail "dep_bin(\"spike.installer\") read nothing for a namespace + name package" b2.log
rm -rf target
"$MCPP" build --strict > b3.log 2>&1 || fail "the build without the feature failed" b3.log
grep -q 'DEPBIN short=\[\] qualified=\[\]' b3.log \
    || fail "the tool was provided without its feature" b3.log

# ── 3. a restatement naming another source is refused, naming both ───────
sed -i.bak 's|spike.installer = { path = "../installer", tools|spike.installer = { path = "../other", tools|' mcpp.toml
rm -rf target
if "$MCPP" build --strict --features installer > b4.log 2>&1; then
    fail "a restatement naming another path was accepted" b4.log
fi
grep -q '../other' b4.log && grep -q '../installer' b4.log \
    || fail "the refusal does not name both sources" b4.log

echo "PASS: 711 a feature-deps restatement names one source; dep_bin answers both spellings"
