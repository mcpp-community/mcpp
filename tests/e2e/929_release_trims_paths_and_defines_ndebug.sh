#!/usr/bin/env bash
# requires: gcc elf unix-shell
# `trim_paths` and `ndebug` (design 2026-10-23 §15.3-§15.4; mcpp#832; R2, R2b).
#
# A release artifact named the machine that built it: `__FILE__` in an
# `assert` reached mcpp's own released binary as
# `/home/runner/work/mcpp/mcpp/modules/libs/src/json/json.hpp`. The shipping
# profiles now map every absolute prefix to what it is -- the root package's
# sources to `<name>@<version>` -- and define NDEBUG, as CMake's Release does.
set -e

MCPP="${MCPP:-mcpp}"
source "$(dirname "$0")/_toolchain_env.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export NO_COLOR=1
fail() { echo "FAIL: $*"; echo "----"; echo "${out:-}"; exit 1; }

mk() {
    mkdir -p "$1/src"
    cat > "$1/mcpp.toml" <<TOML
[package]
name    = "app"
version = "1.2.0"
[toolchain]
default = "gcc@${GCC_VERSION}"
$2
TOML
    cat > "$1/src/main.cpp" <<'CPP'
#include <cassert>
import std;
int main() {
    std::cout << "file " << __FILE__ << ' ' << std::source_location::current().file_name()
              << std::endl;   // flushed: the assert below aborts the process
    assert(false && "assert fired");
}
CPP
}

mk "$work/a" ""
cd "$work/a"
out="$("$MCPP" run --release 2>&1)" || fail "R2b: release ran the assert"
grep -q "file app@1.2.0/src/main.cpp app@1.2.0/src/main.cpp" <<<"$out" || fail "R2: release did not trim __FILE__ and source_location"
a=$(ls -t target/*/*/bin/app | head -1)
strings -a "$a" | grep -qF "$work" && fail "R2: the release artifact still carries the project's path"

# The same sources, another directory: the same embedded paths.
mk "$work/b" ""
(cd "$work/b" && "$MCPP" build --release >/dev/null 2>&1) || fail "R2: the second checkout failed"
b=$(ls -t "$work"/b/target/*/*/bin/app | head -1)
diff <(strings -a "$a" | grep "app@1.2.0") <(strings -a "$b" | grep "app@1.2.0") \
    || fail "R2: two checkouts embed different paths"

# dev keeps both.
set +e; out="$("$MCPP" run 2>&1)"; rc=$?; set -e
[[ $rc -ne 0 ]] || fail "R2b: dev did not run the assert"
grep -qF "file $work/a/src/main.cpp" <<<"$out" || fail "R2: dev trimmed a path"

# Both are profile keys, and `false` restores the previous artifact.
mk "$work/c" '[profile.release]
ndebug     = false
trim_paths = false'
cd "$work/c"
set +e; out="$("$MCPP" run --release 2>&1)"; rc=$?; set -e
[[ $rc -ne 0 ]] || fail "R2b: ndebug = false did not keep the assert"
grep -qF "file $work/c/src/main.cpp" <<<"$out" || fail "R2: trim_paths = false still trimmed"

# A debug build with trimmed paths carries the way back for a debugger.
mk "$work/d" '[profile.release]
debug = true'
cd "$work/d"
"$MCPP" build --release >/dev/null 2>&1 || fail "release with debug failed"
g=$(ls target/*/*/source-map.gdbinit | head -1)
grep -q "set substitute-path app@1.2.0 $work/d" "$g" || fail "source-map.gdbinit does not map the root package back"

echo "PASS: release trims the paths an artifact carries and defines NDEBUG; dev keeps both"
