#!/usr/bin/env bash
# requires: gcc
# 944 -- a dependency's request by version is answered by the root's checkout
# of the package, whether or not the index carries it.
#
# The root's declaration of an identity by path or git overrides a
# dependency's request for it by version (the 2026-09-13-630 decision table,
# row 4). The request was first given to the index to select and pin, so a
# package the index did not carry yet -- the case a root pins a checkout for,
# ahead of a release -- refused the build:
#
#   error: dependency 'openkal-ld': no package found for exact selector
#
# Measured with openkal-musl's `openkal-ld = "0.1.0"` under `cfg(os = "openkal")`
# against a language server's git checkout of openkal-ld.
#
# Criteria:
#   A. A dependency's conditional request by version, for a package no index
#      carries, is answered by the root's path checkout under a target selector.
#   B. The same by the root's git checkout of a repository member.
#   C. A requirement the root's checkout does not satisfy is still refused,
#      naming both.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

case "$(uname -s)" in
    Linux)  OS=linux ;;
    Darwin) OS=macos ;;
    *)      OS=windows ;;
esac

# A repository whose member is the package, at a name no index has.
mkdir -p "$TMP/ws/m944leaf/src"
printf '[workspace]\nmembers = ["m944leaf"]\n' > "$TMP/ws/mcpp.toml"
printf '[package]\nname = "m944leaf"\nversion = "0.1.0"\n\n[targets.m944leaf]\nkind = "lib"\n' > "$TMP/ws/m944leaf/mcpp.toml"
printf 'export module m944leaf;\nexport int leaf() { return 1; }\n' > "$TMP/ws/m944leaf/src/leaf.cppm"
(cd "$TMP/ws" && git init --quiet -b main && git config user.email t@local && git config user.name t \
    && git add -A && git commit --quiet -m init)

# A dependency that asks for it by version, on this system only.
mkdir -p "$TMP/mid/src"
cat > "$TMP/mid/mcpp.toml" <<TOML
[package]
name = "m944mid"
version = "0.1.0"

[targets.m944mid]
kind = "lib"

[target.'cfg(os = "$OS")'.dependencies]
m944leaf = "0.1.0"
TOML
printf 'export module m944mid;\nimport m944leaf;\nexport int mid() { return leaf(); }\n' > "$TMP/mid/src/mid.cppm"

app() {   # app <dir> <declaration of m944leaf>
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<TOML
[package]
name = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[dependencies]
m944mid = { path = "$TMP/mid" }

[target.'cfg(os = "$OS")'.dependencies]
m944leaf = $2
TOML
    printf 'import m944mid;\nint main() { return mid() - 1; }\n' > "$1/src/main.cpp"
}

# A
app "$TMP/a" "{ path = \"$TMP/ws/m944leaf\" }"
(cd "$TMP/a" && "$MCPP" build > build.log 2>&1) || fail "A: the root's path checkout did not answer" "$TMP/a/build.log"
grep -q "no package found" "$TMP/a/build.log" && fail "A: the index was asked" "$TMP/a/build.log"
echo "ok: a path checkout answers a conditional request by version"

# B
app "$TMP/b" "{ git = \"file://$TMP/ws\", branch = \"main\" }"
(cd "$TMP/b" && "$MCPP" build > build.log 2>&1) || fail "B: the root's git checkout did not answer" "$TMP/b/build.log"
echo "ok: a git checkout of a member answers it"

# C
sed -i.bak 's/^m944leaf = "0.1.0"/m944leaf = "0.2.0"/' "$TMP/mid/mcpp.toml"
app "$TMP/c" "{ path = \"$TMP/ws/m944leaf\" }"
if (cd "$TMP/c" && "$MCPP" build > build.log 2>&1); then
    fail "C: a requirement the checkout does not satisfy was accepted" "$TMP/c/build.log"
fi
grep -q "requires 0.2.0" "$TMP/c/build.log" || fail "C: the refusal does not name the requirement" "$TMP/c/build.log"
echo "ok: an unsatisfied requirement is still refused"

echo "PASS: 944 a root checkout answers a version the index lacks"
