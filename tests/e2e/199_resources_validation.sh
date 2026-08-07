#!/usr/bin/env bash
# mcpp#365 — the half of [resources] that is identical on every target.
#
# 197 (native Windows) and 198 (mingw cross) own everything about COMPILING a
# resource, and neither can run on a plain Linux or macOS shard. What can — and
# what has to, because it is the regression this test exists for — is VALIDATION:
# whether a declared path exists is a fact about the working tree, not about the
# target. That check used to sit inside the `is_pe()` branch, so a typo in
# `icon = "assets/app.ico"` was invisible to every non-Windows job and the
# Windows build was the first thing to say so.
#
# Nothing here compiles a resource. On a non-PE host the section is inapplicable
# and the build must stay silent; the file must still have to exist.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/proj/src" "$TMP/proj/assets"
cd "$TMP/proj"
printf 'int main() { return 0; }\n' > src/main.cpp
: > assets/app.ico          # contents are irrelevant to validation

write_toml() {   # $1 = [resources] body
    cat > mcpp.toml <<EOF
[package]
name    = "resval"
version = "1.2.3"

[resources]
$1

[targets.resval]
kind = "bin"
main = "src/main.cpp"
EOF
}

# ── 1. Every declared key must exist, on every target ─────────────────────
#
# One case per key, because they are three separate resolve sites and a fix
# that only covers `icon` would look identical from the outside.
for CASE in 'icon = "assets/typo.ico"' \
            'extra-inputs = ["assets/typo.h"]' \
            'files = ["res/typo.rc"]'; do
    write_toml "$CASE"
    rm -rf target
    "$MCPP" build > v.log 2>&1 \
        && fail "a missing declared resource must fail the build ($CASE)" v.log
    grep -q 'does not exist' v.log \
        || fail "expected a clear missing-file error ($CASE)" v.log
    # The error must name the key the user wrote, or it sends them hunting.
    KEY="${CASE%% *}"
    grep -q "$KEY" v.log \
        || fail "the error must name the [resources] key it came from ($KEY)" v.log
done

# ── 2. ...and when everything exists, a non-PE build says nothing ─────────
#
# "Inapplicable" is not "degraded" and not "skipped with a warning": no units,
# no diagnostics, and no res/ directory.
#
# PE hosts stop here, before the build below: `assets/app.ico` is an EMPTY file
# — enough to exist, which is all §1 needed — and a real resource compiler would
# rightly reject it. 197 builds a structurally valid icon and owns everything
# about compiling one.
case "$(uname -s 2>/dev/null || echo unknown)" in
  MINGW*|MSYS*|CYGWIN*|Windows*) echo "OK (PE host: 197 owns the rest)"; exit 0 ;;
esac

write_toml 'icon = "assets/app.ico"'
rm -rf target
"$MCPP" build > ok.log 2>&1 || fail "build with a valid [resources] failed" ok.log

grep -qi 'resource' ok.log \
    && fail "a non-PE build must say nothing about resources" ok.log
find target -type d -name res | grep -q . \
    && fail "a non-PE build must not emit resource units" ok.log

echo "OK"
