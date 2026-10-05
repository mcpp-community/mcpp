#!/usr/bin/env bash
# requires: windows
# 888 -- the LLVM row on the MSVC ABI in a Visual Studio developer
# environment, and `import std.compat` with the MSVC STL (2026.10.5.2).
#
# vcvars exports `WindowsSdkDir` with a trailing backslash. Rendered as
# `"…\Windows Kits\10\"`, the argument's closing quote was escaped under the
# Windows argv rules, every following argument shifted, and the std module
# precompile received `Files\Microsoft`, `Visual` and the rest of the command
# as inputs (measured on windows-latest). The same build outside a developer
# environment succeeded, which is why no runner saw it.
#
# Read only when the runner's default toolchain is the llvm row, as 703 is.
set -e

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }
MCPP="${MCPP:-mcpp}"

SDK_POSIX="/c/Program Files (x86)/Windows Kits/10"
if [ ! -d "$SDK_POSIX/Include" ]; then
    echo "PASS: 888 (no Windows SDK at the conventional root; nothing to assert)"
    exit 0
fi
export WindowsSdkDir='C:\Program Files (x86)\Windows Kits\10\'

cd "$TMP"
mkdir -p std/src compat/src
printf '[package]\nname = "s"\nversion = "0.1.0"\nstandard = "c++23"\n' > std/mcpp.toml
printf 'import std;\nint main() { std::println("std"); }\n' > std/src/main.cpp
printf '[package]\nname = "c"\nversion = "0.1.0"\nstandard = "c++23"\n' > compat/mcpp.toml
cat > compat/src/main.cpp <<'EOF'
import std.compat;
int main() {
    auto* memory = ::malloc(32);
    if (!memory) return 1;
    ::free(memory);
    std::vector<int> values{1, 2, 3};
    ::printf("compat=%zu\n", values.size());
    return values.size() == 3 ? 0 : 1;
}
EOF

(cd std && "$MCPP" run > ../std.log 2>&1) || fail "import std failed with WindowsSdkDir='$WindowsSdkDir'" std.log
if ! grep -q "Resolved llvm@" std.log; then
    echo "PASS: 888 (the default toolchain here is not the llvm row: $(grep -m1 'Resolved' std.log))"
    exit 0
fi
grep -q '^std' std.log || fail "the std program did not run" std.log
(cd compat && "$MCPP" run > ../compat.log 2>&1) || fail "import std.compat failed on the llvm row" compat.log
grep -q 'compat=3' compat.log || fail "the std.compat program did not run" compat.log

echo "PASS: 888 the llvm row in a developer environment"
