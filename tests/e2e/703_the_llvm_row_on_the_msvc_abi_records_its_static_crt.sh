#!/usr/bin/env bash
# requires: windows
# 703 -- the C++ runtime record of clang on the MSVC ABI (#649 E10, closed by
# #718). Kept at this number: the filename is now inexact (the row's DEFAULT
# is no longer the static CRT), and the fuller Windows leg of #718 lives at
# 814; this file keeps testing the one thing it always tested — what
# `resolution.json` records for the llvm row's C++ runtime contract.
#
# mcpp used to emit a CRT model (`/MT` or `/MD`) only for cl.exe. clang on the
# MSVC ABI received none, its driver linked the static CRT
# (`-defaultlib:libcmt`) regardless of what `cxx_runtime` said, and the table
# recorded `self-contained` for every request while an explicit `host-coupled`
# or `toolchain-coupled` printed that the row did not deliver it.
#
# Every MSVC-ABI row now receives the SAME model cl.exe does, spelled
# `-fms-runtime-lib=static`/`=dll` — one helper, `msvc_abi_crt_word` — reaching
# the compile line, the std/std.compat BMIs and the link command alike. The
# row's DEFAULT is therefore `toolchain-coupled` (the dynamic CRT, with the
# toolset's own vcruntime140.dll/msvcp140.dll staged beside the artifact),
# and an explicit `host-coupled` or `toolchain-coupled` is now delivered
# rather than degraded.
#
# Read only when the default toolchain on this runner is the llvm row: a
# runner whose default is msvc@system prints that and asserts nothing,
# because the cl.exe cells are unchanged and covered elsewhere.
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
    tr -d ' \n\r' < "$(find target -name resolution.json -not -path '*/.build-mcpp/*' | head -1)" \
        | grep -o '"distributable":"[a-z-]*"' | head -1 | cut -d'"' -f4
}

write_app ''
cd app
"$MCPP" build > default.log 2>&1 || fail "the default build failed" default.log
if ! grep -q "Resolved llvm@" default.log; then
    echo "READING #718: the default toolchain here is not the llvm row: $(grep -m1 'Resolved' default.log)"
    echo "PASS: 703 the llvm row on the MSVC ABI records its default CRT (not the llvm row; nothing to assert)"
    exit 0
fi
contract=$(record_of)
echo "READING #718 default record: distributable=$contract"
[ "$contract" = "toolchain-coupled" ] \
    || fail "the llvm row's undeclared default recorded '$contract', not toolchain-coupled" default.log

cd ..
write_app 'cxx_runtime = "host-coupled"'
cd app
rm -rf target
"$MCPP" build > host.log 2>&1 || fail "the host-coupled build failed" host.log
grep -q 'is not delivered for clang on the MSVC ABI' host.log \
    && { fail "an explicit host-coupled request still prints the old E10 message" host.log; } || true
contract=$(record_of)
echo "READING #718 host-coupled record: distributable=$contract"
[ "$contract" = "host-coupled" ] \
    || fail "an explicit host-coupled request recorded '$contract', not host-coupled" host.log

cd ..
write_app 'cxx_runtime = "self-contained"'
cd app
rm -rf target
"$MCPP" build > self.log 2>&1 || fail "the self-contained build failed" self.log
contract=$(record_of)
echo "READING #718 self-contained record: distributable=$contract"
[ "$contract" = "self-contained" ] \
    || fail "an explicit self-contained request recorded '$contract'" self.log

echo "ok: the llvm row's default is toolchain-coupled, and an explicit host-coupled or self-contained is delivered exactly as recorded"

echo "PASS: 703 the llvm row on the MSVC ABI records its default CRT"
