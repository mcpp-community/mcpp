#!/usr/bin/env bash
# 1074 -- a namespace's package may be named like a dependency specification's
# field (#918; docs/06).
#
# `gpp.version = { path = "../version" }` is TOML for the table `gpp` holding the
# key `version`, whose value is a table: the package `version` in the namespace
# `gpp`. A specification's `path`, `version` and `git` are strings and its
# `workspace` a boolean, so only such a value makes a table one specification.
#
# Criteria: a project depending on `gpp.version` by path builds and runs it, and
# `mcpp emit build-database` describes it.
set -e
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; tail -30 "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/version/src" "$TMP/app/src"
cat > "$TMP/version/mcpp.toml" <<'T'
[package]
namespace = "gpp"
name      = "version"
version   = "0.1.0"
T
cat > "$TMP/version/src/version.cppm" <<'T'
export module gpp.version;
export int gpp_version() { return 7; }
T
cat > "$TMP/app/mcpp.toml" <<'T'
[package]
name    = "app"
version = "0.1.0"

[dependencies]
gpp.version = { path = "../version" }
T
cat > "$TMP/app/src/main.cpp" <<'T'
import gpp.version;
int main() { return gpp_version() == 7 ? 0 : 1; }
T

cd "$TMP/app"
"$MCPP" run > run.log 2>&1 || fail "the package gpp.version was not read as a package" run.log
grep -q 'must specify' run.log && fail "the namespace was read as one specification" run.log
"$MCPP" emit build-database --format json > db.json 2> db.err || fail "the build database was not described" db.err
grep -q 'version.cppm' db.json || fail "the build database does not hold gpp.version" db.json
echo "OK"
