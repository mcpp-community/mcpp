#!/usr/bin/env bash
# requires:
# 671_toolchain_is_an_option_of_run_test_and_pack.sh -- `--toolchain` is
# declared on `run`, `test` and `pack` as it is on `build` (#634 A10).
#
# The value always reached the three commands: the pre-parse loop publishes it
# as MCPP_TOOLCHAIN for every command, and `MCPP_TOOLCHAIN=llvm@22.1.8 mcpp
# test` compiled with clang. The spelling the help of `build` teaches was
# refused by the option parser of the other three (2026.9.14.1):
#
#     error: unknown option: --toolchain
#
# Criteria:
#   1. each of `run`, `test` and `pack` accepts the option and hands its value
#      to toolchain resolution: an unknown family is refused by RESOLUTION,
#      naming the value, and not by the option parser;
#   2. positive direction: the toolchain this host resolves by default, named
#      with the option, builds and runs the program and passes the test;
#   3. with a second toolchain installed (llvm@22.1.8 while the default is
#      another family), `mcpp test --toolchain llvm@22.1.8` compiles the tests
#      with that compiler, read from the build graph it wrote. Reported as not
#      measured when the payload is absent, because installing it here would
#      make the criterion a download.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; shift; for f in "$@"; do echo "--- $f ---"; cat "$f" 2>/dev/null; done; exit 1; }

"$MCPP" new tcopt > /dev/null
cd tcopt

# ── 1. the option is accepted and its value reaches resolution ─────────────
for spelling in "run" "test" "pack --format dir"; do
    read -ra words <<<"$spelling"
    set +e
    "$MCPP" "${words[@]}" --toolchain nosuch@0.0.1 > c1.log 2>&1
    rc=$?
    set -e
    [ "$rc" -ne 0 ] || fail "'mcpp $spelling --toolchain nosuch@0.0.1' succeeded" c1.log
    grep -q "unknown option" c1.log \
        && fail "'mcpp $spelling' refused --toolchain as an unknown option" c1.log
    grep -q "unknown toolchain 'nosuch'" c1.log \
        || fail "'mcpp $spelling' did not hand the value to toolchain resolution" c1.log
    echo "mcpp $spelling --toolchain reaches resolution OK"
done

# ── 2. the default toolchain, named with the option, works ─────────────────
"$MCPP" build > b.log 2>&1 || fail "the initial build failed" b.log
own=$(sed -n 's/.*Resolved \([^ ]*\) .*/\1/p' b.log | head -1)
[ -n "$own" ] || fail "could not learn this host's toolchain from the build" b.log

"$MCPP" test --toolchain "$own" > t2.log 2>&1 || fail "mcpp test --toolchain $own failed" t2.log
grep -q "test result ok" t2.log || fail "mcpp test --toolchain $own did not pass" t2.log
"$MCPP" run --toolchain "$own" > r2.log 2>&1 || fail "mcpp run --toolchain $own failed" r2.log
grep -q "Hello from tcopt" r2.log || fail "mcpp run --toolchain $own did not run the program" r2.log
echo "the default toolchain named with --toolchain builds, runs and tests OK"

# ── 3. a second installed toolchain compiles the tests ────────────────────
store="${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs/xim-x-llvm/22.1.8/bin"
case "$own" in
    llvm@*)
        echo "NOT MEASURED: the default toolchain is already $own"
        ;;
    *)
        if [ -x "$store/clang++" ] || [ -x "$store/clang++.exe" ]; then
            "$MCPP" test --toolchain llvm@22.1.8 > t3.log 2>&1 \
                || fail "mcpp test --toolchain llvm@22.1.8 failed" t3.log
            grep -q "Resolved llvm@22.1.8" t3.log \
                || fail "the test build did not resolve llvm@22.1.8" t3.log
            grep -lq "xim-x-llvm/22.1.8/bin/clang++" target/*/*/build.ninja \
                || fail "no build graph compiles with the llvm payload's clang++" t3.log
            echo "mcpp test --toolchain llvm@22.1.8 compiles with clang OK"
        else
            echo "NOT MEASURED: llvm@22.1.8 is not installed in this home"
        fi
        ;;
esac

# ── 4. a spec the command line gave is refused as the command line's ─────
if "$MCPP" build --toolchain nosuch@1.0 > t4.log 2>&1; then
    fail "an unknown toolchain from --toolchain was accepted" t4.log
fi
grep -q "^error: --toolchain = 'nosuch@1.0'" t4.log \
    || fail "the refusal does not name --toolchain as where the value was written" t4.log
if grep -q "\[toolchain\]\." t4.log; then
    fail "the refusal credits a manifest key with a command-line value" t4.log
fi
echo "an unknown toolchain from --toolchain is refused naming --toolchain OK"

echo "PASS: 671_toolchain_is_an_option_of_run_test_and_pack"
