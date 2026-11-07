#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# 1027 -- a configuration group keeps the lock entries of the other groups
# (mcpp#903).
#
# A command over a workspace whose members take two configurations plans one
# group at a time, and each plan writes the lock. A plan updates the entries it
# resolves and keeps the others; "the others" were taken from the members the
# COMMAND asked for, so the group that resolves no entry of a package took the
# package from this configuration: `mcpp test --workspace` removed the native
# target from a dependency only the other group's member uses.
#
# Criteria: after a build of the whole workspace, the entry of the package one
# group uses keeps both the configuration it was recorded for and the one this
# build resolved it in.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/ws"
cd "$TMP/ws"

mkdir -p local-index/pkgs/w .mcpp/.xlings/data/xpkgs/mcpplibs.widget3/1.0.0/src
cat > local-index/pkgs/w/widget3.lua <<'LUA'
package = {
    spec = "1",
    namespace = "mcpplibs",
    name = "widget3",
    description = "a package of the default namespace from a local index",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["1.0.0"] = { url = "https://example.invalid/w.tar.gz" } },
        macosx  = { ["1.0.0"] = { url = "https://example.invalid/w.tar.gz" } },
        windows = { ["1.0.0"] = { url = "https://example.invalid/w.zip" } },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/widget3.cppm" },
        targets = { ["widget3"] = { kind = "lib" } },
        deps = {},
    },
}
LUA
printf 'export module widget3;\nexport int widget3_value() { return 36; }\n' \
    > .mcpp/.xlings/data/xpkgs/mcpplibs.widget3/1.0.0/src/widget3.cppm

cat > mcpp.toml <<'TOML'
[workspace]
members = ["a", "b"]

[workspace.indices]
mcpplibs = { path = "local-index" }
TOML

# Two configurations: `b` is another C++ standard (833 G4).
mkdir -p a/src b/src
cat > a/mcpp.toml <<'TOML'
[package]
name = "a"
version = "0.1.0"

[dependencies]
mcpplibs.widget3 = "1.0.0"

[targets.a]
kind = "bin"
main = "src/main.cpp"
TOML
printf 'import widget3;\nint main() { return widget3_value() == 36 ? 0 : 1; }\n' > a/src/main.cpp
cat > b/mcpp.toml <<'TOML'
[package]
name = "b"
version = "0.1.0"
standard = "c++26"

[targets.b]
kind = "bin"
main = "src/main.cpp"
TOML
printf 'int main() { return 0; }\n' > b/src/main.cpp

"$MCPP" build --workspace --configure-only > first.log 2>&1 || fail "the first configure failed" first.log
grep -q 'widget3' mcpp.lock || fail "the lock records no widget3 entry" mcpp.lock

# The entry as another configuration recorded it as well.
python3 - <<'PY'
import re
text = open("mcpp.lock").read()
head = re.search(r'(\[package\."[^"]*widget3"\]\n(?:(?!\[)[^\n]*\n)*)', text)
block = head.group(1)
block2 = re.sub(r'^targets = .*\n', '', block, flags=re.M) + 'targets = ["x86_64-openkal"]\n'
open("mcpp.lock", "w").write(text.replace(block, block2))
PY

"$MCPP" build --workspace --configure-only > second.log 2>&1 || fail "the second configure failed" second.log
line=$(awk '/widget3"\]/{f=1} f && /^targets/{print; exit}' mcpp.lock)
[[ "$line" == *x86_64-openkal* ]] || fail "the other configuration's target was dropped: $line" mcpp.lock second.log
[[ "$line" == *,* ]] || fail "this configuration's target is missing: $line" mcpp.lock second.log
echo "ok: $line"

echo "PASS: 1027 a configuration group keeps the other groups' lock entries"
