#!/usr/bin/env bash
# 780_a_passing_check_moves_its_stamp_past_a_changed_input.sh — a `role =
# "check"` whose command writes no stamp, after one of its inputs changed.
#
# The engine writes such a check's stamp when the command succeeds (313). Until
# 2026.9.26.2 it only CREATED the stamp: an existing one was left alone. So once
# an input changed, the check ran and passed, and its stamp stayed older than
# that input -- and ninja ran it again on every build after, forever. Measured
# with mcpp-plugins' `deps-cmake`, whose check is a CMake build: every `mcpp
# build` after one edit re-ran configure, build and install.
#
# The criterion is the build AFTER the one that re-ran the check: it must not
# run the check again. Portable like 313: the command is the engine itself.
set -e

source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

mkdir -p src
cat > mcpp.toml <<'EOF'
[package]
name    = "stampmove"
version = "0.1.0"
EOF
cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::printf("STAMP_MOVE_OK\n"); }
EOF
echo one > input.txt

MCPP_HOST="$(host_path "$MCPP")"
INPUT_HOST="$(host_path "$TMP/input.txt")"
cat > build.mcpp <<EOF
import std;
import mcpp;
int main() {
    mcpp::action a;
    a.id          = "probe";
    a.role        = "check";
    a.description = "a check whose command writes no stamp";
    a.arg("$MCPP_HOST").arg("--version")
     .input("$INPUT_HOST")
     .output((std::string(mcpp::out_dir()) + "/probe.stamp").c_str())
     .submit();
}
EOF

# `-v` prints each edge ninja runs; the check's is the one that goes through
# the engine's stamp wrapper.
ran() { grep -q '__action-stamp' "$1"; }

"$MCPP" build -v > b1.log 2>&1 || { cat b1.log; echo "FAIL: build failed"; exit 1; }
ran b1.log || { cat b1.log; echo "FAIL: the first build did not run the check"; exit 1; }

"$MCPP" build -v > b2.log 2>&1 || { cat b2.log; echo "FAIL: no-op rebuild failed"; exit 1; }
if ran b2.log; then cat b2.log; echo "FAIL: the check re-ran with nothing changed"; exit 1; fi

# A changed input: the check runs once...
sleep 1
echo two > input.txt
"$MCPP" build -v > b3.log 2>&1 || { cat b3.log; echo "FAIL: rebuild after the edit failed"; exit 1; }
ran b3.log || { cat b3.log; echo "FAIL: a changed input did not re-run the check"; exit 1; }

# ...and not again: its stamp is now newer than the input.
"$MCPP" build -v > b4.log 2>&1 || { cat b4.log; echo "FAIL: rebuild after the check failed"; exit 1; }
if ran b4.log; then
    cat b4.log
    echo "FAIL: the check re-ran on the build after it passed -- its stamp stayed older than the input"
    exit 1
fi

out="$("$MCPP" run 2>&1 | tail -1)"
[[ "$out" == *STAMP_MOVE_OK* ]] || { echo "FAIL: the program did not run: '$out'"; exit 1; }
echo "OK"
