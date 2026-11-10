#!/usr/bin/env bash
# requires:
# 1068 — a running test holds no build directory (mcpp#915).
#
# `mcpp test` releases the build directory's lock once its tests are built: a
# `mcpp build` and a second `mcpp test` of the same configuration proceed while
# a test runs, and the second test's protocol files are its own. Before, the
# lock was held until the run ended, and an editor's second test waited for
# the first ("Waiting for the build directory, held by pid ... mcpp test").
#
# The first test waits for a file that only the commands started beside it
# create; while the lock was held, they waited for it, and it for them, until
# the test's own limit.
set -e
source "$(dirname "$0")/_host_path.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# `timeout` is not on every host (macOS): the command, ended after SECS.
bounded() {
    local secs=$1; shift
    "$@" & local pid=$!
    ( sleep "$secs"; kill "$pid" 2>/dev/null ) & local guard=$!
    local rc=0
    wait "$pid" || rc=$?
    kill "$guard" 2>/dev/null; wait "$guard" 2>/dev/null || true
    return $rc
}
cd "$TMP"
"$MCPP" new app > /dev/null
cd app
rm -f tests/*.cpp
GATE="$(host_path "$TMP")/gate"
cat > tests/waits.cpp <<EOF
import std;
int main() {
    for (int i = 0; i < 600; ++i) {
        if (std::filesystem::exists("$GATE")) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::println("the gate never opened");
    return 1;
}
EOF
cat > tests/quick.cpp <<'EOF'
int main() { return 0; }
EOF
"$MCPP" test > "$TMP/warm.log" 2>&1 &
warm=$!
sleep 1; touch "$TMP/gate"; wait $warm || { cat "$TMP/warm.log"; echo "FAIL: the tests did not build"; exit 1; }
rm -f "$TMP/gate"

"$MCPP" test waits > "$TMP/first.log" 2>&1 &
first=$!
# The first test is running once its program has started; give it the time to
# be built (it is built already) and to start.
for _ in $(seq 1 100); do
    grep -q 'Running\|waits' "$TMP/first.log" 2>/dev/null && break
    sleep 0.1
done
sleep 1

# Beside it: a build of the same configuration, and a second test.
bounded 60 "$MCPP" build > "$TMP/build.log" 2>&1 || {
    touch "$TMP/gate"; wait $first || true
    cat "$TMP/build.log"; echo "FAIL: mcpp build waited for a running test"; exit 1; }
bounded 60 "$MCPP" test quick > "$TMP/second.log" 2>&1 || {
    touch "$TMP/gate"; wait $first || true
    cat "$TMP/second.log"; echo "FAIL: a second mcpp test waited for a running one"; exit 1; }
if grep -q 'Waiting for the build directory' "$TMP/build.log" "$TMP/second.log"; then
    touch "$TMP/gate"; wait $first || true
    cat "$TMP/build.log" "$TMP/second.log"; echo "FAIL: a command waited for the build directory"; exit 1
fi

touch "$TMP/gate"
wait $first || { cat "$TMP/first.log"; echo "FAIL: the first test failed"; exit 1; }
grep -q '1 passed; 0 failed' "$TMP/first.log" || { cat "$TMP/first.log"; echo "FAIL: the first test's summary"; exit 1; }
grep -q '1 passed; 0 failed' "$TMP/second.log" || { cat "$TMP/second.log"; echo "FAIL: the second test's summary"; exit 1; }

# The protocol files were the invocations', and went with them.
if ls -d target/*/*/test-protocol/*/ 2>/dev/null | grep -q .; then
    ls -R target/*/*/test-protocol; echo "FAIL: an ended invocation left its protocol directory"; exit 1
fi

# The issue's own shape: a test that builds its own project with mcpp. The
# inner build has the test's environment (the subos's `bin` first on PATH); it
# finds the directory free, the configuration the outer command built, and its
# graph current, so it neither waits nor plans.
MCPP_HOST="$(host_path "$(command -v "$MCPP" || echo "$MCPP")")"
INNER_LOG="$(host_path "$TMP")/inner.log"
cat > tests/inner.cpp <<EOF
#include <cstdlib>
#ifdef _WIN32
#define WHOLE(s) "\"" s "\""   // cmd /c drops the outer pair of quotes
#else
#define WHOLE(s) s
#endif
int main() { return std::system(WHOLE("\"$MCPP_HOST\" build > \"$INNER_LOG\" 2>&1")) == 0 ? 0 : 1; }
EOF
"$MCPP" test inner > "$TMP/prime.log" 2>&1 || { cat "$TMP/prime.log" "$TMP/inner.log"; echo "FAIL: a test that builds its project failed"; exit 1; }
bounded 120 "$MCPP" test inner > "$TMP/inner-test.log" 2>&1 || {
    cat "$TMP/inner-test.log" "$TMP/inner.log"; echo "FAIL: a test that builds its project failed"; exit 1; }
if grep -qE 'Waiting for the build directory|Resolving' "$TMP/inner.log"; then
    cat "$TMP/inner.log"; echo "FAIL: the inner build waited or planned"; exit 1
fi

echo "PASS: 1068 a running test holds no build directory"
