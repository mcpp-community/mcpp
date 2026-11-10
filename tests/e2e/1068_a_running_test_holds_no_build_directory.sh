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
timeout 60 "$MCPP" build > "$TMP/build.log" 2>&1 || {
    touch "$TMP/gate"; wait $first || true
    cat "$TMP/build.log"; echo "FAIL: mcpp build waited for a running test"; exit 1; }
timeout 60 "$MCPP" test quick > "$TMP/second.log" 2>&1 || {
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

echo "PASS: 1068 a running test holds no build directory"
