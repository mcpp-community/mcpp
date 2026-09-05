#!/usr/bin/env bash
# requires: unix-shell jq
# A package can say which of its include directories stop at its own boundary.
#
# `publicUsage` TOOK `privateBuild`'s DIRECTORIES ENTIRE, so a package was
# built from exactly the set it published. For almost every package those are
# the same set; for one that vendors a library with an internal header overlay
# they are not, and the difference reaches every consumer.
#
# `mcpplibs/openkal-musl` wrote the case down in its own source, having found
# it three times (port/include/features.h):
#
#   ⓘ THIS IS THE SECOND-BEST REMEDY. The first would be for a package to
#   distinguish the directories it is BUILT FROM from the directories it
#   PUBLISHES. Measured 2026-08-22: mcpp cannot express it.
#
# musl reaches its own declarations through `src/include`, whose headers define
# `hidden`, `weak` and `weak_alias` — names meaningful only to musl's sources.
# Publishing that directory hands those macros to every consumer; which
# consumer breaks on which name was discovered one at a time.
#
# THE CRITERION IS THE DIRECTORY, NOT THE SYMPTOM. Asserting that `hidden`
# no longer collides would go green again the moment the package patched that
# one macro, while the leak stayed. This asserts that the directory is not on
# the consumer's command line — and, separately, that a consumer using the name
# as an ordinary identifier compiles, which is what the user actually reported.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

mkdir -p src lib/src lib/pub lib/internal lib/gen/one lib/gen/two

cat > lib/mcpp.toml <<'EOF'
[package]
name    = "vendored"
version = "0.1.0"

[build]
# ORDER IS LOAD-BEARING and is why `private_include_dirs` is a SUBSET of
# this list rather than a second list: the internal overlay must precede the
# public headers for the package's OWN build, and two TOML arrays cannot
# express one order.
#
# `gen/*` IS A GLOB ON BOTH LINES. The filter is applied AFTER expansion, so
# the glob withholds exactly the directories it expands to. Comparing the
# unexpanded spellings would be the obvious implementation and would publish
# every one of them, because `gen/*` is not literally equal to `gen/one`.
include_dirs         = ["internal", "gen/*", "pub"]
private_include_dirs = ["internal", "gen/*"]
EOF
printf 'export module vendored;\nexport int vendored_v() { return 7; }\n' > lib/src/vendored.cppm
printf '#pragma once\n#define VENDORED_PUBLIC 1\n' > lib/pub/pub.h
# The internal overlay: a macro that is meaningful to the package and a
# perfectly ordinary identifier to everyone else.
printf '#pragma once\n#define hidden __attribute__((visibility("hidden")))\n' > lib/internal/overlay.h
printf '#pragma once\n' > lib/gen/one/one.h
printf '#pragma once\n' > lib/gen/two/two.h

cat > mcpp.toml <<'EOF'
[package]
name    = "boundaryprobe"
version = "0.1.0"

[dependencies]
vendored = { path = "lib" }
EOF
# `hidden` as a local variable — the exact shape openkal-musl#13 reported.
cat > src/main.cpp <<'EOF'
#include <cstdio>
#include <pub.h>
import vendored;
int main() {
    int hidden = 1;
    std::printf("%d %d\n", vendored_v() + hidden, VENDORED_PUBLIC);
}
EOF

if ! "$MCPP" build >/dev/null 2>&1; then
    echo "FAIL: the consumer did not build — the private overlay is still reaching it"
    "$MCPP" build 2>&1 | tail -20 | sed 's/^/        /'
    exit 1
fi

[ -s compile_commands.json ] || { echo "FAIL: no compile_commands.json"; exit 1; }

# DENOMINATORS ON BOTH SIDES. With no provider row or no consumer row the
# assertions below are vacuously true.
prov="$(jq -r '[.[] | select(.file | test("/lib/src/"))] | length' compile_commands.json)"
cons="$(jq -r '[.[] | select(.file | test("/src/main"))] | length' compile_commands.json)"
if [ "$prov" -lt 1 ] || [ "$cons" -lt 1 ]; then
    echo "FAIL: CDB has provider=$prov consumer=$cons rows — nothing was checked"
    exit 1
fi

has_dir() {   # file-pattern, dir-pattern → "true"/"false"
    jq -r --arg f "$1" --arg d "$2" '
      [ .[] | select(.file | test($f))
             | ((.arguments // (.command | split(" "))) | map(test($d)) | any) ]
      | any' compile_commands.json
}

fail=0
# The provider is built from BOTH — the private directory is private, not unused.
[ "$(has_dir '/lib/src/' '/lib/internal')" = true ] || {
    echo "FAIL: the provider lost its own private directory"; fail=1; }
[ "$(has_dir '/lib/src/' '/lib/pub')" = true ] || {
    echo "FAIL: the provider lost its own public directory"; fail=1; }
# The consumer gets the public one and NOT the private one.
[ "$(has_dir '/src/main' '/lib/pub')" = true ] || {
    echo "FAIL: the public directory did not reach the consumer"; fail=1; }
[ "$(has_dir '/src/main' '/lib/internal')" = false ] || {
    echo "FAIL: the private directory leaked to the consumer"; fail=1; }

# AND THE GLOB WITHHOLDS WHAT IT EXPANDS TO — both directories, by name.
# Checking `gen` alone would pass for an implementation that matched the
# unexpanded spelling and published `gen/one` and `gen/two` anyway.
for g in one two; do
    [ "$(has_dir '/lib/src/' "/lib/gen/$g")" = true ] || {
        echo "FAIL: the provider lost 'gen/$g', which its own glob names"; fail=1; }
    [ "$(has_dir '/src/main' "/lib/gen/$g")" = false ] || {
        echo "FAIL: 'gen/$g' leaked to the consumer — the glob was compared unexpanded"
        fail=1; }
done

[ "$fail" = 0 ] || exit 1
echo "OK: built-from and published are separable, globs included (provider=$prov consumer=$cons rows)"
