#!/usr/bin/env bash
# requires: gcc unix-shell
# A shared library a LIBRARY package acquires through `[feature-deps]` reaches
# the link line of that package's test binaries.
#
# THE DEFECT THIS COVERS, AND WHY IT WAS QUIET. `[feature-deps]` is folded into
# the root's dependency map during resolution, so the dependency was fetched
# and COMPILED -- every log line said it was there. The plan read the root's
# edges from a snapshot taken BEFORE that fold, so the dependency's shared
# library was neither linked nor named as an input. `mcpp build` on a library
# package still succeeded, because an archive resolves no symbols; the failure
# appeared in whoever linked an executable, as undefined references to the
# dependency's own entry points.
#
# THE SHAPE IS THE CRITERION. The same project written as a BINARY passes on
# both the defective and the fixed engine -- measured -- so a fixture built
# around `mcpp run` would have been a test that could not fail. What surfaces
# the defect is a library root whose test binary does the linking, which is the
# shape every library package in this ecosystem has.
#
# AND THE CRITERION IS THE PROGRAM RUNNING. Grepping build.ninja for
# `-lgreeter` would pass on a build that named the flag and never found the
# library; reading back the string the library owns distinguishes them.
set -e

MCPP="${MCPP:-mcpp}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The provider: a shared library and nothing else.
mkdir -p "$work/greeter/src"
cat > "$work/greeter/mcpp.toml" <<'TOML'
[package]
name    = "greeter"
version = "0.1.0"

[build]
sources = ["src/greeter.cpp"]

[targets.greeter]
kind = "shared"
TOML
cat > "$work/greeter/src/greeter.cpp" <<'CPP'
extern "C" const char* greeter_line() { return "greeter-from-the-shared-library"; }
CPP

# The consumer: a library package for which the provider is optional.
mkdir -p "$work/app/src" "$work/app/tests"
cat > "$work/app/mcpp.toml" <<'TOML'
[package]
name    = "app"
version = "0.1.0"

[build]
sources = ["src/lib.cpp"]

[targets.app]
kind = "lib"

[features]
default = {}
loud    = { defines = ["USE_GREETER"] }

[feature-deps.loud]
greeter = { path = "../greeter" }
TOML
cat > "$work/app/src/lib.cpp" <<'CPP'
int app_anchor() { return 0; }
CPP
cat > "$work/app/tests/use.cpp" <<'CPP'
#include <cstdio>
#ifdef USE_GREETER
extern "C" const char* greeter_line();
#endif
int main() {
#ifdef USE_GREETER
    std::printf("%s\n", greeter_line());
#else
    std::printf("quiet\n");
#endif
    return 0;
}
CPP

cd "$work/app"

fails=0
check() {
    case "$2" in
        *"$3"*) printf 'ok: %s\n' "$1" ;;
        *) printf 'ASSERT-FAIL: %s -- expected %s in:\n%s\n' "$1" "$3" "$2"
           fails=$((fails + 1)) ;;
    esac
}

loud="$("$MCPP" test use --features loud 2>&1 || true)"
rm -rf target
quiet="$("$MCPP" test use 2>&1 || true)"

printf -- '--- with --features loud ---\n%s\n' "$loud"
printf -- '--- without ---\n%s\n' "$quiet"

check "the feature's shared library links and runs" \
      "$loud" "greeter-from-the-shared-library"
# The control: without the feature the same project still builds and runs, so
# the reading above is about the feature rather than about the fixture.
check "and the same project builds without the feature" \
      "$quiet" "quiet"

if [ "$fails" -ne 0 ]; then
    printf 'FAIL: %s assertion(s) failed\n' "$fails"
    exit 1
fi
printf 'PASS: a [feature-deps] shared library reaches a test binary link line\n'
