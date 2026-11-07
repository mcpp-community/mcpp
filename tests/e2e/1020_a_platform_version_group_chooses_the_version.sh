#!/usr/bin/env bash
# requires: unix-shell gcc fresh-sandbox
# A PLATFORM'S VERSION GROUP CHOOSES THE VERSION (SPEC-013 §4.5, 2026.11.7.1).
# `[platform.versions]` used to be checked after the walk only: a request
# resolved to the newest version its range admits, and the build was refused
# when that was not the group's. The group now takes part in resolution.
#
#   A. A range that admits the group's version resolves to it, though the index
#      holds a newer one.
#   B. A request the group's version does not satisfy is refused before
#      anything is built, naming the request, its requester and the group.
#   C. A development tree (a path dependency) is not bound by the group.
#
# The index is a local path index whose payloads are pre-seeded, so nothing is
# downloaded; the platform is a local directory for this machine's triples.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/plat" "$TMP/proj/src" "$TMP/proj/local-index/pkgs/a" "$TMP/fam-dev/src"
cat > "$TMP/plat/mcpp.toml" <<'TOML'
[package]
name    = "group-platform"
version = "0.1.0"

[platform]
targets = ["x86_64-linux-gnu", "aarch64-linux-gnu", "aarch64-macos", "x86_64-macos"]

[platform.versions]
fam = "1.1.0"
TOML

cd "$TMP/proj"
cat > local-index/pkgs/a/acme.fam.lua <<'EOF'
package = {
    spec = "1", namespace = "acme", name = "acme.fam",
    description = "version-group fixture", licenses = {"MIT"}, type = "package",
    xpm = {
        linux  = { ["1.2.0"] = { url = "u", sha256 = "0" }, ["1.1.0"] = { url = "u", sha256 = "0" }, ["1.0.0"] = { url = "u", sha256 = "0" } },
        macosx = { ["1.2.0"] = { url = "u", sha256 = "0" }, ["1.1.0"] = { url = "u", sha256 = "0" }, ["1.0.0"] = { url = "u", sha256 = "0" } },
    },
    mcpp = { language = "c++23", import_std = false, sources = { "src/fam.cppm" },
             targets = { ["fam"] = { kind = "lib" } }, deps = {} },
}
EOF
# Only the group's version has a payload: resolving another one fails to install.
mkdir -p .mcpp/.xlings/data/xpkgs/acme.fam/1.1.0/src
printf 'export module fam;\nexport int fam_value() { return 110; }\n' > .mcpp/.xlings/data/xpkgs/acme.fam/1.1.0/src/fam.cppm
printf 'import fam;\nint main() { return fam_value() == 110 ? 0 : 1; }\n' > src/main.cpp

use() {   # $1: the request of acme.fam
    cat > mcpp.toml <<EOF
[package]
name    = "proj"
version = "0.1.0"

[indices]
acme = { path = "local-index" }

[target.'cfg(any(linux, macos))']
platform = { path = "../plat" }

[dependencies.acme]
fam = $1

[targets.proj]
kind = "bin"
main = "src/main.cpp"
EOF
}

# A
use '"^1.0.0"'
"$MCPP" build > a.log 2>&1 || fail "A: the group's version was not taken" a.log
grep -q "fam =1.1.0 → v1.1.0" a.log || fail "A: the request did not resolve to the group's version" a.log
"$MCPP" run > a-run.log 2>&1 || fail "A: the program" a-run.log
echo "ok: A, a range resolves to the group's version though the index holds a newer one"

# B
use '"^1.2.0"'
rm -rf target
if "$MCPP" build > b.log 2>&1; then fail "B: a request outside the group was built" b.log; fi
grep -q "fam is requested as '^1.2.0' by proj" b.log || fail "B: the refusal does not name the request" b.log
grep -q "is a version group" b.log && grep -q "names fam 1.1.0" b.log || fail "B: the refusal does not name the group" b.log
[ -z "$(find target -name '*.o' 2>/dev/null)" ] || fail "B: something was compiled before the refusal" b.log
echo "ok: B, a request the group does not admit is refused naming both"

# C
printf '[package]\nnamespace = "acme"\nname = "fam"\nversion = "1.2.0"\n[targets.fam]\nkind = "lib"\n[build]\nsources = ["src/fam.cppm"]\n' > "$TMP/fam-dev/mcpp.toml"
printf 'export module fam;\nexport int fam_value() { return 110; }\n' > "$TMP/fam-dev/src/fam.cppm"
use '{ path = "../fam-dev" }'
rm -rf target
"$MCPP" build > c.log 2>&1 || fail "C: a development tree was bound by the group" c.log
echo "ok: C, a path dependency is not bound by the group"
echo "PASS: 1020 a platform version group chooses the version"
