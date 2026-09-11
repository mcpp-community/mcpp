#!/usr/bin/env bash
# requires: windows
# 642_windows_subsystem.sh -- `windows_subsystem` / `windows_entry` (#618) with
# the native Windows toolchain, which links for the MSVC ABI. The assertions are
# shared with 643, which links the same project for the GNU ABI.
#
# The native half also measures the test binaries: `mcpp test` builds and runs
# them from the same package, and every executable other than the three that
# declare (or are given) the GUI subsystem must read 3.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

BUILD_ARGS=""
RUN_EXE="env"
source "$(dirname "$0")/_windows_subsystem_body.sh"

mkdir -p tests
printf 'int main() { return 0; }\n' > tests/smoke.cpp
"$MCPP" test > test.log 2>&1 || fail "mcpp test failed" test.log
checked=0
while IFS= read -r exe; do
    case "$(basename "$exe")" in gui.exe|tool.exe|winmain.exe) continue ;; esac
    got=$(pe_subsystem "$exe")
    [ "$got" = "3" ] || fail "$exe has Subsystem $got, expected 3" test.log
    checked=$((checked + 1))
done < <(find target -type f -name '*.exe')
# cli, wide, and at least one test binary.
[ "$checked" -ge 3 ] || fail "only $checked console executables were found" test.log
echo "console executables OK ($checked)"
