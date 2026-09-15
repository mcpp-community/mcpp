#!/usr/bin/env bash
# requires: gcc
# 713 -- a git dependency whose key names a package other than the repository's
# root package selects the `[workspace] members` entry that declares it
# (#649 E7). A second declaration of one dependency by one consumer keeps its
# requests, and the compile banner of a git dependency names its reference.
#
# A framework repository holds the framework, its rules and its tools, and a
# consumer pins it by revision before it is published. The git source always
# yielded the root package, so a key naming a member was either refused as a
# name mismatch or, beside a key naming the root, adopted the root's identity
# with a warning and lost its `tools` request. The same early return dropped
# the requests of a dependency named in `[dependencies]` and again in
# `[build-dependencies]`.
#
# Readings: the member alone builds its tool; the root and the member by the
# same revision build without an identity warning and with the member's tool;
# the banner prints the reference instead of an empty version; a dependency
# declared in both tables gets the tool the second declaration requests.
set -e

source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

export MCPP_HOME="$TMP/mcpphome"
mkdir -p "$MCPP_HOME"
if [ -d "$HOME/.mcpp/registry" ]; then
    ln -s "$HOME/.mcpp/registry" "$MCPP_HOME/registry"
fi

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

command -v git >/dev/null || { echo "SKIP: git is not installed"; exit 0; }

mkdir -p repo/src repo/tool/src
cat > repo/mcpp.toml <<'EOF'
[package]
name      = "fw"
namespace = "spike"
version   = "0.1.0"

[targets.fw]
kind = "lib"

[build]
sources = ["src/*.cpp"]

[workspace]
members = ["tool"]
EOF
echo 'int fw_answer() { return 42; }' > repo/src/fw.cpp
cat > repo/tool/mcpp.toml <<'EOF'
[package]
name      = "fw-installer"
namespace = "spike"
version   = "0.1.0"

[targets.fw-installer]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.fw = { path = ".." }
EOF
cat > repo/tool/src/main.cpp <<'EOF'
#include <cstdio>
int fw_answer();
int main() { std::printf("fw-installer ran %d\n", fw_answer()); return 0; }
EOF
(
    cd repo
    git init -q
    git -c user.email=e2e@mcpp.dev -c user.name=e2e add -A
    git -c user.email=e2e@mcpp.dev -c user.name=e2e commit -qm fixture
)
REV=$(git -C repo rev-parse HEAD)
REPO_HOST=$(host_path "$TMP/repo")

write_app() {
    mkdir -p "$TMP/$1/src"
    cat > "$TMP/$1/mcpp.toml"
    echo 'int main() { return 0; }' > "$TMP/$1/src/main.cpp"
    cat > "$TMP/$1/build.mcpp" <<'EOF'
import std;
import mcpp;
int main() {
    const char* a = mcpp::dep_bin("spike.fw-installer", "fw-installer");
    mcpp::warning((std::string("DEPBIN tool=[") + (a ? a : "") + "]").c_str());
    return 0;
}
EOF
}

# ── 1. the member alone ─────────────────────────────────────────────────
write_app member <<EOF
[package]
name    = "member"
version = "0.1.0"

[targets.member]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.fw-installer = { git = "file://$REPO_HOST", rev = "$REV", tools = ["fw-installer"] }
EOF
cd "$TMP/member"
"$MCPP" build > b1.log 2>&1 || fail "a git dependency naming a repository member was refused" b1.log
tool=$(grep -o 'DEPBIN tool=\[[^]]*' b1.log | sed 's/DEPBIN tool=\[//')
[ -n "$tool" ] || fail "the member's tool was not provided" b1.log
[ "$("$tool")" = "fw-installer ran 42" ] || fail "the member's tool did not run"

# ── 2. the root and the member by the same revision ─────────────────────
write_app both <<EOF
[package]
name    = "both"
version = "0.1.0"

[targets.both]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.fw = { git = "file://$REPO_HOST", rev = "$REV" }
spike.fw-installer = { git = "file://$REPO_HOST", rev = "$REV", tools = ["fw-installer"] }
EOF
cat > "$TMP/both/src/main.cpp" <<'EOF'
int fw_answer();
int main() { return fw_answer() == 42 ? 0 : 1; }
EOF
cd "$TMP/both"
"$MCPP" build > b2.log 2>&1 || fail "the root and a member by one revision did not build" b2.log
grep -q "that identity is used" b2.log \
    && fail "the member's key adopted the root's identity" b2.log
grep -q 'DEPBIN tool=\[[^]]' b2.log || fail "the member's tools request was lost" b2.log
grep -q "Compiling spike.fw v$" b2.log && fail "the git banner printed an empty version" b2.log
grep -q "spike.fw (git rev ${REV:0:12})" b2.log \
    || fail "the git banner does not name the reference" b2.log
"$MCPP" run > r2.log 2>&1 || fail "the application over the git root does not run" r2.log

# ── 3. one dependency in [dependencies] and in [build-dependencies] ─────
mkdir -p "$TMP/local/src"
cp -r "$TMP/repo/tool/." "$TMP/local/"
sed -i.bak 's|spike.fw = { path = ".." }||' "$TMP/local/mcpp.toml"
cat > "$TMP/local/src/main.cpp" <<'EOF'
#include <cstdio>
int main() { std::puts("local tool"); return 0; }
EOF
write_app twice <<'EOF'
[package]
name    = "twice"
version = "0.1.0"

[targets.twice]
kind = "bin"
main = "src/main.cpp"

[dependencies]
spike.fw-installer = { path = "../local" }

[build-dependencies]
spike.fw-installer = { path = "../local", tools = ["fw-installer"] }
EOF
cd "$TMP/twice"
"$MCPP" build > b3.log 2>&1 || fail "a dependency declared in two tables did not build" b3.log
grep -q 'DEPBIN tool=\[[^]]' b3.log \
    || fail "the second declaration's tools request was dropped" b3.log

echo "PASS: 713 a git dependency selects a repository member; a second declaration keeps its requests"
