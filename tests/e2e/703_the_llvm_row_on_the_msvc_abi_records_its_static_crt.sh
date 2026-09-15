#!/usr/bin/env bash
# requires: windows
# 703 -- the C++ runtime record of clang on the MSVC ABI (#649 E10).
#
# mcpp emits a CRT model (`/MT` or `/MD`) only for cl.exe. clang on the MSVC ABI
# receives none, and its driver links the static CRT (`-defaultlib:libcmt`), so
# the program imports no vcruntime or ucrt DLL. The contract table was written
# for cl.exe and recorded `host-coupled` for that artifact; an explicit
# `cxx_runtime = "host-coupled"` was recorded and not delivered, with no word.
# The record now states `self-contained`, and the explicit request prints that
# the row does not deliver it. Nothing about the artifact changes.
#
# Read only when the default toolchain on this runner is the llvm row: a runner
# whose default is msvc@system prints that and asserts nothing, because the
# cl.exe cells are unchanged and covered elsewhere.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

cd "$TMP"
mkdir -p app/src
cat > app/src/main.cpp <<'CPP'
import std;
int main() { std::println("crt"); }
CPP
write_app() {   # $1 = [build] lines
    cat > app/mcpp.toml <<TOML
[package]
name    = "app"
version = "0.1.0"

[build]
$1
TOML
}

record_of() {   # prints the distributable contract from the one resolution.json
    tr -d ' \n\r' < "$(find target -name resolution.json | head -1)" \
        | grep -o '"distributable":"[a-z-]*"' | head -1 | cut -d'"' -f4
}

write_app ''
cd app
"$MCPP" build > default.log 2>&1 || fail "the default build failed" default.log
if ! grep -q "Resolved llvm@" default.log; then
    echo "READING E10: the default toolchain here is not the llvm row: $(grep -m1 'Resolved' default.log)"
    echo "PASS: 703 the llvm row on the MSVC ABI records its static CRT (not the llvm row; nothing to assert)"
    exit 0
fi
contract=$(record_of)
echo "READING E10 default record: distributable=$contract"
[ "$contract" = "self-contained" ] \
    || fail "the llvm row recorded '$contract' for a program linked with the static CRT" default.log

cd ..
write_app 'cxx_runtime = "host-coupled"'
cd app
rm -rf target
"$MCPP" build > host.log 2>&1 || fail "the host-coupled build failed" host.log
grep -q 'is not delivered for clang on the MSVC ABI' host.log \
    || fail "an explicit host-coupled request on the llvm row printed nothing" host.log
contract=$(record_of)
echo "READING E10 host-coupled record: distributable=$contract"
[ "$contract" = "self-contained" ] \
    || fail "the undelivered request was recorded as '$contract'" host.log
echo "ok: the llvm row records the static CRT it links, and says an explicit dynamic request is not delivered"

echo "PASS: 703 the llvm row on the MSVC ABI records its static CRT"
