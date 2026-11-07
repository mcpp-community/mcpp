#!/usr/bin/env bash
# requires: unix-shell gcc fresh-sandbox
# A PLATFORM SELECTED AS A SERIES (SPEC-013 §4.6, 2026.11.7.1). A project
# selects one (`"<name>@^0"`); the target table's default is one release
# from 2026.11.7.2 (design 2026-11-07 §4.10):
#
#   A. The series takes its newest release whose `[package] mcpp` floor this
#      mcpp meets: a release made for a newer mcpp is skipped and named.
#   B. mcpp.lock records the release; a newer one published later is not taken
#      while the lock names one the series admits.
#   C. `mcpp update` drops the record, and the next build takes the newer one.
#
# The index is a local path index whose payloads are pre-seeded; the platform
# releases are packages of it for this machine's triples.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
export MCPP_HOME="$TMP/mcpp-home"
source "$(dirname "$0")/_inherit_toolchain.sh"

mkdir -p "$TMP/proj/src" "$TMP/proj/local-index/pkgs/a"
cd "$TMP/proj"
entries=""
release() {   # $1: version, $2: the mcpp floor ('' for none)
    local dir=".mcpp/.xlings/data/xpkgs/acme.plat/$1"
    mkdir -p "$dir"
    {
        printf '[package]\nnamespace = "acme"\nname = "plat"\nversion = "%s"\n' "$1"
        [ -n "$2" ] && printf 'mcpp = ">=%s"\n' "$2"
        printf '\n[platform]\ntargets = ["x86_64-linux-gnu", "aarch64-linux-gnu", "aarch64-macos", "x86_64-macos"]\n'
        printf '\n[platform.build]\ncxxflags = ["-DPLAT_RELEASE=\\"%s\\""]\n' "$1"
    } > "$dir/mcpp.toml"
    entries="[\"$1\"] = { url = \"u\", sha256 = \"0\" }, $entries"
    cat > local-index/pkgs/a/acme.plat.lua <<EOF
package = {
    spec = "1", namespace = "acme", name = "acme.plat",
    description = "platform series fixture", licenses = {"MIT"}, type = "package",
    mcpp = "mcpp.toml",
    xpm = { linux = { $entries }, macosx = { $entries } },
}
EOF
}
release 0.1.0 ''
release 0.2.0 '2099.1.1.1'
cat > mcpp.toml <<'EOF'
[package]
name    = "proj"
version = "0.1.0"

[indices]
acme = { path = "local-index" }

[target.'cfg(any(linux, macos))']
platform = "acme.plat@^0"
EOF
printf '#include <cstdio>\nint main() { std::printf("release %%s\\n", PLAT_RELEASE); }\n' > src/main.cpp

# A
"$MCPP" run > a.log 2>&1 || fail "A: build" a.log
grep -q "plat v0.2.0 needs mcpp >= 2099.1.1.1" a.log || fail "A: the release for a newer mcpp was not named" a.log
grep -q "release 0.1.0" a.log || fail "A: the series did not take 0.1.0" a.log
grep -A4 '\[package."acme.plat"\]' mcpp.lock | grep -q 'version = "0.1.0"' || fail "A: mcpp.lock does not record 0.1.0" mcpp.lock
echo "ok: A, the series skips a release made for a newer mcpp"

# B
release 0.1.1 ''
rm -rf target   # the graph is planned again, against the index that now holds 0.1.1
"$MCPP" run > b.log 2>&1 || fail "B: build" b.log
grep -q "release 0.1.0" b.log || fail "B: a newer release was taken over the lock" b.log
grep -q "(mcpp.lock)" b.log || fail "B: the build does not say the lock chose" b.log
echo "ok: B, the lock keeps the recorded release"

# C
"$MCPP" update > c-update.log 2>&1 || fail "C: mcpp update" c-update.log
rm -rf target
"$MCPP" run > c.log 2>&1 || fail "C: build" c.log
grep -q "release 0.1.1" c.log || fail "C: after mcpp update the newer release was not taken" c.log
echo "ok: C, mcpp update moves the series forward"
echo "PASS: 1021 a platform series is locked and skips newer engines"
