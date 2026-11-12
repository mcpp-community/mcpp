#!/usr/bin/env bash
# requires: llvm unix-shell
# 1010 -- the std module is compiled with the compile words the platform states
# for every unit (SPEC-013 §2.5; 2026.11.3.1).
#
# openkal-platform 0.23.0 compiles every unit on x86_64 with
# `-fcf-protection=full`, which clang records in a BMI and checks when a unit
# imports it. The std module was compiled without the platform's words, and
# every importer was refused:
#
#     error: Control-Flow Branch Protection enabled was disabled in precompiled
#     file '.../pcm.cache/std.pcm' but is currently enabled
#
# A platform for this machine's own triple states the flag here; a program
# that imports std builds and runs with it, and the module's command names it.
# The flag is the x86_64 one, so the test asks it of an x86_64 machine only.
set -e
case "$(uname -m)" in x86_64|amd64) ;; *) echo "SKIP: -fcf-protection is an x86_64 flag"; exit 77 ;; esac
source "$(dirname "$0")/_toolchain_env.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

mkdir -p "$TMP/plat" "$TMP/app/src"
cat > "$TMP/plat/mcpp.toml" <<'TOML'
[package]
name    = "cet-platform"
version = "0.1.0"

[platform]
targets = ["x86_64-linux-gnu"]

[platform.build]
cxxflags = ["-fcf-protection=full"]
TOML

cd "$TMP/app"
cat > mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[toolchain]
default = "llvm@${LLVM_VERSION}"

[target.'cfg(linux)']
platform = { path = "../plat" }
TOML
cat > src/main.cpp <<'CPP'
import std;
int main() {
    std::vector<int> v{1, 2, 3};
    std::println("std with the platform's words: {}", v.size());
    return 0;
}
CPP

"$MCPP" build -v > build.log 2>&1 || fail "a program importing std did not build with the platform's flag" build.log
grep -q 'Control-Flow Branch Protection' build.log && fail "the std module was refused for the flag" build.log
grep -Eq 'std\.cppm.*-fcf-protection=full|-fcf-protection=full.*std\.cppm' build.log \
    || grep -q 'fcf-protection=full' target/*/*/build.ninja 2>/dev/null \
    || fail "the std module's command does not name the platform's flag" build.log
"$MCPP" run > run.log 2>&1 || fail "the program did not run" run.log
grep -q "std with the platform's words: 3" run.log || fail "the program's output" run.log
echo "OK"
