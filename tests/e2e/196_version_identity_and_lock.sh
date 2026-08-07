#!/usr/bin/env bash
# requires: gcc fresh-sandbox
# mcpp#363 — a resolved version must be an index KEY, and mcpp.lock must record
# what was resolved.
#
# Before this, pm/resolver.cppm parsed the index's literal version keys, threw
# them away, and re-rendered an address from the parsed numbers. Anything the
# renderer could not reproduce became an address that does not exist:
#
#   1.92.8-docking   pre-release, truncated to 1.92.8  (and so indistinguishable
#                    from the non-docking release — a different tarball)
#   25.0.4.7.1       five segments, truncated to 25.0.4.7
#   b10069           not a number at all: skipped, then reported as
#                    "no valid versions in index"
#
# Every shape below is one the real xim-pkgindex publishes today (compat.imgui,
# jdk-corretto, jdk-temurin, khistory). The index here is a local path index so
# nothing is downloaded; the payload for the version that must win is pre-seeded,
# and the others deliberately have none — a build that resolves the wrong key
# fails to install, which is itself an assertion.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src" "$TMP/proj/local-index/pkgs/a"
cd "$TMP/proj"

# ── One descriptor per upstream shape ─────────────────────────────────────
mk_pkg() {   # $1 = short name, $2 = version-table body, $3 = module name (default gadget)
    MOD="${3:-gadget}"
    cat > "local-index/pkgs/a/acme.$1.lua" <<EOF
package = {
    spec = "1", namespace = "acme", name = "acme.$1",
    description = "version-model fixture", licenses = {"MIT"}, type = "package",
    xpm = { linux = $2, macosx = $2, windows = $2 },
    mcpp = { language = "c++23", import_std = false,
             sources = { "src/$MOD.cppm" },
             targets = { ["$MOD"] = { kind = "lib" } }, deps = {} },
}
EOF
}

# compat.imgui's shape: a stable release beside a different upstream branch.
mk_pkg im  '{ ["1.92.8"] = { url = "u", sha256 = "0" },
              ["1.92.8-docking"] = { url = "u", sha256 = "0" } }'
# jdk-corretto's shape: alias keys pointing at a five-segment real release.
mk_pkg jdk '{ ["latest"] = { ref = "25.0.4.7.1" },
              ["25.0.4"] = { ref = "25.0.4.7.1" },
              ["25.0.4.7.1"] = { url = "u", sha256 = "0" } }'
# khistory's shape: the only real release has an unorderable key.
mk_pkg kh  '{ ["latest"] = { ref = "pre-v0.0.5" },
              ["pre-v0.0.5"] = { url = "u", sha256 = "0" } }'
# jdk-temurin's shape, but with two REAL entries: a genuine precedence tie.
mk_pkg tie '{ ["1.0.0+a"] = { url = "u", sha256 = "0" },
              ["1.0.0+b"] = { url = "u", sha256 = "0" } }'
# An ordinary package, used below as a DEV-dependency.
mk_pkg dv  '{ ["1.0.0"] = { url = "u", sha256 = "0" } }' devkit

seed() {     # pre-seed a payload so only the RIGHT key can install
    MOD="${3:-gadget}"
    mkdir -p ".mcpp/.xlings/data/xpkgs/acme.$1/$2/src"
    printf 'export module %s;\nexport int %s_value() { return 42; }\n' "$MOD" "$MOD" \
        > ".mcpp/.xlings/data/xpkgs/acme.$1/$2/src/$MOD.cppm"
}
seed im  1.92.8
seed jdk 25.0.4.7.1
seed dv  1.0.0 devkit

printf 'import gadget;\nint main(){ return gadget_value() == 42 ? 0 : 1; }\n' > src/main.cpp

use_dep() {  # $1 = short name, $2 = constraint
    cat > mcpp.toml <<EOF
[package]
name    = "proj"
version = "0.1.0"

[indices]
acme = { path = "local-index" }

[dependencies.acme]
$1 = "$2"

[targets.proj]
kind = "bin"
main = "src/main.cpp"
EOF
    rm -f mcpp.lock
}

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

# ── 1. A range must not reach into a pre-release ──────────────────────────
use_dep im '^1.92.8'
"$MCPP" build > b1.log 2>&1 || fail "^1.92.8 did not build" b1.log
grep -q '^\s*Resolved acme.im .* → v1\.92\.8$' b1.log \
    || fail "^1.92.8 must resolve to 1.92.8, never the -docking branch" b1.log

# ── 2. ...but naming it exactly still addresses it ────────────────────────
# Only 1.92.8 is seeded, so this must fail AT INSTALL with the docking address —
# proving the wire address carried the literal key rather than a truncation.
use_dep im '1.92.8-docking'
"$MCPP" build > b2.log 2>&1 && fail "expected the unseeded -docking payload to fail" b2.log
grep -q '1\.92\.8-docking' b2.log \
    || fail "exact pre-release must be addressed literally, not truncated" b2.log

# ── 3. Five segments survive, and an alias is not a candidate ─────────────
# `^25.0` used to render 25.0.4.7 (a key that does not exist) or pick the alias.
use_dep jdk '^25.0'
"$MCPP" build > b3.log 2>&1 || fail "^25.0 did not build" b3.log
grep -q '^\s*Resolved acme.jdk .* → v25\.0\.4\.7\.1$' b3.log \
    || fail "^25.0 must resolve to the real five-segment key, not an alias or a truncation" b3.log

# ── 4. An unorderable key is named, not blamed on the index ───────────────
use_dep kh '*'
"$MCPP" build > b4.log 2>&1 && fail "a range over unorderable keys must not succeed" b4.log
grep -q 'not ordered versions' b4.log \
    || fail "expected an error naming the unorderable keys" b4.log
grep -q 'acme.kh = "pre-v0.0.5"' b4.log \
    || fail "the error must show the exact pin that works" b4.log
grep -q 'no valid versions in index' b4.log \
    && fail "the old message blames the index for a package it publishes fine" b4.log

# ...and pinning it exactly is accepted (fails later, at install, for want of a
# payload — which is proof that resolution let it through).
use_dep kh 'pre-v0.0.5'
"$MCPP" build > b5.log 2>&1 && fail "expected the unseeded payload to fail" b5.log
grep -q 'not ordered versions' b5.log \
    && fail "an exact unorderable key must resolve, not be rejected" b5.log

# The `=` form must work too — it is exactly what the error above tells the user
# to write, and it is what try_merge_semver produces internally from a literal
# pin. Routing it through the SemVer grammar would reject the remedy.
use_dep kh '=pre-v0.0.5'
"$MCPP" build > b5b.log 2>&1 && fail "expected the unseeded payload to fail" b5b.log
grep -qE 'invalid version constraint|not ordered versions' b5b.log \
    && fail "'=<unorderable key>' must resolve to that key" b5b.log

# ── 5. A genuine precedence tie is refused, not guessed ───────────────────
use_dep tie '^1.0'
"$MCPP" build > b6.log 2>&1 && fail "a tie must not silently pick one" b6.log
grep -q 'compare EQUAL' b6.log || fail "expected the tie to be named" b6.log
grep -q '1\.0\.0+a' b6.log && grep -q '1\.0\.0+b' b6.log \
    || fail "both tied keys must be listed" b6.log

# ...while naming one exactly is not ambiguous at all: the ORDER cannot separate
# them (SemVer excludes build metadata from precedence), the LITERALS can.
use_dep tie '=1.0.0+b'
"$MCPP" build > b6b.log 2>&1 && fail "expected the unseeded payload to fail" b6b.log
grep -q 'compare EQUAL' b6b.log \
    && fail "an exact build-metadata pin must not be reported as a tie" b6b.log
grep -q '1\.0\.0+b' b6b.log || fail "the exact key must be addressed literally" b6b.log

# ── 6. mcpp.lock records the resolution, not the constraint ───────────────
use_dep im '^1.92.8'
"$MCPP" build > b7.log 2>&1 || fail "rebuild failed" b7.log
grep -q 'version = "1.92.8"' mcpp.lock \
    || fail "the lock must record the resolved version" mcpp.lock
grep -q '\^' mcpp.lock \
    && fail "a lock that records a range locks nothing" mcpp.lock
# The banner and the lock read the same data, so they cannot disagree. (The dep
# announces itself as Compiling or Cached depending on the build cache; both go
# through the same version string, which is the point.)
grep -qE '(Compiling|Cached) +acme\.im v1\.92\.8' b7.log \
    || fail "the dependency banner must announce the resolved version" b7.log
grep -q 'acme\.im v\^' b7.log \
    && fail "the banner must never print a constraint as a version" b7.log

# Honest about what it is not: the lock is written but not yet read back for
# index deps. DELETE this assertion in the same change that makes it
# authoritative — it exists so that removal cannot be forgotten.
grep -q 'does not yet pin future builds' mcpp.lock \
    || fail "the lock must state that it does not pin yet" mcpp.lock

# Idempotent: a second build must not churn the file.
cp mcpp.lock lock.first
"$MCPP" build > b8.log 2>&1 || fail "second build failed" b8.log
cmp -s mcpp.lock lock.first || fail "mcpp.lock is not stable across builds" mcpp.lock

# ── 7. ...and stable across COMMANDS, which is the harder half ────────────
#
# The lock is written from the resolution result, and `mcpp test` resolves
# dev-dependencies while `mcpp build` does not. Recording them therefore made a
# VCS-committed file depend on which command ran last: build, test, build wrote
# three different files. A lock has to be a function of the MANIFEST.
mkdir -p tests
printf 'import devkit;\nint main(){ return devkit_value()==42?0:1; }\n' > tests/t_dev.cpp
cat > mcpp.toml <<'EOF'
[package]
name    = "proj"
version = "0.1.0"

[indices]
acme = { path = "local-index" }

[dependencies.acme]
im = "^1.92.8"

[dev-dependencies.acme]
dv = "^1.0"

[targets.proj]
kind = "bin"
main = "src/main.cpp"
EOF
rm -f mcpp.lock
"$MCPP" build > c1.log 2>&1 || fail "build with a dev-dep failed" c1.log
cp mcpp.lock lock.build
"$MCPP" test  > c2.log 2>&1 || fail "test with a dev-dep failed" c2.log
cmp -s mcpp.lock lock.build \
    || { diff lock.build mcpp.lock || true
         fail "mcpp test rewrote mcpp.lock — the lock must not depend on the command"; }
"$MCPP" build > c3.log 2>&1 || fail "build after test failed" c3.log
cmp -s mcpp.lock lock.build \
    || fail "mcpp build rewrote mcpp.lock after mcpp test" mcpp.lock
# The dev-dep really was resolved (so this is not passing by never reaching it)…
grep -q 'Resolved acme.dv' c2.log \
    || fail "the dev-dependency was never resolved — the check above is vacuous" c2.log
# …and it is deliberately absent from the file.
grep -q 'acme\.dv' mcpp.lock \
    && fail "dev-dependencies must not be recorded in mcpp.lock" mcpp.lock
grep -q 'dev-dependencies are excluded' mcpp.lock \
    || fail "the lock must state that dev-dependencies are excluded" mcpp.lock

echo "OK"
